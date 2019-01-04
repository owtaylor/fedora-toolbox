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
 * limitations under the License.
 *
 ***********************************************************************
 *
 * This file implements a FUSE filesystem that exports a filesystem for a
 * "local environment" - see the "local environments for IDE's"
 * specification. The filesystem has two subdirectories ./exe/ and ./raw/
 * which pass through the underlying container either transforming
 * executable files into a stub to execute within the environment, or leaving
 * them untouched.
 *
 * We take a simple approach and make our inode structure have the relative
 * path to the file. A different approach would be that of pasthrough_ll.c
 * example in the libfuse sources - keep an open file descriptor for each
 * cached inode. The problem with this is high file descriptor usage -
 * the kernel may cache many dentries, with the result that FUSE will keep
 * the corresponding inodes alive - possibly exceedingthe rlimit for the
 * process. A small file descriptor cache might help improve performance,
 * especially for getxattr.
 */

#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <fuse_lowlevel.h>
#include <glib.h>

#define ENTRY_TIMEOUT 1.0
#define ATTR_TIMEOUT 1.0

typedef struct _EnvfsInode EnvfsInode;
typedef struct _EnvfsDirHandle EnvfsDirHandle;
typedef struct _EnvfsToolbox EnvfsToolbox;

typedef enum {
    ENVFS_INODE_ROOT,
    ENVFS_INODE_OTHER,
} EnvfsInodeType;

struct _EnvfsInode {
    atomic_uint_least64_t refcount;
    EnvfsInodeType type;
    char *path;
    bool is_raw;
};

struct _EnvfsDirHandle {
    EnvfsInodeType type;
    DIR *dir;
    off_t offset;
};

typedef struct {
    int fd;
} EnvfsFd;

static GMutex envfs_mutex;  /* Locks global state. */

static GHashTable *inodes;
static EnvfsInode *root_inode;

static const char *source_path;
static int source_fd = -1;
static const char *mount_path;
static const char *toolbox_run_path;

#define DEBUG 0
#if DEBUG
#include <stdarg.h>

