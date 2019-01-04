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

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "manager.h"
#include "container.h"
#include "util.h"

// libpod/libpod/container.go
typedef enum {
    CONTAINER_STATE_UNKNOWN,
    CONTAINER_STATE_CONFIGURED,
    CONTAINER_STATE_CREATED,
    CONTAINER_STATE_RUNNING,
    CONTAINER_STATE_STOPPED,
    CONTAINER_STATE_PAUSED
} ContainerState;

struct _ToolboxManagerImplClass {
    ToolboxManagerSkeleton parent_class;
};

struct _ToolboxManagerImpl {
    ToolboxManagerSkeleton parent;

    GHashTable *toolboxes;
    GFile *envroot;
    GSubprocess *container_ps_subprocess;
};

G_DEFINE_TYPE(ToolboxManagerImpl, toolbox_manager_impl, TOOLBOX_TYPE_MANAGER_SKELETON);

static gboolean on_start(ToolboxManager        *manager,
                         GDBusMethodInvocation *invocation,
                         const char            *name,
                         gpointer               user_data);
static gboolean on_stop(ToolboxManager        *manager,
                        GDBusMethodInvocation *invocation,
                        const char            *name,
                        gpointer               user_data);

static void
toolbox_manager_impl_finalize(GObject *object)
{
    ToolboxManagerImpl *impl = TOOLBOX_MANAGER_IMPL(object);

    g_hash_table_destroy(impl->toolboxes);
    g_clear_object(&impl->envroot);
}

static void
toolbox_manager_impl_class_init(ToolboxManagerImplClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = toolbox_manager_impl_finalize;
}

static void
toolbox_manager_impl_init(ToolboxManagerImpl *impl)
{
    impl->toolboxes = g_hash_table_new(g_str_hash, g_str_equal);

    const char *data_dir = g_get_user_data_dir ();
    g_autofree char *envroot_path = g_build_filename(data_dir, "toolbox", "env", NULL);
    impl->envroot = g_file_new_for_path (envroot_path);

    g_signal_connect(impl, "handle-start", G_CALLBACK(on_start), NULL);
    g_signal_connect(impl, "handle-stop", G_CALLBACK(on_stop), NULL);
}

static void
free_toolbox_info(ToolboxInfo *info)
{
    g_free(info->id);
    g_free(info->name);
    g_free(info);
}

static ToolboxContainer *
add_container(ToolboxManagerImpl *impl,
              ToolboxInfo        *info)
{
    g_printerr("%s: Add (ID=%s, Pid=%d)\n", info->name, info->id, info->pid);
    ToolboxContainer *toolbox = toolbox_container_new(impl->envroot, info);
    g_hash_table_insert(impl->toolboxes, toolbox->info.name, toolbox);
    g_assert(g_hash_table_lookup(impl->toolboxes, info->name) == toolbox);

    if (toolbox->info.pid)
        toolbox_container_mount(toolbox);

    return toolbox;
}

static void
remove_container(ToolboxManagerImpl *impl,
                 ToolboxContainer   *toolbox)
{
    g_printerr("%s: Remove\n", toolbox->info.name);
    g_hash_table_remove(impl->toolboxes, toolbox->info.name);
    g_object_unref(toolbox);
}

static ToolboxContainer *
get_container_by_name(ToolboxManagerImpl *impl,
                      const char *name)
{
    return g_hash_table_lookup(impl->toolboxes, name);
}

