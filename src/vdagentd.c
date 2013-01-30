/*  vdagentd.c vdagentd (daemon) code

    Copyright 2010-2012 Red Hat, Inc.

    Red Hat Authors:
    Hans de Goede <hdegoede@redhat.com>
    Gerd Hoffmann <kraxel@redhat.com>

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
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <spice/vd_agent.h>

#include "udscs.h"
#include "vdagentd-proto.h"
#include "vdagentd-proto-strings.h"
#include "vdagentd-uinput.h"
#include "vdagentd-xorg-conf.h"
#include "vdagent-virtio-port.h"
#include "session-info.h"

struct agent_data {
    char *session;
    int width;
    int height;
    struct vdagentd_guest_xorg_resolution *screen_info;
    int screen_count;
};

/* variables */
static const char *pidfilename = "/var/run/spice-vdagentd/spice-vdagentd.pid";
static const char *portdev = "/dev/virtio-ports/com.redhat.spice.0";
static const char *uinput_device = "/dev/uinput";
static int debug = 0;
static struct udscs_server *server = NULL;
static struct vdagent_virtio_port *virtio_port = NULL;
#ifdef HAVE_SESSION_INFO
static struct session_info *session_info = NULL;
#endif
static struct vdagentd_uinput *uinput = NULL;
static VDAgentMonitorsConfig *mon_config = NULL;
static uint32_t *capabilities = NULL;
static int capabilities_size = 0;
#ifdef HAVE_SESSION_INFO
static const char *active_session = NULL;
#else
static unsigned int session_count = 0;
#endif
static struct udscs_connection *active_session_conn = NULL;
static int agent_owns_clipboard[256] = { 0, };
static int quit = 0;
static int retval = 0;

/* utility functions */
/* vdagentd <-> spice-client communication handling */
static void send_capabilities(struct vdagent_virtio_port *vport,
    uint32_t request)
{
    VDAgentAnnounceCapabilities *caps;
    uint32_t size;

    size = sizeof(*caps) + VD_AGENT_CAPS_BYTES;
    caps = calloc(1, size);
    if (!caps) {
        syslog(LOG_ERR, "out of memory allocating capabilities array (write)");
        return;
    }

    caps->request = request;
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MOUSE_STATE);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MONITORS_CONFIG);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_REPLY);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_SELECTION);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_SPARSE_MONITORS_CONFIG);

    vdagent_virtio_port_write(vport, VDP_CLIENT_PORT,
                              VD_AGENT_ANNOUNCE_CAPABILITIES, 0,
                              (uint8_t *)caps, size);
    free(caps);
}

static void do_client_monitors(struct vdagent_virtio_port *vport, int port_nr,
    VDAgentMessage *message_header, VDAgentMonitorsConfig *new_monitors)
{
    VDAgentReply reply;
    uint32_t size;

    /* Store monitor config to send to agents when they connect */
    size = sizeof(VDAgentMonitorsConfig) +
           new_monitors->num_of_monitors * sizeof(VDAgentMonConfig);
    if (message_header->size != size) {
        syslog(LOG_ERR, "invalid message size for VDAgentMonitorsConfig");
        return;
    }

    vdagentd_write_xorg_conf(new_monitors);

    if (!mon_config ||
            mon_config->num_of_monitors != new_monitors->num_of_monitors) {
        free(mon_config);
        mon_config = malloc(size);
        if (!mon_config) {
            syslog(LOG_ERR, "out of memory allocating monitors config");
            return;
        }
    }
    memcpy(mon_config, new_monitors, size);

    /* Send monitor config to currently active agent */
    if (active_session_conn)
        udscs_write(active_session_conn, VDAGENTD_MONITORS_CONFIG, 0, 0,
                    (uint8_t *)mon_config, size);

    /* Acknowledge reception of monitors config to spice server / client */
    reply.type  = VD_AGENT_MONITORS_CONFIG;
    reply.error = VD_AGENT_SUCCESS;
    vdagent_virtio_port_write(vport, port_nr, VD_AGENT_REPLY, 0,
                              (uint8_t *)&reply, sizeof(reply));
}

