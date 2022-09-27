#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h> // TODO remove
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define arraylen(a) (sizeof(a) / sizeof(*a))
#define arrayend(a) (a + arraylen(a))
#define prob(p, t) __builtin_expect_with_probability(!!(t), 1, p)

enum
{
	READABLE	= 1<<0,
	WRITING		= 1<<1,
};

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

int evtfd;
int ringfd;
int listenfd;

u8 fdstate[8192];

char response[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Length: 12\r\n"
	"\r\n"
	"Hello world\n";
char *responsepage;
u32 response_buf_index;

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

int
push(struct io_uring_sqe *e)
{
	u32 head, tail, slot;

	head = atomic_load_explicit((atomic_uint *) sqhead, memory_order_acquire);
	tail = *sqtail;

	if (head+1 == tail)
		return 0;

	slot = tail & *sqmask;
	sqe[slot] = *e;
	sqarray[slot] = slot;

	atomic_store_explicit((atomic_uint *) sqtail, tail+1, memory_order_release);

	return 1;
}

int
pop(struct io_uring_cqe *e)
{
	u32 head;

	head = atomic_load_explicit((atomic_uint *) cqhead, memory_order_acquire);
	if (head == *cqtail)
		return 0;

	*e = cqe[head & *cqmask];
	atomic_store_explicit((atomic_uint *) cqhead, head+1, memory_order_release);

	return 1;
}

void
send_response(int fd, void *buf, u32 len)
{
	struct io_uring_sqe e;
	int n;

	e = (struct io_uring_sqe)
	{
		.opcode		= IORING_OP_WRITE_FIXED,
		.fd		= fd,
		.len		= len,
		.addr		= (u64) buf,
		.user_data	= fd,
		.buf_index	= response_buf_index,
	};

	while (!push(&e))
	{
		n = io_uring_enter(ringfd, nsubmit, 0, IORING_ENTER_SQ_WAIT, 0, 0);
		if (n == -1)
			err(1, "io_uring_enter IORING_ENTER_SQ_WAIT");
		nsubmit -= n;
	}

	nsubmit++;
}

int
readrequest(int fd)
{
	_Alignas(64) u8 b[4096]; // on cache line
	long n;
	u64 v;

	do
		n = read(fd, b, sizeof(b));
	while (n == -1 && errno == EINTR);

	if (n == -1)
		return errno == EAGAIN; // TODO clear READABLE flag or not?

	// Errors or EOF cannot really happen at this point. Because of
	// TCP_DEFER_ACCEPT there must be some bytes to read, unless the
	// peer already went away. A peer that sends fewer bytes than is
	// reasonable is treated with prejudice.
	if (prob(.001, n <= (long) sizeof("GET / HTTP/1.0\r\n")-1))
		return 0;

	if (prob(.001, n == (long) sizeof(b)))
		fdstate[fd] &= ~READABLE;

	__builtin_memcpy(&v, b, 8);

	if (prob(.95, v == 0x5448202F20544547ull)) // "GET / HT"
	{
		v = 0;
		__builtin_memcpy(&v, b+n-4, 4);

		if (prob(.99, v == 0x0A0D0A0D)) // "\r\n\r\n"
		{
			// Common case: GET request with complete headers.
			send_response(fd, responsepage, sizeof(response)-1);
			fdstate[fd] |= WRITING;
			return 1;
		}

		// We're in one of two scenarios:
		// 1. Partial GET request
		// 2. GET request with body
	}

	return 0;
}

int
acceptnew(int listenfd)
{
	int fd;

	fd = accept4(listenfd, 0, 0, SOCK_CLOEXEC|SOCK_NONBLOCK);
	if (prob(.01, fd == -1))
	{
		switch (errno)
		{
			case EAGAIN:
				break; // preempted by other thread
			case ECONNABORTED:
				break; // peer went away
			case EMFILE:
			case ENFILE:
				err(1, "accept"); // TODO handle
			case ENOBUFS:
			case ENOMEM:
				err(1, "accept"); // TODO handle
			default:
				err(1, "accept"); // fatal
		}
		return -1;
	}

	if (prob(.0001, fd >= (int) arraylen(fdstate)))
		errx(1, "too many file descriptors");

	fdstate[fd] = READABLE;

	if (readrequest(fd))
		return fd;

	close(fd);
	return -1;
}

void
io(void)
{
	struct epoll_event events[64];
	struct epoll_event e;
	int timeout;
	int i, n;
	int epfd;
	int fd;

	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd == -1)
		err(1, "epoll_create1");

	e = (struct epoll_event)
	{
		.events = EPOLLIN,
		.data.fd = evtfd,
	};

	if (epoll_ctl(epfd, EPOLL_CTL_ADD, evtfd, &e))
		err(1, "epoll_ctl EPOLL_CTL_ADD");

	e = (struct epoll_event)
	{
		.events = EPOLLIN,
		.data.fd = listenfd,
	};

	if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &e))
		err(1, "epoll_ctl EPOLL_CTL_ADD");

	for (;;)
	{
		if (nsubmit > 0)
		{
			n = io_uring_enter(ringfd, nsubmit, 0, IORING_ENTER_SQ_WAKEUP, 0, 0);
			if (n == -1)
				err(1, "io_uring_enter IORING_ENTER_SQ_WAKEUP");
			nsubmit -= n;
		}

		timeout = -1;

		n = epoll_wait(epfd, events, arraylen(events), timeout);

		for (i = 0; i < n; i++)
		{
			fd = events[i].data.fd;

			if (prob(.1, fd == listenfd))
			{
				fd = acceptnew(listenfd);
				if (fd != -1)
				{
					e = (struct epoll_event)
					{
						.events = EPOLLIN|EPOLLET,
						.data.fd = fd,
					};
					if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e))
						err(1, "epoll_ctl EPOLL_CTL_ADD");
				}
				continue;
			}

			if (prob(.1, fd == evtfd))
			{
				struct io_uring_cqe e;
				u64 data;
				int fd;
				int n;

				do
					n = (int) read(evtfd, &data, sizeof(data));
				while (n == -1 && errno == EINTR);

				while (pop(&e))
				{
					fd = e.user_data;
					fdstate[fd] &= ~WRITING;
					if (fdstate[fd] & READABLE)
						if (!readrequest(fd))
							close(fd);
				}

				continue;
			}

			if (fdstate[fd] & WRITING)
			{
				fdstate[fd] |= READABLE;
				continue;
			}

			if (!readrequest(fd))
				close(fd);
		}
	}

}

