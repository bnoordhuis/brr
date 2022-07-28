#define _GNU_SOURCE
#include <stdio.h> // TODO remove
#include <signal.h> // TODO remove
#include <err.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <threads.h>
#include <unistd.h>

#define arraylen(a) (sizeof(a) / sizeof(*a))
#define arrayend(a) (a + arraylen(a))
#define prob(p, t) __builtin_expect_with_probability(!!(t), 1, p)

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef unsigned int uint;

#define count(name)				\
	do					\
	{					\
		static struct counter counter;	\
		docount(&counter, name);	\
	}					\
	while (0)

struct counter
{
	u64 n;
	char *name;
	struct counter *next;
};

struct counter *counters;

volatile int cbrk;

char response[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Length: 2\r\n"
	"\r\n"
	"OK";

void
docount(struct counter *c, char *name)
{
	atomic_uintptr_t *head;
	atomic_uintptr_t next;
	uint n;

	n = atomic_fetch_add_explicit((_Atomic u64 *) &c->n, 1, memory_order_relaxed);
	if (prob(.999, n > 0))
		return;

	// TODO Weaken memory_order_seq_cst. However, this is the uncommon path
	// and therefore probably not worth optimizing
	head = (atomic_uintptr_t *) &counters;
	for (;;)
	{
		next = atomic_load_explicit(head, memory_order_seq_cst);

		atomic_store_explicit(
			(atomic_uintptr_t *) &c->name, (uintptr_t) name,
			memory_order_seq_cst);

		atomic_store_explicit(
			(atomic_uintptr_t *) &c->next, next,
			memory_order_seq_cst);

		if (atomic_compare_exchange_weak_explicit(
				head, &next, (atomic_uintptr_t) c,
				memory_order_seq_cst, memory_order_seq_cst))
		{
			return;
		}
	}
}

void
printcounters(void)
{
	atomic_uintptr_t *p;
	struct counter *c;

	p = (atomic_uintptr_t *) &counters;
	for (;;)
	{
		c = (struct counter *) atomic_load_explicit(p, memory_order_seq_cst);
		if (!c)
			return;
		p = (atomic_uintptr_t *) &c->next;
		printf("%lu %s\n", c->n, c->name);
	}
}

void
handlecbrk(int nr)
{
	(void) &nr;
	cbrk = 1;
}

int
readrequest(int fd)
{
	_Alignas(64) u8 b[4096]; // on cache line
	long n;
	u64 v;

	n = read(fd, b, sizeof(b));

	// Errors or EOF cannot really happen at this point. Because of
	// TCP_DEFER_ACCEPT there must be some bytes to read, unless the
	// peer already went away. A peer that sends fewer bytes than is
	// reasonable is treated with prejudice.
	if (prob(.001, n <= (int) sizeof("GET / HTTP/1.0\r\n")-1))
	{
		if (n < 0)
		{
			if (errno == EAGAIN)
				count("EAGAIN");
			else if (errno == ECONNRESET)
				count("ECONNRESET");
			else
				count("read error");
		}
		if (n == 0)
			count("read eof");
		if (n > 0)
			count("short read");
		return 0;
	}

	count("incoming request");

	__builtin_memcpy(&v, b, 8);

	if (prob(.95, v == 0x5448202F20544547ull)) // "GET / HT"
	{
		v = 0;
		__builtin_memcpy(&v, b+n-4, 4);

		if (prob(.99, v == 0x0A0D0A0D)) // "\r\n\r\n"
		{
			// Common case: GET request with complete headers.
			n = write(fd, response, sizeof(response)-1);
			if (n == -1)
				count("write error");
			else if (n != (int) sizeof(response)-1)
				count("short write");
			return 1;
		}

		// We're in one of two scenarios:
		// 1. Partial GET request
		// 2. GET request with body
	}

	count("dropped request");

	return 0;
}

int
acceptnew(int svfd)
{
	int fd;

	fd = accept4(svfd, 0, 0, SOCK_CLOEXEC|SOCK_NONBLOCK);
	if (prob(.01, fd == -1))
	{
		switch (errno)
		{
			case EAGAIN:
				count("EAGAIN");
				break; // preempted by other thread
			case ECONNABORTED:
				count("ECONNABORTED");
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

	if (readrequest(fd))
		return fd;

	close(fd);
	return -1;
}

void
io(int svfd)
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
		.events = EPOLLIN|EPOLLEXCLUSIVE,
		.data.fd = svfd,
	};

	if (epoll_ctl(epfd, EPOLL_CTL_ADD, svfd, &e))
		err(1, "epoll_ctl EPOLL_CTL_ADD");

	while (!cbrk)
	{
		timeout = -1;

		n = epoll_wait(epfd, events, arraylen(events), timeout);

		for (i = 0; i < n; i++)
		{
			fd = events[i].data.fd;

			if (fd == svfd)
			{
				fd = acceptnew(svfd);
				if (fd != -1)
				{
					e = (struct epoll_event)
					{
						.events = EPOLLIN,
						.data.fd = fd,
					};
					if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e))
						err(1, "epoll_ctl EPOLL_CTL_ADD");
				}
			}
			else
			{
				if (!readrequest(fd))
					close(fd);
			}
		}
	}

}

int
startio(void *arg)
{
	int *svfd = arg;
	io(*svfd);
	return 0;
}

void
serve(int numthreads, int svfd)
{
	thrd_t *t;
	int i;

	signal(SIGINT, handlecbrk);

	i = 1; // inherited by accepted sockets
	if (setsockopt(svfd, IPPROTO_TCP, TCP_NODELAY, &i, sizeof(i)))
		err(1, "setsockopt TCP_NODELAY");

	i = 10; // seconds
	if (setsockopt(svfd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &i, sizeof(i)))
		err(1, "setsockopt TCP_DEFER_ACCEPT");

	if (numthreads > 1)
	{
		t = malloc(numthreads * sizeof(*t));
		if (t == 0)
			err(1, "malloc");
		for (i = 1; i < numthreads; i++)
		{
			if (thrd_create(t+i, startio, &svfd))
				errx(1, "thrd_create error");
		}
	}

	io(svfd);

	printf("\n");
	printcounters();
}

int
main(int argc, char **argv)
{
	struct sockaddr_in sin;
	int numthreads;
	int svfd;
	int opt;
	int on;

	numthreads = 0;
	for (;;)
	{
		opt = getopt(argc, argv, "t:");
		if (opt == -1)
			break;
		switch (opt)
		{
			case 't':
				numthreads = atoi(optarg);
				break;
		}
	}

	svfd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
	if (svfd == -1)
		err(1, "socket");

	sin = (struct sockaddr_in)
	{
		.sin_family = AF_INET,
		.sin_addr = (struct in_addr){ htonl(INADDR_ANY) },
		.sin_port = htons(9000),
	};

	on = 1;
	if (setsockopt(svfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)))
		err(1, "setsockopt SO_REUSEADDR");

	if (bind(svfd, (struct sockaddr *) &sin, sizeof(sin)))
		err(1, "bind");

	if (listen(svfd, 128))
		err(1, "listen");

	serve(numthreads, svfd);

	return 0;
}