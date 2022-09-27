#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h> // TODO remove
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define arraylen(a) (sizeof(a) / sizeof(*a))
#define arrayend(a) (a + arraylen(a))
#define prob(p, t) __builtin_expect_with_probability(!!(t), 1, p)

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

struct io_uring_params params;
struct io_uring_cqe *cqe;
struct io_uring_sqe *sqe;
u8 *sq;

u32 *sqhead, *sqtail, *sqmask, *sqarray;
u32 *cqhead, *cqtail, *cqmask;
u32 nsubmit;

int ringfd;
int listenfd;

char response[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Length: 12\r\n"
	"\r\n"
	"Hello world\n";
char *responsepage;
u8 *buffers;

int files[256];

int
io_uring_setup(u32 entries, struct io_uring_params *params)
{
	return syscall(__NR_io_uring_setup, entries, params);
}

int
io_uring_register(int ringfd, unsigned opcode, void *arg, unsigned count)
{
	return syscall(__NR_io_uring_register, ringfd, opcode, arg, count);
}

int
io_uring_enter(int ringfd, unsigned nsubmit, unsigned mincomplete, unsigned flags, void *arg, unsigned arglen)
{
	return syscall(__NR_io_uring_enter, ringfd, nsubmit, mincomplete, flags, arg, arglen);
}

void
flush(void)
{
	int n;

	if (nsubmit == 0)
		return;

	n = io_uring_enter(ringfd, nsubmit, 0, IORING_ENTER_SQ_WAKEUP, 0, 0);

	if (n == -1)
		err(1, "io_uring_enter IORING_ENTER_SQ_WAKEUP");

	nsubmit -= n;
}

void
startaccept(int listenfd)
{
	u32 head, slot;

	for (;;)
	{
		head = atomic_load_explicit((atomic_uint *) sqhead, memory_order_acquire);

		if (head+1 != *sqtail)
			break;

		flush();
	}

	slot = *sqtail & *sqmask;

	sqarray[slot] = slot;

	sqe[slot] = (struct io_uring_sqe)
	{
		.opcode		= IORING_OP_ACCEPT,
		.flags		= IOSQE_ASYNC,
		.fd		= listenfd,
		.user_data	= ~0ull,
		.accept_flags	= SOCK_CLOEXEC|SOCK_NONBLOCK,
	};

	atomic_store_explicit((atomic_uint *) sqtail, *sqtail+1, memory_order_release);
	nsubmit += 1;
}

void
writeread(int fd, void *buf, u32 len)
{
	u32 slot0, slot1;

	slot0 = (*sqtail+0) & *sqmask;
	slot1 = (*sqtail+1) & *sqmask;

	sqarray[slot0] = slot0;
	sqarray[slot1] = slot1;

	sqe[slot0] = (struct io_uring_sqe)
	{
		.opcode		= IORING_OP_WRITE_FIXED,
		.fd		= fd,
		.len		= len,
		.addr		= (u64) buf,
		.flags		= IOSQE_IO_LINK,
		.user_data	= 1337,
		.buf_index	= 0,
	};

	sqe[slot1] = (struct io_uring_sqe)
	{
		.opcode		= IORING_OP_READ_FIXED,
		.fd		= fd,
		.len		= 4096,
		.addr		= (u64) &buffers[4096],
		.user_data	= fd,
		.buf_index	= 0,
	};

	atomic_store_explicit((atomic_uint *) sqtail, *sqtail+2, memory_order_release);

	nsubmit += 2;
}

int
readrequest(int fd, void *buf, size_t len)
{
	u64 v;

	// Errors or EOF cannot really happen at this point. Because of
	// TCP_DEFER_ACCEPT there must be some bytes to read, unless the
	// peer already went away. A peer that sends fewer bytes than is
	// reasonable is treated with prejudice.
	if (prob(.001, len <= sizeof("GET / HTTP/1.0\r\n")-1))
		return 0;

	__builtin_memcpy(&v, buf, 8);

	if (prob(.95, v == 0x5448202F20544547ull)) // "GET / HT"
	{
		v = 0;
		__builtin_memcpy(&v, buf+len-4, 4);

		if (prob(.99, v == 0x0A0D0A0D)) // "\r\n\r\n"
		{
			// Common case: GET request with complete headers.
			writeread(fd, responsepage, sizeof(response)-1);
			return 1;
		}

		// We're in one of two scenarios:
		// 1. Partial GET request
		// 2. GET request with body
	}

	return 0;
}

void
io(void)
{
	struct io_uring_cqe e;
	u32 head;
	int fd;
	int n;

	startaccept(listenfd);

	for (;;)
	{
		// TODO only set IORING_ENTER_SQ_WAKEUP when nsubmit > 0?
		do
			n = io_uring_enter(ringfd, nsubmit, 1, IORING_ENTER_GETEVENTS|IORING_ENTER_SQ_WAKEUP, 0, 0);
		while (n == -1 && errno == EINTR);

		if (n == -1)
			err(1, "io_uring_enter IORING_ENTER_GETEVENTS");

		nsubmit -= n;

		for (;;)
		{
			head = atomic_load_explicit((atomic_uint *) cqhead, memory_order_acquire);

			if (head == *cqtail)
				break;

			e = cqe[head & *cqmask];

			atomic_store_explicit((atomic_uint *) cqhead, head+1, memory_order_release);

			if (e.res < 0)
				continue;

			if (e.user_data == 1337)
				continue;

			if (e.user_data == ~0ull)
			{
				startaccept(listenfd);

				fd = e.res;
				do
					n = read(fd, &buffers[4096], 4096);
				while (n == -1 && errno == EINTR);
			}
			else
			{
				fd = (int) e.user_data;
				n = e.res;
			}

			if (n <= 0)
			{
				close(fd);
				continue;
			}

			if (!readrequest(fd, &buffers[4096], n))
				close(fd);
		}
	}
}

int
main(void)
{
	struct sockaddr_in sin;
	int on;
	int i;

	for (i = 0; i < (int) arraylen(files); i++)
		files[i] = -1;

	params = (struct io_uring_params)
	{
		.flags		= IORING_SETUP_SQPOLL,	// needs CAP_SYS_NICE?
		.sq_thread_idle	= 1,			// milliseconds
	};

	ringfd = io_uring_setup(32, &params);
	if (ringfd == -1)
		err(1, "io_uring_setup");

	// TODO add workaround
	if (0 == (params.features & IORING_FEAT_SQPOLL_NONFIXED))
		errx(1, "kernel too old"); // requires 5.11+

	sq = mmap(
		0, params.sq_off.array + params.sq_entries * sizeof(u32),
		PROT_READ|PROT_WRITE,
		MAP_SHARED|MAP_POPULATE,
		ringfd, IORING_OFF_SQ_RING
	);
	u8 *cq = mmap(
		0, params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe),
		PROT_READ|PROT_WRITE,
		MAP_SHARED|MAP_POPULATE,
		ringfd, IORING_OFF_CQ_RING
	);
	sqe = mmap(
		0, params.sq_entries * sizeof(struct io_uring_sqe),
		PROT_READ|PROT_WRITE,
		MAP_SHARED|MAP_POPULATE,
		ringfd, IORING_OFF_SQES
	);
	if (sq == MAP_FAILED || cq == MAP_FAILED || sqe == MAP_FAILED)
		err(1, "mmap");

	sqhead = (u32 *) (sq + params.sq_off.head);
	sqtail = (u32 *) (sq + params.sq_off.tail);
	sqmask = (u32 *) (sq + params.sq_off.ring_mask);
	sqarray = (u32 *) (sq + params.sq_off.array);

	cqhead = (u32 *) (cq + params.cq_off.head);
	cqtail = (u32 *) (cq + params.cq_off.tail);
	cqmask = (u32 *) (cq + params.cq_off.ring_mask);
	cqe = (struct io_uring_cqe *) (cq + params.cq_off.cqes);

	buffers = mmap(
		0, 32768,
		PROT_READ|PROT_WRITE,
		MAP_ANONYMOUS|MAP_PRIVATE,
		-1, 0
	);
	if (buffers == MAP_FAILED)
		err(1, "mmap");

	responsepage = memcpy(buffers, response, sizeof(response)-1);

	struct iovec buf = (struct iovec)
	{
		.iov_base	= buffers,
		.iov_len	= 32768,
	};

	if (io_uring_register(ringfd, IORING_REGISTER_BUFFERS, &buf, 1))
		err(1, "io_uring_register IORING_REGISTER_BUFFERS");

	if (io_uring_register(ringfd, IORING_REGISTER_FILES, files, arraylen(files)))
		err(1, "io_uring_register IORING_REGISTER_FILES");

	listenfd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0);
	if (listenfd == -1)
		err(1, "socket");

	sin = (struct sockaddr_in)
	{
		.sin_family = AF_INET,
		.sin_addr = (struct in_addr){ htonl(INADDR_ANY) },
		.sin_port = htons(9000),
	};

	on = 1;
	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)))
		err(1, "setsockopt SO_REUSEADDR");

	if (bind(listenfd, (struct sockaddr *) &sin, sizeof(sin)))
		err(1, "bind");

	if (listen(listenfd, 128))
		err(1, "listen");

	i = 1; // inherited by accepted sockets
	if (setsockopt(listenfd, IPPROTO_TCP, TCP_NODELAY, &i, sizeof(i)))
		err(1, "setsockopt TCP_NODELAY");

	i = 10; // seconds
	if (setsockopt(listenfd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &i, sizeof(i)))
		err(1, "setsockopt TCP_DEFER_ACCEPT");

	io();

	return 0;
}
