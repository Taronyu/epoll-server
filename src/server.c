/*
This file is part of epoll-server.

epoll-server is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

epoll-server is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with epoll-server. If not, see <http://www.gnu.org/licenses/>.
*/

#include "server.h"
#include <assert.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#define CLIENT_BUF_SIZE 2048

/* Client instance */
struct client
{
	/* Client buffer */
	char buffer[CLIENT_BUF_SIZE];
	/* Remote IP address */
	char addr[INET_ADDRSTRLEN];
	/* Server instance */
	struct server *srv;
	/* Socket */
	int sd;

	struct client *next;
	struct client *prev;
};

/* Server instance */
struct server
{
	/* Server event handler */
	const struct srv_handler *handler;
	/* Connected clients list */
	struct client *clients;
	/* Server socket */
	int sd;
	/* Stop server flag */
	volatile sig_atomic_t shouldQuit;
};

/*
 * Remove a client from the clients list and free its resources.
 */
static void cl_free(struct client *cl)
{
	if (cl == NULL)
	{
		return;
	}

	/* Remove client from the list */
	if (cl != cl->next)
	{
		cl->next->prev = cl->prev;
		cl->prev->next = cl->next;
	}
	else
	{
		/* Last client removed */
		cl->srv->clients = NULL;
	}

	/*
	 * Close the socket if necessary. Closing the socket will also remove it
	 * from the epoll list (see epoll manpage for details).
	 */
	if (cl->sd > -1)
	{
		if (shutdown(cl->sd, SHUT_RDWR) == -1)
		{
			perror("shutdown");
		}

		if (close(cl->sd) == -1)
		{
			perror("close");
		}
	}

	free(cl);
}

/*
 * Create a new client instance and add it to the connected clients list.
 * Returns NULL on failure.
 */
static struct client *cl_create(struct server *srv, int sd,
	const struct sockaddr *addr)
{
	struct client *cl;

	assert(srv != NULL);
	assert(addr != NULL);

	cl = malloc(sizeof(struct client));
	if (cl == NULL)
	{
		fprintf(stderr, "Failed to create client instance: out of memory.\n");
		return NULL;
	}

	memset(cl, 0, sizeof(struct client));
	cl->srv = srv;
	cl->sd = sd;

	/* Get remote IP */
	if (addr->sa_family == AF_INET)
	{
		inet_ntop(AF_INET, &((struct sockaddr_in*)addr)->sin_addr,
			cl->addr, INET_ADDRSTRLEN);
	}
	else
	{
		cl->addr[0] = '\0';
	}

	/* Add client to clients list */
	if (srv->clients != NULL)
	{
		cl->next = srv->clients;
		cl->prev = srv->clients->prev;
		srv->clients->prev->next = cl;
		srv->clients->prev = cl;
	}
	else
	{
		cl->next = cl->prev = cl;
		srv->clients = cl;
	}

	return cl;
}

/*
 * Raise the server start event.
 */
static void srv_onStart(struct server *srv)
{
	const struct srv_handler *h;

	assert(srv != NULL);

	h = srv->handler;
	if (h != NULL && h->on_start != NULL)
	{
		h->on_start();
	}
}

/*
 * Raise the server stop event.
 */
static void srv_onStop(struct server *srv)
{
	const struct srv_handler *h;

	assert(srv != NULL);

	h = srv->handler;
	if (h != NULL && h->on_stop != NULL)
	{
		h->on_stop();
	}
}

/*
 * Raise the client connect event.
 */
static void srv_onConnect(struct client *cl)
{
	const struct srv_handler *h;

	assert(cl != NULL);

	h = cl->srv->handler;
	if (h != NULL && h->on_connect != NULL)
	{
		h->on_connect(cl->addr);
	}
}

/*
 * Raise the client disconnect event.
 */
static void srv_onDisconnect(struct client *cl)
{
	const struct srv_handler *h;

	assert(cl != NULL);

	h = cl->srv->handler;
	if (h != NULL && h->on_disconnect != NULL)
	{
		h->on_disconnect(cl->addr);
	}
}

