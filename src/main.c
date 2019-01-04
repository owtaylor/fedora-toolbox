/*
 * Copyright © 2018 Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.#include <gio/gio.h>
 */

#include <glib-unix.h>

#include "container.h"
#include "manager.h"
#include "util.h"

static ToolboxManager *manager;

static void
on_bus_acquired(GDBusConnection *connection,
                const gchar *name,
                gpointer user_data)
{
    g_autoptr(GError) error = NULL;
    GMainLoop *loop = user_data;

    manager = toolbox_manager_impl_new();

    if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(manager), connection,
                                          "/org/fedoraproject/Toolbox/Manager", &error)) {
        g_printerr("Failed to export manager object: %s", error->message);
        g_main_loop_quit(loop);
    }

    toolbox_manager_impl_start(TOOLBOX_MANAGER_IMPL(manager));
}

static void
on_name_acquired(GDBusConnection *connection,
                 const gchar *name,
                 gpointer user_data)
{
}

static void
on_name_lost(GDBusConnection *connection,
             const gchar *name,
            gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_main_loop_quit(loop);
}

gboolean
on_sigint(void *data)
{
    GMainLoop *loop = data;
    g_printerr("Received SIGINT\n");
    g_main_loop_quit(loop);

    return G_SOURCE_REMOVE;
}

gboolean
on_sigterm(void *data)
{
    GMainLoop *loop = data;
    g_printerr("Received SIGTERM\n");
    g_main_loop_quit(loop);

    return G_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    guint id;

    g_type_ensure(TOOLBOX_TYPE_CONTAINER);
    g_type_ensure(TOOLBOX_TYPE_MANAGER);
    toolbox_executable_init(argv[0]);

    loop = g_main_loop_new(NULL, FALSE);

    id = g_bus_own_name(G_BUS_TYPE_SESSION, "org.fedoraproject.Toolbox.Manager",
                        G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
                        on_bus_acquired, on_name_acquired, on_name_lost, loop, NULL);

    g_unix_signal_add(SIGTERM, on_sigterm, loop);
    g_unix_signal_add(SIGINT, on_sigint, loop);

    g_main_loop_run (loop);
    toolbox_manager_impl_stop(TOOLBOX_MANAGER_IMPL(manager));

    g_bus_unown_name (id);

    return 0;
}