static void
trace(EnvfsInode *inode,
      const char *format,
    ...)
{
    va_list ap;

    if (inode->type == ENVFS_INODE_ROOT)
        fprintf(stderr, "Root: ");
    else if (inode->path)
        fprintf(stderr, "%s (%s): ", inode->path, inode->is_raw ? "raw" : "exe");
    else
        fprintf(stderr, ". (%s): ", inode->is_raw ? "raw" : "exe");

    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

#define TRACE(inode, format, ...) \
  trace (inode, format __VA_OPT__(,) __VA_ARGS__)
#else
#define TRACE(inode, format, ...) ((void)0)
#endif

static inline EnvfsInode *
get_inode(fuse_ino_t ino)
{
    if (ino == FUSE_ROOT_ID)
        return root_inode;
    else
        return (EnvfsInode *)ino;
}

static EnvfsInode *
envfs_inode_new(EnvfsInodeType type)
{
    EnvfsInode *inode = g_new0(EnvfsInode, 1);
    inode->type = type;
    atomic_store(&inode->refcount, 1);

    return inode;
}

static guint
envfs_inode_hash(const void *key)
{
    const EnvfsInode *inode = key;

    return inode->is_raw * 60013 + (inode->path ? g_str_hash(inode->path) : 0);
}

static gboolean
envfs_inode_equal(gconstpointer a,
                  gconstpointer b)
{
    const EnvfsInode *inode_a = a;
    const EnvfsInode *inode_b = b;

    return g_strcmp0(inode_a->path, inode_b->path) == 0 &&
           inode_a->is_raw == inode_b->is_raw;
}

static EnvfsFd
envfs_inode_get_fd(EnvfsInode *inode)
{
    EnvfsFd fd;
    if (inode->path)
        fd.fd = openat(source_fd, inode->path, O_PATH | O_NOFOLLOW);
    else
        fd.fd = source_fd;

    return fd;
}

static int
envfs_inode_stat(EnvfsInode *inode, struct stat *statbuf)
{
    switch (inode->type) {
        case ENVFS_INODE_ROOT:
            statbuf->st_ino = 1;
            statbuf->st_mode = S_IFDIR | 0755;
            statbuf->st_nlink = 4; /* number of subdirs + 2 */
            statbuf->st_uid = getuid();
            statbuf->st_gid = getgid();
            // statbuf.st_atim = x;
            // statbuf.st_mtim = x;
            break;
        case ENVFS_INODE_OTHER:
          {
            int res = fstatat(source_fd, inode->path, statbuf, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
            if (res == -1)
                return -1;

            if (!inode->is_raw &&
                S_ISREG(statbuf->st_mode) &&
                (statbuf->st_mode & 0100) != 0)
            {
                res = lstat(toolbox_run_path, statbuf);
                if (res == -1)
                    return -1;
            }

            statbuf->st_mode &= ~0222;
          }
    }

    return 0;
}

static inline void
envfs_fd_clear(EnvfsFd *fd)
{
    if (fd->fd != -1 && fd->fd != source_fd)
        close(fd->fd);
    fd->fd = -1;
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(EnvfsFd, envfs_fd_clear)

/* Uses or frees path */
static EnvfsInode *
envfs_lookup_inode(char *path, bool is_raw)
{
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&envfs_mutex);

    EnvfsInode tmp_inode = { 0 };
    tmp_inode.path = path;
    tmp_inode.is_raw = is_raw;

    EnvfsInode *inode = g_hash_table_lookup(inodes, &tmp_inode);
    if (inode) {
        atomic_fetch_add(&inode->refcount, 1);
        if (path)
            free(path);
    } else {
        inode = envfs_inode_new(ENVFS_INODE_OTHER);
        inode->path = path;
        inode->is_raw = is_raw;
        g_hash_table_insert(inodes, inode, inode);
    }

    return inode;
}

static void
envfs_lookup_root(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    bool is_raw;
    g_autofree char *base_name = NULL;
    EnvfsInode *inode;
    struct fuse_entry_param param = { 0 };

    if (strcmp(name, "raw") == 0) {
        is_raw = true;
    } else if (strcmp(name, "exe") == 0) {
        is_raw = false;
    } else {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (fstat(source_fd, &param.attr) == -1) {
        fuse_reply_err(req, errno);
        return;
    }

    inode = envfs_lookup_inode(NULL, is_raw);

    param.attr.st_mode &= ~0222;

    param.ino = (uintptr_t)inode;
    param.generation = 1;
    param.attr_timeout = ATTR_TIMEOUT;
    param.entry_timeout = ENTRY_TIMEOUT;
    fuse_reply_entry(req, &param);
}

static void
envfs_lookup_other(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    EnvfsInode *parent_inode = get_inode(parent);
    EnvfsInode *inode;
    struct fuse_entry_param param = { 0 };
    g_autofree char *path = NULL;

    if (parent_inode->path) {
        path = g_build_filename(parent_inode->path, name, NULL);
    } else {
        path = g_strdup(name);
    }

    int res = fstatat(source_fd, path, &param.attr, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
    if (res == -1) {
        fuse_reply_err(req, errno);
        return;
    }

    if (!parent_inode->is_raw &&
        S_ISREG(param.attr.st_mode) &&
        (param.attr.st_mode & 0100) != 0)
    {
        g_free(path);
        path = strdup(toolbox_run_path);

        res = fstatat(source_fd, path, &param.attr, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
        if (res == -1) {
            fuse_reply_err(req, errno);
            return;
        }
    }

    param.attr.st_mode &= ~0222;

    inode = envfs_lookup_inode(g_steal_pointer(&path), parent_inode->is_raw);

    param.ino = (uintptr_t)inode;
    param.attr_timeout = ATTR_TIMEOUT;
    param.entry_timeout = ENTRY_TIMEOUT;
    fuse_reply_entry(req, &param);
}

static void
envfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    EnvfsInode *parent_inode = get_inode(parent);
    TRACE(parent_inode, "lookup %s", name);

    switch (parent_inode->type) {
        case ENVFS_INODE_ROOT:
            envfs_lookup_root(req, parent, name);
            break;
        case ENVFS_INODE_OTHER:
            envfs_lookup_other(req, parent, name);
            break;
        break;
    }
}

static void
envfs_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
    EnvfsInode *inode = get_inode(ino);
    if (inode->type == ENVFS_INODE_ROOT)
        return;

    guint64 after = atomic_fetch_sub(&inode->refcount, nlookup);
    if (after == nlookup) {
        g_mutex_lock(&envfs_mutex);
        g_hash_table_remove(inodes, inode);
        g_mutex_unlock(&envfs_mutex);
        g_free(inode->path);
        g_free(inode);
    }
}

static void
envfs_forget_multi(fuse_req_t req, size_t count, struct fuse_forget_data *forgets)
{
    size_t i;
    for (i = 0; i != count; i++) {
        envfs_forget(NULL, forgets[i].ino, forgets[i].nlookup);
    }
}

static void
envfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    EnvfsInode *inode = get_inode(ino);
    TRACE(inode, "getattr");
    struct stat statbuf = { 0 };

    if (envfs_inode_stat(inode, &statbuf) == -1) {
        fuse_reply_err(req, errno);
        return;
    }

    fuse_reply_attr(req, &statbuf, ATTR_TIMEOUT);
}

static void
envfs_readlink(fuse_req_t req, fuse_ino_t ino)
{
    EnvfsInode *inode = get_inode(ino);
    TRACE(inode, "readlink");

    char buf[PATH_MAX + 1];
    buf[PATH_MAX] = '\0';

    switch (inode->type) {
    case ENVFS_INODE_ROOT:
        fuse_reply_err(req, EINVAL);
        return;
    case ENVFS_INODE_OTHER:
      {
        ssize_t res = readlinkat(source_fd, inode->path, buf, PATH_MAX);
        if (res == -1) {
            fuse_reply_err(req, errno);
            return;
        }

        buf[res] = '\0';
        fuse_reply_readlink(req, buf);
      }
    }
}

static void
envfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    EnvfsInode *inode = get_inode(ino);
    TRACE(inode, "open %x", fi->flags);

    if (fi->flags & (O_WRONLY | O_RDWR)) {
        fuse_reply_err(req, EACCES);
        return;
    }

    if (inode->type != ENVFS_INODE_OTHER) {
        fuse_reply_err(req, EISDIR);
        return;
    }

    int res;
    if (inode->path) {
        res = openat(source_fd, inode->path, fi->flags);
    } else {
        char buf[64];
        sprintf(buf, "/proc/self/fd/%d", source_fd);
        res = open(buf, fi->flags);
    }
    if (res == -1) {
        fuse_reply_err(req, errno);
        return;
    }

    fi->fh = res;

    fuse_reply_open(req, fi);
}

static void
envfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
#if DEBUG
    EnvfsInode *inode = get_inode(ino);
    TRACE(inode, "read");
#endif

    struct fuse_bufvec bufvec = FUSE_BUFVEC_INIT(size);
    bufvec.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    bufvec.buf[0].fd = fi->fh;
    bufvec.buf[0].pos = off;

    fuse_reply_data(req, &bufvec, FUSE_BUF_SPLICE_MOVE);
}