static void do_client_capabilities(struct vdagent_virtio_port *vport,
    VDAgentMessage *message_header,
    VDAgentAnnounceCapabilities *caps)
{
    int new_size = VD_AGENT_CAPS_SIZE_FROM_MSG_SIZE(message_header->size);

    if (capabilities_size != new_size) {
        capabilities_size = new_size;
        free(capabilities);
        capabilities = malloc(capabilities_size * sizeof(uint32_t));
        if (!capabilities) {
            syslog(LOG_ERR, "oom allocating capabilities array (read)");
            capabilities_size = 0;
            return;
        }
    }
    memcpy(capabilities, caps->caps, capabilities_size * sizeof(uint32_t));
    if (caps->request)
        send_capabilities(vport, 0);
}

static void do_client_clipboard(struct vdagent_virtio_port *vport,
    VDAgentMessage *message_header, uint8_t *data)
{
    uint32_t msg_type = 0, data_type = 0, size = message_header->size;
    uint8_t selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;

    if (!active_session_conn) {
        syslog(LOG_WARNING,
               "Could not find an agent connnection belonging to the "
               "active session, ignoring client clipboard request");
        return;
    }

    if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
      selection = data[0];
      data += 4;
      size -= 4;
    }

    switch (message_header->type) {
    case VD_AGENT_CLIPBOARD_GRAB:
        msg_type = VDAGENTD_CLIPBOARD_GRAB;
        agent_owns_clipboard[selection] = 0;
        break;
    case VD_AGENT_CLIPBOARD_REQUEST: {
        VDAgentClipboardRequest *req = (VDAgentClipboardRequest *)data;
        msg_type = VDAGENTD_CLIPBOARD_REQUEST;
        data_type = req->type;
        data = NULL;
        size = 0;
        break;
    }
    case VD_AGENT_CLIPBOARD: {
        VDAgentClipboard *clipboard = (VDAgentClipboard *)data;
        msg_type = VDAGENTD_CLIPBOARD_DATA;
        data_type = clipboard->type;
        size = size - sizeof(VDAgentClipboard);
        data = clipboard->data;
        break;
    }
    case VD_AGENT_CLIPBOARD_RELEASE:
        msg_type = VDAGENTD_CLIPBOARD_RELEASE;
        data = NULL;
        size = 0;
        break;
    }

    udscs_write(active_session_conn, msg_type, selection, data_type,
                data, size);
}

int virtio_port_read_complete(
        struct vdagent_virtio_port *vport,
        int port_nr,
        VDAgentMessage *message_header,
        uint8_t *data)
{
    uint32_t min_size = 0;

    if (message_header->protocol != VD_AGENT_PROTOCOL) {
        syslog(LOG_ERR, "message with wrong protocol version ignoring");
        return 0;
    }

    switch (message_header->type) {
    case VD_AGENT_MOUSE_STATE:
        if (message_header->size != sizeof(VDAgentMouseState))
            goto size_error;
        vdagentd_uinput_do_mouse(&uinput, (VDAgentMouseState *)data);
        if (!uinput) {
            /* Try to re-open the tablet */
            struct agent_data *agent_data =
                udscs_get_user_data(active_session_conn);
            if (agent_data)
                uinput = vdagentd_uinput_create(uinput_device,
                                                agent_data->width,
                                                agent_data->height,
                                                agent_data->screen_info,
                                                agent_data->screen_count,
                                                debug > 1);
            if (!uinput) {
                syslog(LOG_CRIT, "Fatal uinput error");
                retval = 1;
                quit = 1;
            }
        }
        break;
    case VD_AGENT_MONITORS_CONFIG:
        if (message_header->size < sizeof(VDAgentMonitorsConfig))
            goto size_error;
        do_client_monitors(vport, port_nr, message_header,
                    (VDAgentMonitorsConfig *)data);
        break;
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
        if (message_header->size < sizeof(VDAgentAnnounceCapabilities))
            goto size_error;
        do_client_capabilities(vport, message_header,
                        (VDAgentAnnounceCapabilities *)data);
        break;
    case VD_AGENT_CLIPBOARD_GRAB:
    case VD_AGENT_CLIPBOARD_REQUEST:
    case VD_AGENT_CLIPBOARD:
    case VD_AGENT_CLIPBOARD_RELEASE:
        switch (message_header->type) {
        case VD_AGENT_CLIPBOARD_GRAB:
            min_size = sizeof(VDAgentClipboardGrab); break;
        case VD_AGENT_CLIPBOARD_REQUEST:
            min_size = sizeof(VDAgentClipboardRequest); break;
        case VD_AGENT_CLIPBOARD:
            min_size = sizeof(VDAgentClipboard); break;
        }
        if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                    VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
            min_size += 4;
        }
        if (message_header->size < min_size) {
            goto size_error;
        }
        do_client_clipboard(vport, message_header, data);
        break;
    default:
        syslog(LOG_WARNING, "unknown message type %d, ignoring",
               message_header->type);
    }

    return 0;