static void
refresh_containers(ToolboxManagerImpl *impl,
                   GHashTable         *new_containers)
{
    g_autoptr(GHashTable) old_dirs =
      g_hash_table_new_full(g_str_hash, g_str_equal, (GDestroyNotify)g_free, NULL);
    g_autoptr(GHashTable) old_extra =
      g_hash_table_new_full(g_str_hash, g_str_equal, (GDestroyNotify)g_free, NULL);
    g_autoptr(GError) error = NULL;
    GHashTableIter iter;

    g_autoptr(GFileEnumerator) enumerator = g_file_enumerate_children(
      impl->envroot,
      G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE
                                     "," G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);

    if (!enumerator) {
        g_printerr("Failed to list contents of envroot: %s\n", error->message);
        g_clear_error(&error);
    }

    g_autofree char *old_default_link = NULL;
    g_autofree char *new_default_link = NULL;

    while (TRUE) {
        g_autoptr(GFileInfo) info = g_file_enumerator_next_file(enumerator, NULL, &error);
        if (!info) {
            if (error) {
                g_printerr("Failed to list contents of envroot: %s\n", error->message);
                g_clear_error(&error);
            }
            break;
        }

        const char *name = g_file_info_get_name(info);

        switch (g_file_info_get_file_type (info)) {
            case G_FILE_TYPE_DIRECTORY:
                g_hash_table_insert(old_dirs, g_strdup(name), GUINT_TO_POINTER(1));
                break;
            case G_FILE_TYPE_SYMBOLIC_LINK:
                if (g_strcmp0(name, "_default") == 0) {
                    old_default_link = g_strdup(g_file_info_get_symlink_target(info));
                    break;
                }
                /* fall through */
            default:
                g_hash_table_insert(old_extra, g_strdup(name), GUINT_TO_POINTER(1));
                break;
        }
    }

    g_hash_table_iter_init(&iter, old_extra);
    gpointer key;
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        const char *name = key;
        g_autoptr(GFile) child = g_file_get_child(impl->envroot, name);
        if (!g_file_delete(child, NULL, &error)) {
            g_autofree char *path = g_file_get_path(child);
            g_printerr("Unable to delete %s: %s\n", path, error->message);
            g_clear_error(&error);
        }
    }

    g_hash_table_iter_init(&iter, new_containers);
    gpointer value;
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        ToolboxInfo *info = value;
        ToolboxContainer *toolbox = get_container_by_name(impl, info->name);
        if (!toolbox)
            toolbox = add_container(impl, info);
        else
            toolbox_container_update(toolbox, info);

        gboolean has_dir = g_hash_table_lookup (old_dirs, info->name) != NULL;
        if (!has_dir) {
            g_autoptr(GFile) child = g_file_get_child(impl->envroot, info->name);
            if (!g_file_make_directory(child, NULL, &error)) {
                g_autofree char *path = g_file_get_path(child);
                g_printerr("Unable to create %s: %s\n", path, error->message);
                g_clear_error(&error);
            }
        }

        if (g_str_has_prefix(info->name, "fedora-toolbox") &&
            (!new_default_link || strcmp(toolbox->info.name, new_default_link) > 0))
        {
            g_clear_pointer(&new_default_link, g_free);
            new_default_link = g_strdup(info->name);
        }
    }

    g_hash_table_iter_init(&iter, impl->toolboxes);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        ToolboxContainer *toolbox = value;
        if (!g_hash_table_lookup(new_containers, toolbox->info.name))
            remove_container(impl, toolbox);
    }

    g_hash_table_iter_init(&iter, old_dirs);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        const char *name = key;
        if (!g_hash_table_lookup(new_containers, name)) {
            g_autoptr(GFile) child = g_file_get_child(impl->envroot, name);
            if (!g_file_delete(child, NULL, &error)) {
                g_autofree char *path = g_file_get_path(child);
                g_clear_error(&error);
            }
        }
    }

    if (g_strcmp0(new_default_link, old_default_link) != 0) {
        g_autoptr(GFile) child = g_file_get_child(impl->envroot, "_default");
        if (old_default_link && !g_file_delete(child, NULL, &error)) {
            g_autofree char *path = g_file_get_path(child);
            g_printerr("Unable to delete %s: %s\n", path, error->message);
            g_clear_error(&error);
        }

        if (new_default_link &&
            !g_file_make_symbolic_link(child, new_default_link, NULL, &error))
        {
            g_printerr("Unable to create default symlink: %s\n", error->message);
            g_clear_error(&error);
        }
    }
}