/*
 * Raise the client data receive event.
 */
static void srv_onReceive(struct client *cl, const char *buf, ssize_t len)
{
	const struct srv_handler *h;

	assert(cl != NULL);

	/* Echo data back to client */
	if (write(cl->sd, buf, len) != len)
	{
		fprintf(stderr, "Failed to write response data.\n");
	}

	h = cl->srv->handler;
	if (h != NULL && h->on_receive != NULL)
	{
		h->on_receive(cl->addr, buf, len);
	}
}

/*
 * Set a socket descriptor to use non-blocking IO.
 * Returns 0 on success, -1 on failure.
 */
static int srv_setNonBlocking(int sd)
{
	int flags;

	flags = fcntl(sd, F_GETFL, 0);
	if (flags == -1)
	{
		goto on_error;
	}

	flags |= O_NONBLOCK;
	if (fcntl(sd, F_SETFL, flags) == -1)
	{
		goto on_error;
	}

	return 0;

on_error:
	perror("fcntl");
	return -1;
}

/*
 * Create and bind a new TCP server socket on the given port.
 * Returns socket descriptor on success, -1 on failure.
 */
static int srv_createAndBind(int port)
{
	int sd;
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);

	/* Create a TCP socket */
	sd = socket(AF_INET, SOCK_STREAM, 0);
	if (sd == -1)
	{
		perror("socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	if (bind(sd, (struct sockaddr*)&addr, addrlen) == -1)
	{
		perror("bind");
		close(sd);
		return -1;
	}

	return sd;
}

/*
 * Remove all clients from the connected clients list.
 */
static void srv_freeAllClients(struct server *srv)
{
	assert(srv != NULL);

	while (srv->clients != NULL)
	{
		cl_free(srv->clients);
	}
}

/*
 * Handle epoll error events.
 */
static void srv_handleError(const struct epoll_event *ev)
{
	struct client *cl;

	fprintf(stderr, "Connection lost or epoll error.\n");

	cl = ev->data.ptr;

	srv_onDisconnect(cl);
	cl_free(cl);
}

/*
 * Handle accept events.
 */
static void srv_handleAccept(int efd, const struct epoll_event *ev)
{
	struct epoll_event eev;
	struct sockaddr addr;
	socklen_t addrlen = sizeof(addr);
	int sd;
	struct server *srv;
	struct client *cl = NULL;

	srv = ev->data.ptr;

	sd = accept(srv->sd, &addr, &addrlen);
	if (sd == -1)
	{
		perror("accept");
		return;
	}

	if (srv_setNonBlocking(sd) != 0)
	{
		goto on_error;
	}

	cl = cl_create(srv, sd, &addr);
	if (cl == NULL)
	{
		goto on_error;
	}

	memset(&eev, 0, sizeof(eev));
	eev.data.ptr = cl;
	eev.events = EPOLLIN | EPOLLET;

	if (epoll_ctl(efd, EPOLL_CTL_ADD, sd, &eev) == -1)
	{
		perror("epoll_ctl");
		goto on_error;
	}

	srv_onConnect(cl);
	return;

on_error:
	cl_free(cl);
	close(sd);
}

/*
 * Handle data receive events.
 */
static void srv_handleReceive(const struct epoll_event *ev)
{
	struct client *cl;
	int done = 0;

	cl = ev->data.ptr;

	/*
	 * We're running in edge triggered mode, i.e. we get notified only once
	 * when data is available. Therefore we must read all available data at
	 * once.
	 */
	while (1)
	{
		ssize_t len;

		len = read(cl->sd, cl->buffer, CLIENT_BUF_SIZE);
		if (len == -1)
		{
			if (errno != EAGAIN)
			{
				perror("read");
				done = 1;
			}

			break;
		}
		else if (len == 0)
		{
			done = 1;
			break;
		}
		else
		{
			srv_onReceive(cl, cl->buffer, len);
		}
	}

	if (done != 0)
	{
		/* Remove client */
		srv_onDisconnect(cl);
		cl_free(cl);
	}
}

static void srv_eventLoop(struct server *srv, int queueSize)
{
	struct epoll_event eev;
	struct epoll_event *events = NULL;
	int efd;

	assert(srv != NULL);

	/* Create epoll file descriptor */
	efd = epoll_create1(0);
	if (efd == -1)
	{
		perror("epoll_create1");
		return;
	}

	/* Register server socket */
	memset(&eev, 0, sizeof(eev));
	eev.data.ptr = srv;
	eev.events = EPOLLIN | EPOLLET;

	if (epoll_ctl(efd, EPOLL_CTL_ADD, srv->sd, &eev) == -1)
	{
		perror("epoll_ctl");
		goto on_exit;
	}

	/* Create event queue */
	events = calloc(queueSize, sizeof(struct epoll_event));
	if (events == NULL)
	{
		fprintf(stderr, "Failed to create event queue: out of memory.\n");
		goto on_exit;
	}

	srv->shouldQuit = 0;

	/* Event loop */
	while (srv->shouldQuit == 0)
	{
		int n, i;

		/* Wait for epoll events */
		n = epoll_wait(efd, events, queueSize, -1);

		/* Alternative: check if interrupted */
		/*
		if (n == -1 && errno == EINTR)
		{
			break;
		}
		*/

		/* Process events */
		for (i = 0; i < n; ++i)
		{
			struct epoll_event *ev = &events[i];

			if ((ev->events & EPOLLERR) ||
				(ev->events & EPOLLHUP) ||
				!(ev->events & EPOLLIN))
			{
				srv_handleError(ev);
			}
			else if (ev->data.ptr == srv)
			{
				srv_handleAccept(efd, ev);
			}
			else
			{
				srv_handleReceive(ev);
			}
		}
	}

on_exit:
	/* Cleanup */
	free(events);
	close(efd);
}

int srv_setHandler(struct server *srv, const struct srv_handler *h)
{
	if (srv != NULL)
	{
		srv->handler = h;
		return 0;
	}
	else
	{
		return -1;
	}
}

struct server *srv_create(const struct srv_handler *h)
{
	struct server *srv;

	srv = malloc(sizeof(struct server));
	if (srv == NULL)
	{
		fprintf(stderr, "Failed to create server: out of memory.\n");
		return NULL;
	}

	memset(srv, 0, sizeof(struct server));
	srv->handler = h;
	srv->clients = NULL;
	srv->sd = -1;

	return srv;
}

void srv_free(struct server *srv)
{
	if (srv == NULL)
	{
		return;
	}

	srv_freeAllClients(srv);

	if (srv->sd > -1)
	{
		close(srv->sd);
	}

	free(srv);
}

int srv_run(struct server *srv, int port, int queueSize)
{
	int rc = 0;

	if (srv == NULL || srv->sd > -1)
	{
		fprintf(stderr, "Invalid server instance.\n");
		return -1;
	}

	/* Create server socket */
	srv->sd = srv_createAndBind(port);
	if (srv->sd == -1)
	{
		return -1;
	}

	rc = srv_setNonBlocking(srv->sd);
	if (rc == -1)
	{
		goto on_exit;
	}

	rc = listen(srv->sd, 5);
	if (rc == -1)
	{
		perror("listen");
		goto on_exit;
	}

	srv_onStart(srv);
	srv_eventLoop(srv, queueSize);
	srv_onStop(srv);

on_exit:
	srv_freeAllClients(srv);
	close(srv->sd);
	srv->sd = -1;

	return rc;
}

void srv_stop(struct server *srv)
{
	if (srv != NULL)
	{
		/*
		 * Note that this likely won't work if SA_RESTART is set when
		 * registering the signal handler (epoll_wait won't return with -1
		 * and set errno to EINTR then).
		 */
		srv->shouldQuit = 1;
	}
	else
	{
		fprintf(stderr, "Invalid server instance.\n");
	}
}