size_error:
    syslog(LOG_ERR, "read: invalid message size: %u for message type: %u",
           message_header->size, message_header->type);
    return 0;
}

/* vdagentd <-> vdagent communication handling */
int do_agent_clipboard(struct udscs_connection *conn,
        struct udscs_message_header *header, const uint8_t *data)
{
    uint8_t selection = header->arg1;
    uint32_t msg_type = 0, data_type = -1, size = header->size;

    if (!VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                 VD_AGENT_CAP_CLIPBOARD_BY_DEMAND))
        goto error;

    /* Check that this agent is from the currently active session */
    if (conn != active_session_conn) {
        if (debug)
            syslog(LOG_DEBUG, "%p clipboard req from agent which is not in "
                              "the active session?", conn);
        goto error;
    }

    if (!virtio_port) {
        syslog(LOG_ERR, "Clipboard req from agent but no client connection");
        goto error;
    }

    if (!VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                 VD_AGENT_CAP_CLIPBOARD_SELECTION) &&
            selection != VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD) {
        goto error;
    }

    switch (header->type) {
    case VDAGENTD_CLIPBOARD_GRAB:
        msg_type = VD_AGENT_CLIPBOARD_GRAB;
        agent_owns_clipboard[selection] = 1;
        break;
    case VDAGENTD_CLIPBOARD_REQUEST:
        msg_type = VD_AGENT_CLIPBOARD_REQUEST;
        data_type = header->arg2;
        size = 0;
        break;
    case VDAGENTD_CLIPBOARD_DATA:
        msg_type = VD_AGENT_CLIPBOARD;
        data_type = header->arg2;
        break;
    case VDAGENTD_CLIPBOARD_RELEASE:
        msg_type = VD_AGENT_CLIPBOARD_RELEASE;
        size = 0;
        agent_owns_clipboard[selection] = 0;
        break;
    }

    if (size != header->size) {
        syslog(LOG_ERR,
               "unexpected extra data in clipboard msg, disconnecting agent");
        return -1;
    }

    if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        size += 4;
    }
    if (data_type != -1) {
        size += 4;
    }

    vdagent_virtio_port_write_start(virtio_port, VDP_CLIENT_PORT, msg_type,
                                    0, size);

    if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        uint8_t sel[4] = { selection, 0, 0, 0 };
        vdagent_virtio_port_write_append(virtio_port, sel, 4);
    }
    if (data_type != -1) {
        vdagent_virtio_port_write_append(virtio_port, (uint8_t*)&data_type, 4);
    }

    vdagent_virtio_port_write_append(virtio_port, data, header->size);

    return 0;

error:
    if (header->type == VDAGENTD_CLIPBOARD_REQUEST) {
        /* Let the agent know no answer is coming */
        udscs_write(conn, VDAGENTD_CLIPBOARD_DATA,
                    selection, VD_AGENT_CLIPBOARD_NONE, NULL, 0);
    }
    return 0;
}

/* When we open the vdagent virtio channel, the server automatically goes into
   client mouse mode, so we can only have the channel open when we know the
   active session resolution. This function checks that we have an agent in the
   active session, and that it has told us its resolution. If these conditions
   are met it sets the uinput tablet device's resolution and opens the virtio
   channel (if it is not already open). If these conditions are not met, it
   closes both. */
