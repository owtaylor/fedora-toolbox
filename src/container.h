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

#pragma once
#include <gio/gio.h>

typedef struct {
    char *id;
    char *name;
    int pid;
} ToolboxInfo;

struct _ToolboxContainer {
    GObject object;

    ToolboxInfo info;
    GFile *envroot;

    GSList *pending_start_tasks;
    GSList *pending_stop_tasks;

    GSubprocess *fuse_subprocess;
};

#define TOOLBOX_TYPE_CONTAINER toolbox_container_get_type()

G_DECLARE_FINAL_TYPE (ToolboxContainer, toolbox_container, TOOLBOX, CONTAINER, GObject)

ToolboxContainer *toolbox_container_new(GFile       *envroot,
                                        ToolboxInfo *info);
void toolbox_container_start_async(ToolboxContainer *toolbox,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data);
gboolean toolbox_container_start_finish(ToolboxContainer *toolbox,
                                        GAsyncResult *result,
                                        GError **error);
void toolbox_container_stop_async(ToolboxContainer *toolbox,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
gboolean toolbox_container_stop_finish(ToolboxContainer *toolbox,
                                    GAsyncResult *result,
                                       GError **error);
void toolbox_container_mount(ToolboxContainer *toolbox);
void toolbox_container_unmount(ToolboxContainer *toolbox);
void toolbox_container_update(ToolboxContainer *toolbox,
                              ToolboxInfo      *info);
