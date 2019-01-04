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

#include <gio/gunixmounts.h>

#include <sys/stat.h>
#include <stdio.h>

#include "util.h"

gboolean
toolbox_unmount_path(const char *path, GError **error)
{
    g_autoptr(GSubprocess) subprocess = g_subprocess_new(
      G_SUBPROCESS_FLAGS_NONE, error, "fusermount", "-u", "-q", "-z", path, NULL);
    if (!subprocess)
        return FALSE;

    return g_subprocess_wait_check(subprocess, NULL, error);
}

void
toolbox_cleanup_old_mounts(const char *envroot_path)
{
    g_autoptr(GError) error = NULL;

    struct stat envroot_statbuf;
    if (stat(envroot_path, &envroot_statbuf) == -1) {
        perror("stat envroot");
        return;
    }

    GList *mounts = g_unix_mounts_get(NULL);
    for (GList *l = mounts; l; l = l->next) {
        GUnixMountEntry *entry = l->data;
        const char *mount_path = g_unix_mount_get_mount_path(entry);

        g_autoptr(GFile) mount_file = g_file_new_for_path(mount_path);
        g_autoptr(GFile) mount_parent = g_file_get_parent(mount_file);
        if (mount_parent == NULL)
            continue;

        g_autofree char *parent_path = g_file_get_path(mount_parent);
        struct stat parent_statbuf;
        if (stat(parent_path, &parent_statbuf) == -1)
            continue;

        if (parent_statbuf.st_ino == envroot_statbuf.st_ino &&
            parent_statbuf.st_dev == envroot_statbuf.st_dev)
        {
            g_printerr("Found old mount at %s, unmounting\n", mount_path);
            if (!toolbox_unmount_path(mount_path, &error)) {
                g_printerr("Failed to unmount %s: %s", mount_path, error->message);
                g_clear_error(&error);
            }
        }
    }
    g_list_free_full(mounts, (GDestroyNotify)g_unix_mount_free);
}

static char *toolboxd_dir = NULL;
static char *alt_dir = NULL;

void
toolbox_executable_init(const char *argv0)
{
    toolboxd_dir = g_path_get_dirname(argv0);
    g_autoptr(GFile) iter = g_file_new_for_path(toolboxd_dir);
    while (TRUE) {
        g_autoptr(GFile) parent = g_file_get_parent(iter);
        if (parent == NULL)
            break;
        g_clear_object(&iter);
        iter = g_steal_pointer(&parent);

        g_autofree char *basename = g_file_get_basename(iter);
        if (strcmp(basename, "toolbox") == 0) {
            g_autoptr(GFile) copying = g_file_get_child(iter, "COPYING");
            if (g_file_query_exists(copying, NULL))
                alt_dir = g_file_get_path(iter);
        }
    }
}

char *
toolbox_executable_get(const char *name)
{
    g_autofree char *path = g_build_filename(toolboxd_dir, name, NULL);
    if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
        return g_steal_pointer(&path);
    g_clear_pointer(&path, (GDestroyNotify)g_free);

    path = g_build_filename(alt_dir, name, NULL);
    if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
        return g_steal_pointer(&path);

    g_printerr("Failed to find %s\n", name);
    return NULL;
}
