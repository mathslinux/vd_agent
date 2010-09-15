/*  vdagent.c xorg-client to vdagentd (daemon).

    Copyright 2010 Red Hat, Inc.

    Red Hat Authors:
    Hans de Goede <hdegoede@redhat.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or   
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of 
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the  
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>

#include "udscs.h"
#include "vdagentd-proto.h"
#include "vdagent-x11.h"

void daemon_read_complete(struct udscs_connection *conn,
    struct udscs_message_header *header, const uint8_t *data)
{
}

int main(int argc, char *argv[])
{
    struct udscs_connection *client;
    struct vdagent_x11 *x11;
    fd_set readfds, writefds;
    int n, nfds, x11_fd;
    /* FIXME make configurable */
    int verbose = 1;
    
    client = udscs_connect(VDAGENTD_SOCKET, daemon_read_complete, NULL);
    if (!client)
        exit(1);

    x11 = vdagent_x11_create(client, verbose);
    if (!x11) {
        udscs_destroy_connection(client);
        exit(1);
    }

    /* FIXME exit when server is gone */
    for (;;) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        nfds = udscs_client_fill_fds(client, &readfds, &writefds);
        x11_fd = vdagent_x11_get_fd(x11);
        FD_SET(x11_fd, &readfds);
        if (x11_fd >= nfds)
            nfds = x11_fd + 1;

        n = select(nfds, &readfds, &writefds, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            perror("select");
            exit(1);
        }

        udscs_client_handle_fds(client, &readfds, &writefds);
        if (FD_SET(x11_fd, &readfds))
            vdagent_x11_do_read(x11);
    }

    vdagent_x11_destroy(x11);
    udscs_destroy_connection(client);

    return 0;    
}