static void check_xorg_resolution(void)
{
    struct agent_data *agent_data = udscs_get_user_data(active_session_conn);

    if (agent_data && agent_data->screen_info) {
        if (!uinput)
            uinput = vdagentd_uinput_create(uinput_device,
                                            agent_data->width,
                                            agent_data->height,
                                            agent_data->screen_info,
                                            agent_data->screen_count,
                                            debug > 1);
        else
            vdagentd_uinput_update_size(&uinput,
                                        agent_data->width,
                                        agent_data->height,
                                        agent_data->screen_info,
                                        agent_data->screen_count);
        if (!uinput) {
            syslog(LOG_CRIT, "Fatal uinput error");
            retval = 1;
            quit = 1;
            return;
        }

        if (!virtio_port) {
            syslog(LOG_INFO, "opening vdagent virtio channel");
            virtio_port = vdagent_virtio_port_create(portdev,
                                                     virtio_port_read_complete,
                                                     NULL);
            if (!virtio_port) {
                syslog(LOG_CRIT, "Fatal error opening vdagent virtio channel");
                retval = 1;
                quit = 1;
                return;
            }
            send_capabilities(virtio_port, 1);
        }
    } else {
#ifndef WITH_STATIC_UINPUT
        vdagentd_uinput_destroy(&uinput);
#endif
        if (virtio_port) {
            vdagent_virtio_port_flush(&virtio_port);
            vdagent_virtio_port_destroy(&virtio_port);
            syslog(LOG_INFO, "closed vdagent virtio channel");
        }
    }
}

#ifdef HAVE_SESSION_INFO
static int connection_matches_active_session(struct udscs_connection **connp,
    void *priv)
{
    struct udscs_connection **conn_ret = (struct udscs_connection **)priv;
    struct agent_data *agent_data = udscs_get_user_data(*connp);

    /* Check if this connection matches the currently active session */
    if (!agent_data->session || !active_session)
        return 0;
    if (strcmp(agent_data->session, active_session))
        return 0;

    *conn_ret = *connp;
    return 1;
}
#endif

void release_clipboards(void)
{
    uint8_t sel;

    for (sel = 0; sel < VD_AGENT_CLIPBOARD_SELECTION_SECONDARY; ++sel) {
        if (agent_owns_clipboard[sel] && virtio_port) {
            vdagent_virtio_port_write(virtio_port, VDP_CLIENT_PORT,
                                      VD_AGENT_CLIPBOARD_RELEASE, 0, &sel, 1);
        }
        agent_owns_clipboard[sel] = 0;
    }
}

void update_active_session_connection(void)
{
#ifdef HAVE_SESSION_INFO
    int n;
    struct udscs_connection *new_conn = NULL;
    if (!active_session)
        active_session = session_info_get_active_session(session_info);

    n = udscs_server_for_all_clients(server, connection_matches_active_session,
                                     (void*)&new_conn);
    if (n != 1)
        new_conn = NULL;

    if (new_conn == active_session_conn)
        return;

    active_session_conn = new_conn;
    if (debug)
        syslog(LOG_DEBUG, "%p is now the active session", new_conn);
    if (active_session_conn && mon_config)
        udscs_write(active_session_conn, VDAGENTD_MONITORS_CONFIG, 0, 0,
                    (uint8_t *)mon_config, sizeof(VDAgentMonitorsConfig) +
                    mon_config->num_of_monitors * sizeof(VDAgentMonConfig));
#endif

    release_clipboards();

    check_xorg_resolution();    
}

