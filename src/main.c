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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* Application configuration */
struct config
{
	int port;
	int eventQueue;
};

/* Server instance */
static struct server *g_srv = NULL;

/*
 * Shows usage information.
 */
static void printUsage(void)
{
	puts("Options:");
	puts(" -e n  Set event queue size.");
	puts(" -h    Displays this help text.");
	puts(" -p n  Set port number.");
}

/*
 * Parses command line arguments.
 * Returns 0 on success, -1 on failure.
 */
static int parseArgs(int argc, char *argv[], struct config *cfg)
{
	int ch;

	assert(cfg != NULL);

	/* Set defaults */
	cfg->port = 5033;
	cfg->eventQueue = 64;

	while ((ch = getopt(argc, argv, "e:hp:")) != -1)
	{
		switch (ch)
		{
		case 'e':
			cfg->eventQueue = atoi(optarg);
			if (cfg->eventQueue < 1)
			{
				fprintf(stderr, "Invalid event queue size: %d\n",
					cfg->eventQueue);
				return -1;
			}
			break;
		case 'h':
			printUsage();
			return -1;
		case 'p':
			cfg->port = atoi(optarg);
			break;
		default:
			return -1;
		}
	}

	return 0;
}

static void onStartHandler(void)
{
	puts("Server started");
}

static void onStopHandler(void)
{
	puts("Server stopped");
}

static void onConnectHandler(const char *ip)
{
	printf("Client %s connected\n", ip);
}

static void onDisconnectHandler(const char *ip)
{
	printf("Client %s disconnected\n", ip);
}

static void onReceiveHandler(const char *ip, const char *buffer, int len)
{
	int i;
	printf("Received %d bytes from %s:\n", len, ip);

	for (i = 0; i < len - 1; ++i)
	{
		printf("0x%02X ", buffer[i]);
	}

	printf("0x%02X\n", buffer[len - 1]);
}

/* Custom signal handler */
static void onSignal(int s)
{
	switch (s)
	{
	case SIGINT:
	case SIGTERM:
		if (g_srv != NULL)
		{
			srv_stop(g_srv);
		}
		break;
	}
}

/*
 * Register new signal handler.
 * Returns 0 on success, -1 on failure.
 */
static int registerSignalHandler(void)
{
	/*
	 * TODO Maybe it would be safer to register the custom signal handler from
	 * within the server. This way we could ensure it is registered properly.
	 */
	struct sigaction sa;

	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = onSignal;
	/* Don't use SA_RESTART here */
	sa.sa_flags = 0;
	sigfillset(&sa.sa_mask);

	if (sigaction(SIGINT, &sa, NULL) != 0)
	{
		goto on_error;
	}

	if (sigaction(SIGTERM, &sa, NULL) != 0)
	{
		goto on_error;
	}

	return 0;

on_error:
	perror("sigaction");
	return -1;
}

int main(int argc, char *argv[])
{
	struct config cfg;
	struct srv_handler handler;
	int rc;

	if (parseArgs(argc, argv, &cfg) != 0)
	{
		return 1;
	}

	if (registerSignalHandler() != 0)
	{
		return 1;
	}

	printf("Starting server on port %d\n", cfg.port);

	/* Register server event handler */
	handler.on_start = onStartHandler;
	handler.on_stop = onStopHandler;
	handler.on_connect = onConnectHandler;
	handler.on_disconnect = onDisconnectHandler;
	handler.on_receive = onReceiveHandler;

	g_srv = srv_create(&handler);
	if (g_srv == NULL)
	{
		return -1;
	}

	rc = srv_run(g_srv, cfg.port, cfg.eventQueue);
	srv_free(g_srv);

	return rc;
}
