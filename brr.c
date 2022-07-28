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

volatile int cbrk;
int numthreads;
int edge;

char response[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Length: 2\r\n"
	"\r\n"
	"OK";

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
		return 0;

	__builtin_memcpy(&v, b, 8);

	if (prob(.95, v == 0x5448202F20544547ull)) // "GET / HT"
	{
		v = 0;
		__builtin_memcpy(&v, b+n-4, 4);

		if (prob(.99, v == 0x0A0D0A0D)) // "\r\n\r\n"
		{
			// Common case: GET request with complete headers.
			n = write(fd, response, sizeof(response)-1);
			return 1;
		}

		// We're in one of two scenarios:
		// 1. Partial GET request
		// 2. GET request with body
	}

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
					if (edge)
						e.events |= EPOLLET;
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

int
main(int argc, char **argv)
{
	struct sockaddr_in sin;
	int svfd;
	int opt;
	int on;
	int i;
	thrd_t *t;

	for (;;)
	{
		opt = getopt(argc, argv, "et:");
		if (opt == -1)
			break;
		switch (opt)
		{
			case 'e':
				edge = 1; // edge-triggered i/o
				break;
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
			if (thrd_create(t+i, startio, &svfd))
				errx(1, "thrd_create error");
	}

	io(svfd);

	return 0;
}
