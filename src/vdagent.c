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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <spice/vd_agent.h>

#include "udscs.h"
#include "vdagentd-proto.h"
#include "vdagentd-proto-strings.h"
#include "vdagent-x11.h"

static const char *portdev = "/dev/virtio-ports/com.redhat.spice.0";
static int debug = 0;
static struct vdagent_x11 *x11 = NULL;
static struct udscs_connection *client = NULL;
static int quit = 0;
static int version_mismatch = 0;

void daemon_read_complete(struct udscs_connection **connp,
    struct udscs_message_header *header, uint8_t *data)
{
    switch (header->type) {
    case VDAGENTD_MONITORS_CONFIG:
        vdagent_x11_set_monitor_config(x11, (VDAgentMonitorsConfig *)data);
        free(data);
        break;
    case VDAGENTD_CLIPBOARD_REQUEST:
        vdagent_x11_clipboard_request(x11, header->arg1, header->arg2);
        free(data);
        break;
    case VDAGENTD_CLIPBOARD_GRAB:
        vdagent_x11_clipboard_grab(x11, header->arg1, (uint32_t *)data,
                                   header->size / sizeof(uint32_t));
        free(data);
        break;
    case VDAGENTD_CLIPBOARD_DATA:
        vdagent_x11_clipboard_data(x11, header->arg1, header->arg2,
                                   data, header->size);
        /* vdagent_x11_clipboard_data takes ownership of the data (or frees
           it immediately) */
        break;
    case VDAGENTD_CLIPBOARD_RELEASE:
        vdagent_x11_clipboard_release(x11, header->arg1);
        free(data);
        break;
    case VDAGENTD_VERSION:
        if (strcmp((char *)data, VERSION) != 0) {
            syslog(LOG_INFO, "vdagentd version mismatch: got %s expected %s",
                   data, VERSION);
            udscs_destroy_connection(connp);
            version_mismatch = 1;
        }
        break;
    default:
        syslog(LOG_ERR, "Unknown message from vdagentd type: %d, ignoring",
               header->type);
        free(data);
    }
}

int client_setup(int reconnect)
{
    while (!quit) {
        client = udscs_connect(VDAGENTD_SOCKET, daemon_read_complete, NULL,
                               vdagentd_messages, VDAGENTD_NO_MESSAGES,
                               debug);
        if (client || !reconnect || quit) {
            break;
        }
        sleep(1);
    }
    return client == NULL;
}

static void usage(FILE *fp)
{
    fprintf(fp,
            "vdagent -- spice agent xorg client\n"
            "options:\n"
            "  -h         print this text\n"
            "  -d         log debug messages\n"
            "  -s <port>  set virtio serial port  [%s]\n"
            "  -x         don't daemonize\n",
            portdev);
}

static void quit_handler(int sig)
{
    quit = 1;
}

void daemonize(void)
{
    int x, retval = 0;

    /* detach from terminal */
    switch (fork()) {
    case 0:
        close(0); close(1); close(2);
        setsid();
        x = open("/dev/null", O_RDWR); x = dup(x); x = dup(x);
        break;
    case -1:
        syslog(LOG_ERR, "fork: %s", strerror(errno));
        retval = 1;
    default:
        exit(retval);
    }
}

static int file_test(const char *path)
{
    struct stat buffer;

    return stat(path, &buffer);
}

int main(int argc, char *argv[])
{
    fd_set readfds, writefds;
    int c, n, nfds, x11_fd;
    int do_daemonize = 1;
    int x11_sync = 0;
    struct sigaction act;

    for (;;) {
        if (-1 == (c = getopt(argc, argv, "-dxhys:")))
            break;
        switch (c) {
        case 'd':
            debug++;
            break;
        case 's':
            portdev = optarg;
            break;
        case 'x':
            do_daemonize = 0;
            break;
        case 'y':
            x11_sync = 1;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 1;
        }
    }

    memset(&act, 0, sizeof(act));
    act.sa_flags = SA_RESTART;
    act.sa_handler = quit_handler;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);

    openlog("spice-vdagent", do_daemonize ? LOG_PID : (LOG_PID | LOG_PERROR),
            LOG_USER);

    if (file_test(portdev) != 0) {
        syslog(LOG_ERR, "Missing virtio device '%s': %s",
                portdev, strerror(errno));
        return 1;
    }

    if (do_daemonize)
        daemonize();

reconnect:
    if (version_mismatch) {
        syslog(LOG_INFO, "Version mismatch, restarting");
        sleep(1);
        execvp(argv[0], argv);
    }

    if (client_setup(do_daemonize)) {
        return 1;
    }

    x11 = vdagent_x11_create(client, debug, x11_sync);
    if (!x11) {
        udscs_destroy_connection(&client);
        return 1;
    }

    while (client && !quit) {
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
            syslog(LOG_ERR, "Fatal error select: %s", strerror(errno));
            break;
        }

        if (FD_ISSET(x11_fd, &readfds))
            vdagent_x11_do_read(x11);
        udscs_client_handle_fds(&client, &readfds, &writefds);
    }

    vdagent_x11_destroy(x11, client == NULL);
    udscs_destroy_connection(&client);
    if (!quit)
        goto reconnect;

    return 0;
}