int
startio(void *unused)
{
	(void) &unused;
	io();
	return 0;
}

int
main(void)
{
	struct sockaddr_in sin;
	int on;
	int i;

	params = (struct io_uring_params)
	{
		.flags		= IORING_SETUP_SQPOLL,	// needs CAP_SYS_NICE?
		.sq_thread_idle	= 1,			// milliseconds
	};

	evtfd = eventfd(0, EFD_CLOEXEC|EFD_NONBLOCK);
	if (evtfd == -1)
		err(1, "eventfd");

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

	if (io_uring_register(ringfd, IORING_REGISTER_EVENTFD, &evtfd, 1))
		err(1, "io_uring_register IORING_REGISTER_EVENTFD");

	responsepage = mmap(
		0, sizeof(response)-1,
		PROT_READ|PROT_WRITE,
		MAP_ANONYMOUS|MAP_PRIVATE,
		-1, 0
	);
	if (responsepage == MAP_FAILED)
		err(1, "mmap");

	memcpy(responsepage, response, sizeof(response)-1);

	struct iovec buf = (struct iovec)
	{
		.iov_base	= responsepage,
		.iov_len	= sizeof(response)-1,
	};

	if (io_uring_register(ringfd, IORING_REGISTER_BUFFERS, &buf, 1))
		err(1, "io_uring_register IORING_REGISTER_BUFFERS");

	listenfd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
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