static void
on_ps_process_done (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
    ToolboxManagerImpl *impl = user_data;
    g_clear_object(&impl->container_ps_subprocess);
    g_autofree char *stdout_buf = NULL;

    g_autoptr(GError) error = NULL;
    if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &stdout_buf,
                                              NULL, &error)) {
        g_printerr("podman ps failed: %s\n", error->message);
        return;
    }

    int status = g_subprocess_get_exit_status(G_SUBPROCESS (source));
    if (status != 0) {
        g_printerr("podman ps exited with status: %d\n", status);
        return;
    }

    g_autoptr(JsonNode) node = json_from_string(stdout_buf, &error);
    if (!node) {
        g_printerr("podman ps output: %s", error->message);
        return;
    }

    g_autoptr(GHashTable) new_containers = g_hash_table_new_full(
      g_str_hash, g_str_equal, NULL, (GDestroyNotify)free_toolbox_info);

    if (JSON_NODE_HOLDS_ARRAY(node)) {
        JsonArray *array = json_node_get_array(node);
        for (guint i = 0; i < json_array_get_length(array); i++) {
            JsonNode *child = json_array_get_element(array, i);
            if (!JSON_NODE_HOLDS_OBJECT(child))
                continue;

            JsonObject *obj = json_node_get_object(child);
            JsonNode *id_node = json_object_get_member(obj, "ID");
            JsonNode *names_node = json_object_get_member(obj, "Names");
            JsonNode *state_node = json_object_get_member(obj, "State");
            JsonNode *pid_node = json_object_get_member(obj, "Pid");
            if (!id_node || !names_node || !pid_node)
                continue;

            const char *id = json_node_get_string(id_node);
            const char *names = json_node_get_string(names_node);
            int state = json_node_get_int(state_node);
            int pid = json_node_get_int(pid_node);
            if (!id || !names)
                continue;

            gboolean has_toolbox_label = FALSE;
            JsonNode *labels_node = json_object_get_member(obj, "Labels");
            if (labels_node && JSON_NODE_HOLDS_OBJECT(labels_node)) {
                JsonObject *labels_obj = json_node_get_object(labels_node);
                JsonNode *component_node =
                  json_object_get_member(labels_obj, "com.redhat.component");
                if (component_node) {
                    const char *component_label = json_node_get_string(component_node);
                    if (g_strcmp0(component_label, "fedora-toolbox") == 0)
                        has_toolbox_label = TRUE;
                }
            }

            if (has_toolbox_label) {
                ToolboxInfo *info = g_new0(ToolboxInfo, 1);
                info->name = g_strdup(names);
                info->id = g_strdup(id);
                // podman reports the PID that a stopped container used to have
                info->pid = state == CONTAINER_STATE_RUNNING ? pid : 0;
                g_hash_table_replace(new_containers, info->name, info);
            }
        }
    }

    refresh_containers(impl, new_containers);
}

static void
check_running(ToolboxManagerImpl *impl)
{
    g_autoptr(GError) error = NULL;
    if (impl->container_ps_subprocess)
        return;

    impl->container_ps_subprocess =
      g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, "podman", "ps", "-a",
                       "--format=json", "--no-trunc", "--namespace", NULL);
    if (!impl->container_ps_subprocess) {
        g_printerr("Failed to call podman ps: %s", error->message);
        return;
    }

    /* FIXME: memory management */
    g_subprocess_communicate_async(impl->container_ps_subprocess, NULL, NULL, on_ps_process_done, impl);
}

static void
on_start_done (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
    GDBusMethodInvocation *invocation = user_data;
    ToolboxManagerImpl *impl = g_dbus_method_invocation_get_user_data(invocation);

    g_autoptr(GError) error = NULL;
    if (!toolbox_container_start_finish(TOOLBOX_CONTAINER(source), result, &error))
        g_dbus_method_invocation_return_gerror(invocation, error);
    else {
        toolbox_manager_complete_start(TOOLBOX_MANAGER(impl), invocation);
        check_running(impl);
    }
}

