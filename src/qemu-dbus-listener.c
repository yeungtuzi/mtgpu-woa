/* 最小 QEMU dbus display 监听器：验证 RegisterListener + Scanout/Update 能通 */
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_w, g_h, g_stride;
static int g_scans = 0, g_updates = 0;

static const char *iface_xml =
  "<node>"
  " <interface name='org.qemu.Display1.Listener'>"
  "  <method name='Scanout'>"
  "   <arg type='u' direction='in'/><arg type='u' direction='in'/>"
  "   <arg type='u' direction='in'/><arg type='u' direction='in'/>"
  "   <arg type='ay' direction='in'/>"
  "  </method>"
  "  <method name='Update'>"
  "   <arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "   <arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "   <arg type='u' direction='in'/><arg type='u' direction='in'/>"
  "   <arg type='ay' direction='in'/>"
  "  </method>"
  "  <method name='Disable'/>"
  "  <method name='MouseSet'>"
  "   <arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "   <arg type='i' direction='in'/>"
  "  </method>"
  "  <method name='CursorDefine'>"
  "   <arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "   <arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "   <arg type='ay' direction='in'/>"
  "  </method>"
  " </interface>"
  "</node>";

static void on_method(GDBusConnection *c, const gchar *sender, const gchar *path,
                      const gchar *iface, const gchar *method, GVariant *params,
                      GDBusMethodInvocation *inv, gpointer user_data) {
    if (!strcmp(method, "Scanout")) {
        guint32 w, h, stride, fmt;
        GVariant *data;
        g_variant_get(params, "(uuuu@ay)", &w, &h, &stride, &fmt, &data);
        gsize n = g_variant_get_size(data);
        g_w = (int)w; g_h = (int)h; g_stride = (int)stride;
        g_scans++;
        printf("Scanout %ux%u stride=%u fmt=0x%x bytes=%zu\n", w, h, stride, fmt, n);
        if (g_scans == 1) {
            const guint8 *px = g_variant_get_data(data);
            FILE *f = fopen("/tmp/dbtest.ppm", "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
                for (int y = 0; y < g_h; y++) {
                    const guint8 *row = px + (size_t)y * g_stride;
                    for (int x = 0; x < g_w; x++) {
                        /* BGRX */
                        guint8 rgb[3] = { row[x*4+2], row[x*4+1], row[x*4+0] };
                        fwrite(rgb, 1, 3, f);
                    }
                }
                fclose(f);
                printf("saved /tmp/dbtest.ppm\n");
            }
        }
        g_variant_unref(data);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "Update")) {
        gint32 x, y, w, h; guint32 stride, fmt; GVariant *data;
        g_variant_get(params, "(iiiiuu@ay)", &x, &y, &w, &h, &stride, &fmt, &data);
        g_updates++;
        if (g_updates <= 3)
            printf("Update %dx%d+%d+%d stride=%u fmt=0x%x bytes=%zu\n",
                   w, h, x, y, stride, fmt, g_variant_get_size(data));
        g_variant_unref(data);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else {
        g_dbus_method_invocation_return_value(inv, NULL);
    }
}

static const GDBusInterfaceVTable vtable = { .method_call = on_method };

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *addr = argv[1];
    GError *err = NULL;
    GDBusConnection *bus = g_dbus_connection_new_for_address_sync(addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &err);
    if (!bus) { printf("bus connect: %s\n", err->message); return 1; }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { perror("socketpair"); return 1; }
    GUnixFDList *fdl = g_unix_fd_list_new();
    int idx = g_unix_fd_list_append(fdl, sv[0], &err);
    if (idx < 0) { printf("fd list: %s\n", err->message); return 1; }

    GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync(bus, "org.qemu",
        "/org/qemu/Display1/Console_0", "org.qemu.Display1.Console",
        "RegisterListener", g_variant_new("(h)", 0), G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE, 5000, fdl, NULL, NULL, &err);
    if (!r) { printf("RegisterListener: %s\n", err->message); return 1; }
    g_variant_unref(r);
    printf("RegisterListener ok, peer fd=%d\n", sv[1]);

    GSocket *sock = g_socket_new_from_fd(sv[1], &err);
    if (!sock) { printf("socket: %s\n", err->message); return 1; }
    GSocketConnection *conn = g_socket_connection_factory_create_connection(sock);
    GDBusConnection *peer = g_dbus_connection_new_sync(G_IO_STREAM(conn), NULL,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
        NULL, NULL, &err);
    if (!peer) { printf("peer: %s\n", err->message); return 1; }

    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(iface_xml, &err);
    if (!info) { printf("node info: %s\n", err->message); return 1; }
    guint id = g_dbus_connection_register_object(peer, "/org/qemu/Display1/Listener",
        info->interfaces[0], &vtable, NULL, NULL, &err);
    if (!id) { printf("register object: %s\n", err->message); return 1; }
    printf("listener exported, waiting for scanout...\n");
    fflush(stdout);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