static void
envfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
#if DEBUG
    EnvfsInode *inode = get_inode(ino);
    TRACE(inode, "release");
#endif

    /* Closing a file open only for reading is not expected to produce errors;
     * and to return an error to the filesystem level, we'd need to
     * implement .flush anyways
     * */
    close(fi->fh);
    fuse_reply_err(req, 0);
}

static void
envfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    EnvfsInode *inode = get_inode(ino);
    TRACE(inode, "opendir");

    EnvfsDirHandle *handle;
    int fd = -1;

    switch (inode->type) {
    case ENVFS_INODE_ROOT:
        break;
    case ENVFS_INODE_OTHER:
      {
        fd = openat(source_fd, inode->path ? inode->path : ".", O_RDONLY | O_NONBLOCK | O_DIRECTORY);
        if (fd == -1) {
            fuse_reply_err(req, errno);
            return;
        }
        break;
      }
    }

    DIR *dir = NULL;

    if (fd != -1) {
        dir = fdopendir(fd);
        if (!dir) {
            fuse_reply_err(req, errno);
            (void)close(fd);
            return;
        }
    }

    handle = g_new0(EnvfsDirHandle, 1);
    handle->type = inode->type;
    handle->dir = dir;
    handle->offset = 0;

    fi->fh = (uintptr_t)handle;
    fuse_reply_open(req, fi);
}

