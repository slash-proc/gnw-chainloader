/*
 * File-operations module (PIE, transient).
 *
 * The heavy file-tree algorithms the in-core browser used to carry: recursive
 * folder copy (with self-copy guard + free-space pre-flight), recursive delete,
 * and the tree-size walk. The browser loads this on demand, runs one op, and
 * reclaims the slot (mod_pool_mark/reset), so the 40 KiB core never carries it.
 *
 * The module reaches the filesystems through the ALREADY-MOUNTED vfs_driver_t
 * pointers the core hands it (their opendir/readdir/mkdir/unlink/statfs are
 * callable directly). It never mounts/unmounts or loads drivers -- the core owns
 * that lifecycle. Leaf-file copies and the cancel/progress UI go back through the
 * host so the single 4 KiB copy buffer + the translated progress strings stay in
 * the core. Stateless across calls except for the per-op host pointer + counter.
 */
#include "system/module.h"
#include "system/fileops.h"
#include "storage/vfs.h"
#include <string.h>

MODULE_HEADER;

#define MAX_COPY_DEPTH 8   /* recursion guard (matches the browser's nav stack) */

/* Per-op state (single-active: one file op runs at a time). */
static const fileops_host_t *g_h;
static int g_size_count;    /* files counted so far during the pre-flight walk */

/* base + "/" + name -> out (cap bytes). Returns false if it wouldn't fit. */
static bool join_path(char *out, uint32_t cap, const char *base, const char *name) {
    uint32_t bl = strlen(base), nl = strlen(name);
    int slash = (bl > 0 && base[bl - 1] != '/') ? 1 : 0;
    if (bl + slash + nl + 1 > cap) return false;
    strcpy(out, base);
    if (slash) { out[bl] = '/'; out[bl + 1] = '\0'; }
    strcat(out, name);
    return true;
}

/* The basename of a path (for the single-file copy display name). */
static const char *base_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

/* Read the n-th real entry (skipping "." and "..") of `path`, re-opening the dir
 * each call so no handle is held across recursion (driver dir pools are shallow).
 * 1 = found (ent filled), 0 = past the end, -1 = opendir failure. */
static int read_nth_entry(vfs_driver_t *drv, const char *path, int n, vfs_dirent_t *ent) {
    void *dir = NULL;
    if (drv->opendir(path, &dir) != 0) return -1;
    int count = 0, found = 0;
    vfs_dirent_t e;
    while (drv->readdir(dir, &e) > 0) {
        if (e.name[0] == '.' && (e.name[1] == '\0' ||
            (e.name[1] == '.' && e.name[2] == '\0'))) continue;   /* skip "." / ".." */
        if (count == n) { *ent = e; found = 1; break; }
        count++;
    }
    if (drv->closedir) drv->closedir(dir);
    return found ? 1 : 0;
}

/* Progress/cancel callback for the shared in-core copy loop; user = display name. */
static int copy_cb(int pct, void *user) {
    if (g_h->poll()) return 1;                                  /* cancel */
    g_h->progress(pct, FILEOPS_PHASE_COPY, (const char *)user, 0);
    return 0;
}

/* Copy one regular file via the host's in-core copy loop, mapping its result. */
static int copy_one_file(vfs_driver_t *sd, const char *sp, vfs_driver_t *dd,
                         const char *dp, uint32_t size, const char *disp) {
    switch (g_h->copy_open_file(sd, sp, dd, dp, size, copy_cb, (void *)disp)) {
        case VFS_COPY_OK:     return FILEOPS_OK;
        case VFS_COPY_CANCEL: return FILEOPS_CANCEL;
        case VFS_COPY_FULL:   return FILEOPS_DISK_FULL;
        case VFS_COPY_READ:   return FILEOPS_READ_ERR;
        case VFS_COPY_WRITE:  return FILEOPS_WRITE_ERR;
        default:              return FILEOPS_OPEN_ERR;
    }
}

/* Sum the byte sizes of every regular file under `path` (recursively). Sets
 * *ok=false on any error / cancel. Pool-safe via read_nth_entry (re-scan/entry). */
static uint64_t tree_size(vfs_driver_t *drv, const char *path, int depth, bool *ok) {
    if (depth > MAX_COPY_DEPTH) { *ok = false; return 0; }
    uint64_t total = 0;
    vfs_dirent_t ent;
    int n = 0, r;
    while ((r = read_nth_entry(drv, path, n, &ent)) == 1) {
        if (g_h->poll()) { *ok = false; return total; }
        if (ent.type == VFS_TYPE_DIR) {
            char child[512];
            if (!join_path(child, sizeof(child), path, ent.name)) { *ok = false; return total; }
            total += tree_size(drv, child, depth + 1, ok);
            if (!*ok) return total;
        } else {
            total += ent.size;
            g_size_count++;
            g_h->progress((int)((g_h->get_tick() / 20) % 100),
                          FILEOPS_PHASE_CALC, NULL, g_size_count);
        }
        n++;
    }
    if (r < 0) *ok = false;
    return total;
}

