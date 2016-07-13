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

#ifndef SERVER_H
#define SERVER_H

struct server;

/* Server event handler interface */
struct srv_handler
{
	void (*on_start)(void);
	void (*on_stop)(void);
	void (*on_connect)(const char *ip);
	void (*on_disconnect)(const char *ip);
	void (*on_receive)(const char *ip, const char *buffer, int len);
};

/*
 * Sets the server event handler.
 * Returns 0 on success, -1 on failure.
 */
int srv_setHandler(struct server *srv, const struct srv_handler *h);

/*
 * Creates a new server instance.
 * Returns NULL on failure.
 */
struct server *srv_create(const struct srv_handler *h);

/*
 * Frees a server instance.
 */
void srv_free(struct server *srv);

/*
 * Starts the server on the given port. This will block until the server is
 * stopped.
 */
int srv_run(struct server *srv, int port, int queueSize);

/*
 * Stops the server.
 */
void srv_stop(struct server *srv);

#endif