static void
envfs_readdir_other(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
    EnvfsDirHandle *handle = (EnvfsDirHandle *)fi->fh;
    g_autofree char *buf = g_try_malloc(size);
    if (!buf) {
        fuse_reply_err(req, ENOMEM);
        return;
    }

    if (handle->offset != off) {
        seekdir(handle->dir, off);
        handle->offset = off;
    }

    ssize_t buf_used = 0;
    while (true) {
        struct dirent *e;

        errno = 0;
        e = readdir(handle->dir);
        if (!e) {
            if (errno != 0) {
                fuse_reply_err(req, errno);
                return;
            }

            break;
        }

        handle->offset = e->d_off;

        struct stat stbuf = { 0 };

        stbuf.st_ino = e->d_ino;
        stbuf.st_mode = e->d_type << 12;

        size_t remaining = size - buf_used;
        size_t this_used = fuse_add_direntry(req, buf + buf_used, remaining, e->d_name, &stbuf, e->d_off);
        if (remaining < this_used)
            break;

        buf_used += this_used;
    }

    fuse_reply_buf(req, buf, buf_used);
}

static void
envfs_readdir_root(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
    g_autofree char *buf = g_try_malloc(size);
    if (!buf) {
        fuse_reply_err(req, ENOMEM);
        return;
    }

    ssize_t buf_used = 0;
    while (true) {
        const char *name;
        g_autofree char *free_name = NULL;
        int type;
        if (off == 0) {
            name = ".";
            type = DT_DIR;
        } else if (off == 1) {
            name = "..";
            type = DT_DIR;
        } else if (off == 2) {
            name = "exe";
            type = DT_DIR;
        } else if (off == 3) {
            name = "raw";
            type = DT_DIR;
        } else {
            break;
        }

        off += 1;

        struct stat stbuf = { 0 };
        stbuf.st_ino = off;
        stbuf.st_mode = type << 12;

        size_t remaining = size - buf_used;
        size_t this_used = fuse_add_direntry(req, buf + buf_used, remaining, name, &stbuf, off);
        if (remaining < this_used)
            break;
        buf_used += this_used;
    }

    fuse_reply_buf(req, buf, buf_used);
}

static void
envfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
#if DEBUG
    EnvfsInode *inode = get_inode(ino);
    TRACE(inode, "readdir %zd", off);
#endif

    EnvfsDirHandle *handle = (EnvfsDirHandle *)fi->fh;
    switch (handle->type) {
        case ENVFS_INODE_ROOT:
            envfs_readdir_root(req, ino, size, off, fi);
            break;
        case ENVFS_INODE_OTHER:
            envfs_readdir_other(req, ino, size, off, fi);
            break;
    }
}

static void
envfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
#if DEBUG
    EnvfsInode *inode = get_inode(ino);
    TRACE(inode, "releasedir");
#endif

    EnvfsDirHandle *handle = (EnvfsDirHandle *)fi->fh;
    switch (handle->type) {
        case ENVFS_INODE_ROOT:
            break;
        case ENVFS_INODE_OTHER:
            if (closedir(handle->dir) == -1)
                fuse_reply_err(req, errno);
            else
                fuse_reply_err(req, 0);
            break;
    }

    g_free(handle);
}

static void
envfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size)
{
    EnvfsInode *inode = get_inode(ino);
    TRACE(inode, "getxattr %s", name);

    g_autofree char *buf = NULL;
    if (size != 0) {
        buf = g_try_malloc(size);
        if (!buf) {
            fuse_reply_err(req, errno);
            return;
        }
    }

    switch (inode->type) {
        case ENVFS_INODE_ROOT:
            fuse_reply_err(req, ENODATA);
            return;
        case ENVFS_INODE_OTHER:
          {
            EnvfsFd fd = envfs_inode_get_fd(inode);
            if (fd.fd == -1) {
                fuse_reply_err(req, errno);
                return;
            }
            char buf[64];
            sprintf(buf, "/proc/self/fd/%d", fd.fd);
            ssize_t res = getxattr(buf, name, buf, size);
            if (res == -1) {
                fuse_reply_err(req, errno);
            } else {
                fuse_reply_buf(req, buf, res);
            }
            return;
          }
    }
}

