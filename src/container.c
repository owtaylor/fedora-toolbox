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

#include "container.h"
#include "util.h"

struct _ToolboxContainerClass {
    GObjectClass object_class;
};

G_DEFINE_TYPE(ToolboxContainer, toolbox_container, G_TYPE_OBJECT);

static void
toolbox_container_finalize(GObject *object)
{
    ToolboxContainer *container = TOOLBOX_CONTAINER(object);

    g_assert(container->pending_start_tasks == NULL);
    g_assert(container->pending_stop_tasks == NULL);

    g_free(container->info.name);
    g_free(container->info.id);
}

static void
toolbox_container_class_init(ToolboxContainerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = toolbox_container_finalize;
}

static void
toolbox_container_init(ToolboxContainer *container)
{
}

ToolboxContainer *
toolbox_container_new(GFile       *envroot,
                      ToolboxInfo *info)
{
    ToolboxContainer *container = g_object_new(TOOLBOX_TYPE_CONTAINER, NULL);

    container->envroot = g_object_ref(envroot);
    container->info.name = g_strdup(info->name);
    container->info.id = g_strdup(info->id);
    container->info.pid = info->pid;

    return container;
}

static void
finish_tasks(GSList *tasks,
             GError *error)
{
    for (GSList *l = tasks; l; l = l->next) {
        if (error)
            g_task_return_error(l->data, g_error_copy(error));
        else
            g_task_return_boolean(l->data, TRUE);
    }

    g_slist_free(tasks);
}

static void
on_start_process_done (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
    ToolboxContainer *toolbox = user_data;

    GSList *tasks = g_steal_pointer(&toolbox->pending_start_tasks);

    g_autoptr(GError) error = NULL;
    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error)) {
        g_printerr("podman start failed: %s\n", error->message);
        finish_tasks(tasks, error);
    } else {
        g_printerr("Container started\n");
        finish_tasks(tasks, NULL);
    }
}

void
toolbox_container_start_async(ToolboxContainer *toolbox,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
    g_autoptr(GTask) task = g_task_new(toolbox, NULL, callback, user_data);
    GError *error = NULL;

    if (toolbox->info.pid != 0) {
        g_task_return_boolean(task, TRUE);
        return;
    }

    if (toolbox->pending_start_tasks) {
        toolbox->pending_start_tasks =
          g_slist_prepend(toolbox->pending_start_tasks, g_object_ref(task));
        return;
    }

    g_autoptr(GSubprocess) subprocess = g_subprocess_new(
        G_SUBPROCESS_FLAGS_NONE, &error, "podman", "start", toolbox->info.name, NULL);

    if (subprocess == NULL) {
        g_task_return_error(task, error);
        return;
    }

    toolbox->pending_start_tasks = g_slist_prepend(toolbox->pending_start_tasks, g_steal_pointer(&task));
    g_subprocess_wait_check_async(subprocess, NULL, on_start_process_done, toolbox);
}

gboolean
toolbox_container_start_finish(ToolboxContainer *toolbox,
                               GAsyncResult *result,
                               GError **error)
{
    GTask *task = G_TASK(result);

    return g_task_propagate_boolean(task, error);
}

static void
on_stop_process_done (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
    ToolboxContainer *toolbox = user_data;

    GSList *tasks = g_steal_pointer(&toolbox->pending_stop_tasks);

    g_autoptr(GError) error = NULL;
    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error)) {
        g_printerr("podman stop failed: %s\n", error->message);
        finish_tasks(tasks, error);
    } else {
        g_printerr("Container stopped\n");
        finish_tasks(tasks, NULL);
    }
}

void
toolbox_container_stop_async(ToolboxContainer *toolbox,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
    g_autoptr(GTask) task = g_task_new(toolbox, NULL, callback, user_data);
    GError *error = NULL;

    if (toolbox->info.pid == 0) {
        g_task_return_boolean(task, TRUE);
        return;
    }

    if (toolbox->pending_stop_tasks) {
        toolbox->pending_stop_tasks =
          g_slist_prepend(toolbox->pending_stop_tasks, g_object_ref(task));
        return;
    }

    g_autoptr(GSubprocess) subprocess = g_subprocess_new(
        G_SUBPROCESS_FLAGS_NONE, &error, "podman", "stop", toolbox->info.name, NULL);

    if (subprocess == NULL) {
        g_task_return_error(task, error);
        return;
    }

    toolbox->pending_stop_tasks = g_slist_prepend(toolbox->pending_stop_tasks, g_steal_pointer(&task));
    g_subprocess_wait_check_async(subprocess, NULL, on_stop_process_done, toolbox);
}

gboolean
toolbox_container_stop_finish(ToolboxContainer *toolbox,
                              GAsyncResult *result,
                              GError **error)
{
    GTask *task = G_TASK(result);

    return g_task_propagate_boolean(task, error);
}

void
toolbox_container_mount(ToolboxContainer *toolbox)
{
    g_autoptr(GError) error = NULL;

    g_return_if_fail(toolbox->fuse_subprocess == NULL);
    g_return_if_fail(toolbox->info.pid != 0);

    g_autofree char *pid_string = g_strdup_printf("%d", toolbox->info.pid);
    g_autoptr(GFile) mount_file = g_file_get_child(toolbox->envroot, toolbox->info.name);
    g_autofree char *mount_path = g_file_get_path(mount_file);

    g_autofree char *envfs_path = toolbox_executable_get("toolbox-envfs");
    g_autofree char *toolbox_run_path = toolbox_executable_get("toolbox-run");
    toolbox->fuse_subprocess = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
                                                envfs_path, pid_string, mount_path, toolbox_run_path, NULL);
    if (toolbox->fuse_subprocess == NULL)
        g_printerr("Failed to mount %s: %s", toolbox->info.name, error->message);
    else
        g_printerr("Mounted %s on %s\n", toolbox->info.name, mount_path);
}

void
toolbox_container_unmount(ToolboxContainer *toolbox)
{
    g_autoptr(GError) error = NULL;

    g_return_if_fail(toolbox->fuse_subprocess != NULL);

    g_autoptr(GFile) mount_file = g_file_get_child(toolbox->envroot, toolbox->info.name);
    g_autofree char *mount_path = g_file_get_path(mount_file);

    if (!toolbox_unmount_path(mount_path, &error)) {
        g_printerr("Failed to unmount %s: %s\n", toolbox->info.name, error->message);
        return;
    }

    if (!g_subprocess_wait(toolbox->fuse_subprocess, NULL, &error)) {
        g_printerr("Failed to wait for fuse exit: %s\n", error->message);
        return;
    }

    toolbox->fuse_subprocess = NULL;
    g_printerr("Unmounted %s\n", toolbox->info.name);
}

void
toolbox_container_update(ToolboxContainer *toolbox,
                         ToolboxInfo      *info)
{
    if (strcmp(info->id, toolbox->info.id) != 0) {
        g_printerr("%s: Update ID: %s => %s\n", toolbox->info.name, toolbox->info.id, info->id);
        g_free(toolbox->info.id);
        toolbox->info.id = g_strdup(info->id);
    }

    if (info->pid != toolbox->info.pid) {
        g_printerr("%s: Update Pid: %d => %d\n", toolbox->info.name, toolbox->info.pid, info->pid);
        if (toolbox->info.pid)
            toolbox_container_unmount(toolbox);
        toolbox->info.pid = info->pid;
        if (toolbox->info.pid)
            toolbox_container_mount(toolbox);
    }
}