/* Recursively copy the directory tree at `sp` (on sd) to `dp` (on dd). */
static int copy_tree(vfs_driver_t *sd, const char *sp, vfs_driver_t *dd,
                     const char *dp, int depth) {
    if (depth > MAX_COPY_DEPTH) return FILEOPS_TOO_DEEP;
    if (dd->mkdir(dp) != 0) return FILEOPS_WRITE_ERR;   /* exists or unwritable */
    vfs_dirent_t ent;
    int n = 0, r;
    while ((r = read_nth_entry(sd, sp, n, &ent)) == 1) {
        if (g_h->poll()) return FILEOPS_CANCEL;
        char sc[512], dc[512];
        if (!join_path(sc, sizeof(sc), sp, ent.name) ||
            !join_path(dc, sizeof(dc), dp, ent.name)) return FILEOPS_PATH_LONG;
        int res = (ent.type == VFS_TYPE_DIR)
            ? copy_tree(sd, sc, dd, dc, depth + 1)
            : copy_one_file(sd, sc, dd, dc, ent.size, ent.name);
        if (res != FILEOPS_OK) return res;
        n++;
    }
    return (r < 0) ? FILEOPS_READ_ERR : FILEOPS_OK;
}

/* Recursively delete a directory depth-first, then remove the now-empty dir.
 * Always re-reads entry 0 because the listing shrinks as we delete. */
static int delete_tree(vfs_driver_t *drv, const char *path, int depth) {
    if (depth > MAX_COPY_DEPTH) return FILEOPS_TOO_DEEP;
    vfs_dirent_t ent;
    int r;
    while ((r = read_nth_entry(drv, path, 0, &ent)) == 1) {
        if (g_h->poll()) return FILEOPS_CANCEL;
        char child[512];
        if (!join_path(child, sizeof(child), path, ent.name)) return FILEOPS_PATH_LONG;
        if (ent.type == VFS_TYPE_DIR) {
            int res = delete_tree(drv, child, depth + 1);
            if (res != FILEOPS_OK) return res;
        } else {
            g_h->progress((int)((g_h->get_tick() / 20) % 100),
                          FILEOPS_PHASE_DELETE, ent.name, 0);
            if (drv->unlink(child) != 0) return FILEOPS_WRITE_ERR;
        }
    }
    if (r < 0) return FILEOPS_READ_ERR;
    return (drv->unlink(path) == 0) ? FILEOPS_OK : FILEOPS_WRITE_ERR;   /* the now-empty dir */
}

static int fileops_copy(const fileops_host_t *h, vfs_driver_t *src, const char *sp,
                        vfs_driver_t *dst, const char *dp, int is_dir, uint32_t size,
                        int same_vol) {
    g_h = h;
    g_size_count = 0;

    if (!is_dir) {
        uint32_t tot = 0, fre = 0;
        if (dst->statfs && dst->statfs(&tot, &fre) == 0 &&
            (uint64_t)size > (uint64_t)fre * 512u) return FILEOPS_NO_SPACE;
        return copy_one_file(src, sp, dst, dp, size, base_of(sp));
    }

    /* Guard against pasting a folder into itself / a descendant on the same volume. */
    if (same_vol) {
        uint32_t sl = strlen(sp);
        if (strncmp(dp, sp, sl) == 0 && (dp[sl] == '/' || dp[sl] == '\0'))
            return FILEOPS_COPY_SELF;
    }

    /* Pre-flight free-space: sum the tree, refuse upfront (all-or-nothing). */
    uint32_t tot = 0, fre = 0;
    if (dst->statfs && dst->statfs(&tot, &fre) == 0) {
        bool ok = true;
        uint64_t need = tree_size(src, sp, 0, &ok);
        if (!ok) return h->poll() ? FILEOPS_CANCEL : FILEOPS_SRC_READ;
        if (need > (uint64_t)fre * 512u) return FILEOPS_NO_SPACE;
    }
    return copy_tree(src, sp, dst, dp, 0);
}

static int fileops_del(const fileops_host_t *h, vfs_driver_t *drv,
                       const char *path, int is_dir) {
    g_h = h;
    if (is_dir) return delete_tree(drv, path, 0);
    g_h->progress(0, FILEOPS_PHASE_DELETE, base_of(path), 0);
    return (drv->unlink(path) == 0) ? FILEOPS_OK : FILEOPS_WRITE_ERR;
}

void init_module(const fileops_host_t *host, fileops_api_t *out) {
    (void)host;   /* stateless: host is passed to copy()/del() per call */
    out->copy = fileops_copy;
    out->del  = fileops_del;
}