void agent_connect(struct udscs_connection *conn)
{
#ifdef HAVE_SESSION_INFO
    uint32_t pid;
#endif
    struct agent_data *agent_data;

    agent_data = calloc(1, sizeof(*agent_data));
    if (!agent_data) {
        syslog(LOG_ERR, "Out of memory allocating agent data, disconnecting");
        udscs_destroy_connection(&conn);
        return;
    }
#ifdef HAVE_SESSION_INFO
    pid = udscs_get_peer_cred(conn).pid;
    agent_data->session = session_info_session_for_pid(session_info, pid);
#else
    session_count++;
    if (session_count == 1) {
        active_session_conn = conn;
    } else {
        /* disable communication with agents when we've got multiple
         * connections to the vdagentd and no consolekit since we can't
         * know to which one we should send data
         */
        syslog(LOG_ERR, "Trying to use multiple vdagent without ConsoleKit, "
               "disabling vdagent to avoid potential information leak");
        active_session_conn = NULL;
    }
#endif

    udscs_set_user_data(conn, (void *)agent_data);
    udscs_write(conn, VDAGENTD_VERSION, 0, 0,
                (uint8_t *)VERSION, strlen(VERSION) + 1);
    update_active_session_connection();
}

void agent_disconnect(struct udscs_connection *conn)
{
    struct agent_data *agent_data = udscs_get_user_data(conn);

#ifndef HAVE_SESSION_INFO
    if (conn == active_session_conn)
        active_session_conn = NULL;
#endif

    free(agent_data->session);
    agent_data->session = NULL;
    update_active_session_connection();

    free(agent_data);
#ifndef HAVE_SESSION_INFO
    session_count--;
#endif
}

void agent_read_complete(struct udscs_connection **connp,
    struct udscs_message_header *header, uint8_t *data)
{
    struct agent_data *agent_data = udscs_get_user_data(*connp);

    switch (header->type) {
    case VDAGENTD_GUEST_XORG_RESOLUTION: {
        struct vdagentd_guest_xorg_resolution *res;
        int n = header->size / sizeof(*res);

        /* Detect older version session agent, but don't disconnect, as
           that stops it from getting the VDAGENTD_VERSION message, and then
           it will never re-exec the new version... */
        if (header->arg1 == 0 && header->arg2 == 0) {
            syslog(LOG_INFO, "got old session agent xorg resolution message, "
                             "ignoring");
            free(data);
            return;
        }

        if (header->size != n * sizeof(*res)) {
            syslog(LOG_ERR, "guest xorg resolution message has wrong size, "
                            "disconnecting agent");
            udscs_destroy_connection(connp);
            free(data);
            return;
        }

        free(agent_data->screen_info);
        res = malloc(n * sizeof(*res));
        if (!res) {
            syslog(LOG_ERR, "out of memory allocating screen info");
            n = 0;
        }
        memcpy(res, data, n * sizeof(*res));
        agent_data->width  = header->arg1;
        agent_data->height = header->arg2;
        agent_data->screen_info  = res;
        agent_data->screen_count = n;

        check_xorg_resolution();
        break;
    }
    case VDAGENTD_CLIPBOARD_GRAB:
    case VDAGENTD_CLIPBOARD_REQUEST:
    case VDAGENTD_CLIPBOARD_DATA:
    case VDAGENTD_CLIPBOARD_RELEASE:
        if (do_agent_clipboard(*connp, header, data)) {
            udscs_destroy_connection(connp);
            free(data);
            return;
        }
        break;
    default:
        syslog(LOG_ERR, "unknown message from vdagent: %u, ignoring",
               header->type);
    }
    free(data);
}

/* main */

static void usage(FILE *fp)
{
    fprintf(fp,
            "vdagentd\n"
            "options:\n"
            "  -h         print this text\n"
            "  -d         log debug messages (use twice for extra info)\n"
            "  -s <port>  set virtio serial port  [%s]\n"
            "  -u <dev>   set uinput device       [%s]\n"
            "  -x         don't daemonize\n",
            portdev, uinput_device);
}

void daemonize(void)
{
    int x;
    FILE *pidfile;

    /* detach from terminal */
    switch (fork()) {
    case 0:
        close(0); close(1); close(2);
        setsid();
        x = open("/dev/null", O_RDWR); x = dup(x); x = dup(x);
        pidfile = fopen(pidfilename, "w");
        if (pidfile) {
            fprintf(pidfile, "%d\n", (int)getpid());
            fclose(pidfile);
        }
        break;
    case -1:
        syslog(LOG_ERR, "fork: %m");
        retval = 1;
    default:
        udscs_destroy_server(server);
        exit(retval);
    }
}

