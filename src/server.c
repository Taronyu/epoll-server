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

/* Client instance */
struct client
{
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
};


/*
 * Removes a client from the clients list and frees its resources.
 */
static void cl_free(struct client *cl)
{
	if (cl == NULL)
	{
		return;
	}

	/* Remove client from client list */
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
 * Creates a new client instance and adds it to the connected clients list.
 * Returns NULL on failure.
 */
static struct client *cl_create(struct server *srv, int sd)
{
	struct client *cl;

	assert(srv != NULL);

	cl = malloc(sizeof(struct client));
	if (cl == NULL)
	{
		fprintf(stderr, "Failed to create client instance: out of memory.\n");
		return NULL;
	}

	memset(cl, 0, sizeof(struct client));
	cl->srv = srv;
	cl->sd = sd;

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
 * Raises the server start event.
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
 * Raises the server stop event.
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
 * Raises the client connect event.
 */
static void srv_onConnect(struct server *srv)
{
	const struct srv_handler *h;

	assert(srv != NULL);

	h = srv->handler;
	if (h != NULL && h->on_connect != NULL)
	{
		h->on_connect();
	}
}

/*
 * Raises the client disconnect event.
 */
static void srv_onDisconnect(struct server *srv)
{
	const struct srv_handler *h;

	assert(srv != NULL);

	h = srv->handler;
	if (h != NULL && h->on_disconnect != NULL)
	{
		h->on_disconnect();
	}
}

/*
 * Raises the client data receive event.
 */
static void srv_onReceive(struct server *srv, const char *buf, ssize_t len)
{
	const struct srv_handler *h;

	assert(srv != NULL);

	h = srv->handler;
	if (h != NULL && h->on_receive != NULL)
	{
		h->on_receive(buf, len);
	}
}

/*
 * Sets a socket descriptor to non-blocking IO.
 * Return 0 on success, -1 on failure.
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
 * Creates and binds a new TCP server socket on the given port.
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
 * Removes all clients from the connected clients list.
 */
static void srv_freeAllClients(struct server *srv)
{
	assert(srv != NULL);

	while (srv->clients != NULL)
	{
		struct client *cl = srv->clients->next;
		cl_free(srv->clients);
		srv->clients = cl;
	}
}

/*
 * Handles epoll error events.
 */
static void srv_handleError(const struct epoll_event *ev)
{
	struct client *cl;
	struct server *srv;

	fprintf(stderr, "Connection lost or epoll error.\n");

	cl = ev->data.ptr;
	srv = cl->srv;

	/* If the event should contain client data, the order must be reversed. */
	cl_free(cl);
	srv_onDisconnect(srv);
}

/*
 * Handles accept events.
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

	cl = cl_create(srv, sd);
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

	srv_onConnect(srv);
	return;

on_error:
	cl_free(cl);
	close(sd);
}

/*
 * Handles data receive events.
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
		char buffer[2048];
		ssize_t len;

		len = read(cl->sd, buffer, 2048);
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
			srv_onReceive(cl->srv, buffer, len);
		}
	}

	if (done != 0)
	{
		/* Remove client */
		struct server *srv = cl->srv;
		cl_free(cl);
		srv_onDisconnect(srv);
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

	/* Event loop */
	while (1)
	{
		int n, i;

		/* Wait for epoll events */
		n = epoll_wait(efd, events, queueSize, -1);
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
