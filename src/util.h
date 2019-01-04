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

gboolean toolbox_unmount_path(const char *path, GError **error);
void toolbox_cleanup_old_mounts(const char *envroot_path);
void toolbox_executable_init(const char *argv0);
char *toolbox_executable_get(const char *name);