void main_loop(void)
{
    fd_set readfds, writefds;
    int n, nfds;
#ifdef HAVE_SESSION_INFO
    int ck_fd = 0;
#endif

    while (!quit) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        nfds = udscs_server_fill_fds(server, &readfds, &writefds);
        n = vdagent_virtio_port_fill_fds(virtio_port, &readfds, &writefds);
        if (n >= nfds)
            nfds = n + 1;

#ifdef HAVE_SESSION_INFO
        ck_fd = session_info_get_fd(session_info);
        FD_SET(ck_fd, &readfds);
        if (ck_fd >= nfds)
            nfds = ck_fd + 1;
#endif

        n = select(nfds, &readfds, &writefds, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_CRIT, "Fatal error select: %m");
            retval = 1;
            break;
        }

        udscs_server_handle_fds(server, &readfds, &writefds);

        if (virtio_port) {
            vdagent_virtio_port_handle_fds(&virtio_port, &readfds, &writefds);
            if (!virtio_port) {
                syslog(LOG_CRIT,
                       "AIIEEE lost spice client connection, reconnecting");
                virtio_port = vdagent_virtio_port_create(portdev,
                                                     virtio_port_read_complete,
                                                     NULL);
            }
            if (!virtio_port) {
                syslog(LOG_CRIT, "Fatal error opening vdagent virtio channel");
                retval = 1;
                break;
            }
        }

#ifdef HAVE_SESSION_INFO
        if (FD_ISSET(ck_fd, &readfds)) {
            active_session = session_info_get_active_session(session_info);
            update_active_session_connection();
        }
#endif
    }
}

static void quit_handler(int sig)
{
    quit = 1;
}

int main(int argc, char *argv[])
{
    int c;
    int do_daemonize = 1;
    struct sigaction act;

    for (;;) {
        if (-1 == (c = getopt(argc, argv, "-dhxs:u:")))
            break;
        switch (c) {
        case 'd':
            debug++;
            break;
        case 's':
            portdev = optarg;
            break;
        case 'u':
            uinput_device = optarg;
            break;
        case 'x':
            do_daemonize = 0;
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

    openlog("spice-vdagentd", do_daemonize ? 0 : LOG_PERROR, LOG_USER);

    /* Setup communication with vdagent process(es) */
    server = udscs_create_server(VDAGENTD_SOCKET, agent_connect,
                                 agent_read_complete, agent_disconnect,
                                 vdagentd_messages, VDAGENTD_NO_MESSAGES,
                                 debug);
    if (!server) {
        syslog(LOG_CRIT, "Fatal could not create server socket %s",
               VDAGENTD_SOCKET);
        return 1;
    }
    if (chmod(VDAGENTD_SOCKET, 0666)) {
        syslog(LOG_CRIT, "Fatal could not change permissions on %s: %m",
               VDAGENTD_SOCKET);
        udscs_destroy_server(server);
        return 1;
    }

    if (do_daemonize)
        daemonize();

#ifdef WITH_STATIC_UINPUT
    uinput = vdagentd_uinput_create(uinput_device, 1024, 768, NULL, 0,
                                    debug > 1);
    if (!uinput) {
        udscs_destroy_server(server);
        return 1;
    }
#endif

#ifdef HAVE_SESSION_INFO
    session_info = session_info_create(debug);
    if (!session_info) {
        syslog(LOG_CRIT, "Fatal could not get session information");
        vdagentd_uinput_destroy(&uinput);
        udscs_destroy_server(server);
        return 1;
    }
#endif

    main_loop();

    release_clipboards();

    vdagentd_uinput_destroy(&uinput);
    vdagent_virtio_port_flush(&virtio_port);
    vdagent_virtio_port_destroy(&virtio_port);
#ifdef HAVE_SESSION_INFO
    session_info_destroy(session_info);
#endif
    udscs_destroy_server(server);
    if (unlink(VDAGENTD_SOCKET) != 0)
        syslog(LOG_ERR, "unlink %s: %s", VDAGENTD_SOCKET, strerror(errno));
    syslog(LOG_INFO, "vdagentd quiting, returning status %d", retval);

    if (do_daemonize)
        unlink(pidfilename);

    return retval;
}