static void
envfs_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
    EnvfsInode *inode = get_inode(ino);
    TRACE(inode, "listxattr");

    g_autofree char *buf = 0;
    if (size != 0) {
        buf = g_try_malloc(size);
        if (!buf) {
            fuse_reply_err(req, errno);
            return;
        }
    }

    switch (inode->type) {
        case ENVFS_INODE_ROOT:
            fuse_reply_buf(req, buf, 0);
            return;
        case ENVFS_INODE_OTHER:
          {
            EnvfsFd fd = envfs_inode_get_fd(inode);
            if (fd.fd == -1) {
                fuse_reply_err(req, errno);
                return;
            }
            ssize_t res = flistxattr(fd.fd, NULL, size);
            if (res == -1)
                fuse_reply_err(req, errno);
            else
                fuse_reply_buf(req, buf, res);
            return;
          }
    }
}

static void
envfs_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
    EnvfsInode *inode = get_inode(ino);
    TRACE(inode, "access %x", mask);

    if ((mask & W_OK) != 0) {
        fuse_reply_err(req, EACCES);
        return;
    }

    switch (inode->type) {
        case ENVFS_INODE_ROOT:
            fuse_reply_err(req, 0);
            return;
        case ENVFS_INODE_OTHER:
          {
            g_auto(EnvfsFd) fd = envfs_inode_get_fd(inode);
            if (fd.fd == -1) {
                fuse_reply_err(req, errno);
                return;
            }
            char buf[64];
            sprintf(buf, "/proc/self/fd/%d", fd.fd);
            if (access(buf, mask) == -1)
                fuse_reply_err(req, errno);
            else
                fuse_reply_err(req, 0);
            break;
          }
    }
}

struct fuse_lowlevel_ops ops = {
    .lookup = envfs_lookup,
    .forget = envfs_forget,
    .getattr = envfs_getattr,
    .readlink = envfs_readlink,
    .open = envfs_open,
    .read = envfs_read,
    .release = envfs_release,
    .opendir = envfs_opendir,
    .readdir = envfs_readdir,
    .releasedir = envfs_releasedir,
    .getxattr = envfs_getxattr,
    .listxattr = envfs_listxattr,
    .access = envfs_access,
    .forget_multi = envfs_forget_multi,
};

static void
usage(void)
{
    fprintf(stderr, "Usage: toolbox-envfsd CONTAINER_PID MOUNT_PATH TOOLBOX_RUN_PATH\n");
}

static bool
enter_namespace(const char *ns_path)
{
    g_auto(EnvfsFd) ns_fd = { -1 };
    ns_fd.fd = open(ns_path, O_RDONLY);
    if (ns_fd.fd == -1) {
      perror("Opening namespace");
      return false;
    }

    int res = setns(ns_fd.fd, 0);
    if (res == -1) {
      perror("setns");
      return false;
    }

    return true;
}

int
main(int argc, char **argv)
{
    char *fixed_argv[] = {
        "-oro"
    };
    int fixed_argc = sizeof(fixed_argv) / sizeof(fixed_argv[0]);
    struct fuse_args args = FUSE_ARGS_INIT(fixed_argc, fixed_argv);
    int container_pid;
    char *endptr;

    if (argc != 4) {
        usage();
        return 1;
    }

    container_pid = strtod(argv[1], &endptr);
    if (endptr == argv[1] || *endptr != '\0') {
        usage();
        return 1;
    }

    source_path = g_strdup_printf("/proc/%d/root", container_pid);
    source_fd = open(source_path, O_PATH);
    if (source_fd == -1) {
        perror("Unable to open source path");
        return 1;
    }

    g_autofree char *ns_path = g_strdup_printf("/proc/%d/ns/user", container_pid);

    mount_path = argv[2];
    struct fuse_chan *chan = fuse_mount(mount_path, &args);
    if (!chan) {
        fprintf(stderr, "Failed to create mount channel");
        return 1;
    }

    toolbox_run_path = argv[3];

    struct fuse_session *session = fuse_lowlevel_new(&args, &ops, sizeof(ops), NULL);
    if (!session)
        return 1;

    if (fuse_set_signal_handlers(session) == -1) {
        fprintf(stderr, "Failed to set signal handlers");
        return 1;
    }

    inodes = g_hash_table_new (envfs_inode_hash, envfs_inode_equal);
    root_inode = envfs_inode_new(ENVFS_INODE_ROOT);

    fuse_session_add_chan(session, chan);

    if (enter_namespace(ns_path))
        fuse_session_loop_mt(session);

    fuse_session_remove_chan(chan);

    fuse_session_destroy(session);
    fuse_unmount(mount_path, chan);

    return 0;
}