static gboolean
on_start(ToolboxManager        *manager,
         GDBusMethodInvocation *invocation,
         const char            *name,
         gpointer               user_data)
{
    g_printerr("Start %s\n", name);

    ToolboxManagerImpl *impl = TOOLBOX_MANAGER_IMPL(manager);
    ToolboxContainer *toolbox = get_container_by_name(impl, name);
    if (toolbox == NULL) {
        g_dbus_method_invocation_return_error(invocation,
                                              G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                              "No toolbox %s", name);
        return TRUE;
    }

    toolbox_container_start_async(toolbox, on_start_done, invocation);
    return TRUE;
}

static void
on_stop_done (GObject      *source,
              GAsyncResult *result,
              gpointer      user_data)
{
    GDBusMethodInvocation *invocation = user_data;
    ToolboxManagerImpl *impl = g_dbus_method_invocation_get_user_data(invocation);

    g_autoptr(GError) error = NULL;
    if (!toolbox_container_stop_finish(TOOLBOX_CONTAINER(source), result, &error))
        g_dbus_method_invocation_return_gerror(invocation, error);
    else {
        toolbox_manager_complete_stop(TOOLBOX_MANAGER(impl), invocation);
        check_running(impl);
    }
}

static gboolean
on_stop(ToolboxManager        *manager,
        GDBusMethodInvocation *invocation,
        const char            *name,
        gpointer               user_data)
{
    ToolboxManagerImpl *impl = TOOLBOX_MANAGER_IMPL(manager);
    ToolboxContainer *toolbox = get_container_by_name(impl, name);
    if (toolbox == NULL) {
        g_dbus_method_invocation_return_error(invocation,
                                              G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                              "No toolbox %s", name);
        return TRUE;
    }

    toolbox_container_stop_async(toolbox, on_stop_done, invocation);
    return TRUE;
}

static void
on_socket_monitor_changed(GFileMonitor      *monitor,
                          GFile             *file,
                          GFile             *other_file,
                          GFileMonitorEvent  event_type,
                          gpointer           user_data)
{
    ToolboxManagerImpl *impl = user_data;
    check_running(impl);
}

ToolboxManager *
toolbox_manager_impl_new(void)
{
    return g_object_new(TOOLBOX_TYPE_MANAGER_IMPL, NULL);
}

void
toolbox_manager_impl_start(ToolboxManagerImpl *impl)
{
    g_autofree char *envroot_path = g_file_get_path(impl->envroot);
    g_autoptr(GError) error = NULL;

    if (!g_file_make_directory_with_parents(impl->envroot, NULL, &error)) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
            g_clear_error(&error);
        } else {
            g_printerr("Can't create %s: %s", envroot_path, error->message);
            return;
        }
    }

    const char *user_runtime_dir = g_get_user_runtime_dir();
    g_autofree char *socket_path = g_build_filename(user_runtime_dir, "libpod/tmp/socket", NULL);
    g_autoptr(GFile) socket_file = g_file_new_for_path(socket_path);

    /* FIXME: disconnect */
    GFileMonitor *monitor = g_file_monitor_directory(socket_file, G_FILE_MONITOR_NONE, NULL, &error);
    g_signal_connect(monitor, "changed",
                     G_CALLBACK(on_socket_monitor_changed), impl);

    toolbox_cleanup_old_mounts(envroot_path);
    check_running(impl);
}

void
toolbox_manager_impl_stop(ToolboxManagerImpl *impl)
{
    GHashTableIter iter;

    g_hash_table_iter_init(&iter, impl->toolboxes);
    gpointer value;
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        ToolboxContainer *toolbox = value;
        if (toolbox->info.pid != 0)
            toolbox_container_unmount(toolbox);
    }
}
