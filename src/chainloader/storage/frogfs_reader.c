/*
 * frogfs_reader.c - Lightweight, read-only FrogFS directory traverser.
 * 
 * Does not use the hash table or dynamic memory allocation. Instead, it reads
 * the directory tree directly from memory-mapped flash.
 */

#include "frogfs_reader.h"
#include <string.h>

#define FROGFS_MAGIC 0x474F5246
#define FROGFS_VER_MAJOR 1

#define FROGFS_IS_DIR(e) ((e)->child_count < 0xFF00)
#define FROGFS_IS_FILE(e) ((e)->child_count >= 0xFF00)
#define FROGFS_IS_COMP(e) ((e)->child_count > 0xFF00)

typedef struct __attribute__((packed)) {
    uint32_t magic; 
    uint8_t ver_major; 
    uint8_t ver_minor; 
    uint16_t num_entries; 
    uint32_t bin_sz; 
} frogfs_head_t;

typedef struct __attribute__((packed)) {
    uint32_t hash; 
    uint32_t offs; 
} frogfs_hash_t;

typedef struct __attribute__((packed)) {
    uint32_t parent; 
    union {
        uint16_t child_count; 
        struct {
            uint8_t compression; 
            uint8_t _reserved;
        };
    };
    uint8_t seg_sz; 
    uint8_t opts; 
} frogfs_entry_t;

typedef struct __attribute__((packed)) {
    const frogfs_entry_t entry;
    uint32_t children[];
} frogfs_dir_t;

static const char *get_name(const frogfs_entry_t *entry) {
    if (FROGFS_IS_DIR(entry)) {
        return (const char *) entry + 8 + (entry->child_count * 4);
    } else if (FROGFS_IS_FILE(entry) && !FROGFS_IS_COMP(entry)) {
        return (const char *) entry + 16;
    } else {
        return (const char *) entry + 20;
    }
}

int frogfs_mount(const void *base_addr) {
    const frogfs_head_t *head = (const frogfs_head_t *)base_addr;
    if (head->magic != FROGFS_MAGIC || head->ver_major != FROGFS_VER_MAJOR) {
        return -1;
    }
    return 0;
}

static const void* frogfs_get_root(const void *base_addr) {
    const frogfs_head_t *head = (const frogfs_head_t *)base_addr;
    const frogfs_hash_t *hash_table = (const frogfs_hash_t *)((const uint8_t *)base_addr + sizeof(frogfs_head_t));
    return (const void *)(hash_table + head->num_entries);
}

int frogfs_dir_open(frogfs_dir_reader_t *dir, const char *path) {
    dir->root_dir = frogfs_get_root(dir->fs_base);
    
    // For root path "/"
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        dir->current_dir = dir->root_dir;
        dir->child_idx = 0;
        const frogfs_dir_t *d = (const frogfs_dir_t *)dir->current_dir;
        dir->child_count = d->entry.child_count;
        return 0;
    }

    const frogfs_dir_t *current = (const frogfs_dir_t *)dir->root_dir;
    const char *p = path;
    if (*p == '/') p++;
    
    while (*p) {
        const char *next_slash = NULL;
        for (const char *s = p; *s; s++) {
            if (*s == '/') {
                next_slash = s;
                break;
            }
        }
        int seg_len = next_slash ? (next_slash - p) : strlen(p);
        
        bool found = false;
        for (int i = 0; i < current->entry.child_count; i++) {
            const frogfs_entry_t *child = (const frogfs_entry_t *)((const uint8_t *)dir->fs_base + current->children[i]);
            if (child->seg_sz == seg_len && memcmp(get_name(child), p, seg_len) == 0) {
                if (!next_slash || *(next_slash + 1) == '\0') {
                    if (FROGFS_IS_DIR(child)) {
                        dir->current_dir = child;
                        dir->child_idx = 0;
                        dir->child_count = child->child_count;
                        return 0;
                    }
                    return -1;
                } else {
                    if (FROGFS_IS_DIR(child)) {
                        current = (const frogfs_dir_t *)child;
                        found = true;
                        break;
                    }
                    return -1; 
                }
            }
        }
        if (!found) return -1;
        p = next_slash + 1;
    }
    return -1;
}

int frogfs_dir_read(frogfs_dir_reader_t *dir, frogfs_info_t *info) {
    if (dir->child_idx >= dir->child_count) {
        return 0; 
    }
    
    const frogfs_dir_t *d = (const frogfs_dir_t *)dir->current_dir;
    const frogfs_entry_t *child = (const frogfs_entry_t *)((const uint8_t *)dir->fs_base + d->children[dir->child_idx]);
    
    info->type = FROGFS_IS_DIR(child) ? FROGFS_TYPE_DIR : FROGFS_TYPE_REG;
    int copy_len = child->seg_sz < sizeof(info->name) - 1 ? child->seg_sz : sizeof(info->name) - 1;
    memcpy(info->name, get_name(child), copy_len);
    info->name[copy_len] = '\0';
    
    if (FROGFS_IS_FILE(child)) {
        if (FROGFS_IS_COMP(child)) {
            uint32_t *real_sz_ptr = (uint32_t *)((const uint8_t *)child + 16);
            info->size = *real_sz_ptr;
        } else {
            uint32_t *data_sz_ptr = (uint32_t *)((const uint8_t *)child + 12);
            info->size = *data_sz_ptr;
        }
    } else {
        info->size = 0;
    }
    
    dir->child_idx++;
    return 1;
}

int frogfs_dir_close(frogfs_dir_reader_t *dir) {
    return 0;
}

const void* frogfs_get_file_data(const void *fs_base, const char *path, uint32_t *out_size) {
    frogfs_dir_reader_t dir;
    dir.fs_base = fs_base;
    
    static char parent[128];
    static char fname[64];
    
    const char *last_slash = NULL;
    for (const char *s = path; *s; s++) {
        if (*s == '/') last_slash = s;
    }
    if (!last_slash) return NULL;
    
    int parent_len = last_slash - path;
    if (parent_len == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        memcpy(parent, path, parent_len);
        parent[parent_len] = '\0';
    }
    
    strcpy(fname, last_slash + 1);
    
    if (frogfs_dir_open(&dir, parent) < 0) return NULL;
    
    const frogfs_dir_t *d = (const frogfs_dir_t *)dir.current_dir;
    for (int i = 0; i < d->entry.child_count; i++) {
        const frogfs_entry_t *child = (const frogfs_entry_t *)((const uint8_t *)dir.fs_base + d->children[i]);
        if (FROGFS_IS_FILE(child)) {
            const char *name = get_name(child);
            int fname_len = strlen(fname);
            if (child->seg_sz == fname_len && memcmp(name, fname, fname_len) == 0) {
                if (FROGFS_IS_COMP(child)) {
                    return NULL; // Compressed file direct access unsupported
                }
                uint32_t offset = *(const uint32_t *)((const uint8_t *)child + 8);
                *out_size = *(const uint32_t *)((const uint8_t *)child + 12);
                return (const uint8_t *)dir.fs_base + offset;
            }
        }
    }
    return NULL;
}

#include "vfs.h"

static frogfs_dir_reader_t frogfs_vfs_dirs[2];
static bool frogfs_vfs_dirs_used[2];

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t offset;
} frogfs_vfs_file_t;

static frogfs_vfs_file_t frogfs_vfs_files[2];
static bool frogfs_vfs_files_used[2];

static const void* frogfs_active_base = NULL;
static uint32_t frogfs_active_size = 0;

static int frogfs_vfs_mount(uint32_t base_addr, uint32_t size) {
    frogfs_active_base = (const void*)base_addr;
    frogfs_active_size = size;
    return frogfs_mount(frogfs_active_base);
}

static int frogfs_vfs_statfs(uint32_t *total_bytes, uint32_t *free_bytes) {
    if (!frogfs_active_base) return -1;
    const frogfs_head_t *head = (const frogfs_head_t *)frogfs_active_base;
    *total_bytes = frogfs_active_size;
    *free_bytes = (frogfs_active_size > head->bin_sz) ? (frogfs_active_size - head->bin_sz) : 0;
    return 0;
}

static int frogfs_vfs_unmount(void) {
    frogfs_active_base = NULL;
    return 0;
}

static int frogfs_vfs_opendir(const char *path, void **dir_ctx) {
    if (!frogfs_active_base) return -1;
    
    int idx = -1;
    for (int i = 0; i < 2; i++) {
        if (!frogfs_vfs_dirs_used[i]) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;
    
    frogfs_vfs_dirs[idx].fs_base = frogfs_active_base;
    int res = frogfs_dir_open(&frogfs_vfs_dirs[idx], path);
    if (res < 0) return res;
    
    frogfs_vfs_dirs_used[idx] = true;
    *dir_ctx = &frogfs_vfs_dirs[idx];
    return 0;
}

static int frogfs_vfs_readdir(void *dir_ctx, vfs_dirent_t *ent) {
    frogfs_info_t info;
    int res = frogfs_dir_read((frogfs_dir_reader_t *)dir_ctx, &info);
    if (res <= 0) return res;
    
    strncpy(ent->name, info.name, sizeof(ent->name) - 1);
    ent->name[sizeof(ent->name) - 1] = '\0';
    ent->type = (info.type == FROGFS_TYPE_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    ent->size = info.size;
    return 1;
}

static int frogfs_vfs_closedir(void *dir_ctx) {
    frogfs_dir_reader_t *dir = (frogfs_dir_reader_t *)dir_ctx;
    for (int i = 0; i < 2; i++) {
        if (&frogfs_vfs_dirs[i] == dir) {
            frogfs_vfs_dirs_used[i] = false;
            break;
        }
    }
    return 0;
}

static int frogfs_vfs_open(const char *path, int mode, void **file_ctx) {
    if (!frogfs_active_base) return -1;
    if (mode & 2) return -1; // Write mode unsupported
    
    int idx = -1;
    for (int i = 0; i < 2; i++) {
        if (!frogfs_vfs_files_used[i]) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;
    
    uint32_t size = 0;
    const void *data = frogfs_get_file_data(frogfs_active_base, path, &size);
    if (!data) return -1;
    
    frogfs_vfs_files[idx].data = data;
    frogfs_vfs_files[idx].size = size;
    frogfs_vfs_files[idx].offset = 0;
    frogfs_vfs_files_used[idx] = true;
    *file_ctx = &frogfs_vfs_files[idx];
    return 0;
}

static int frogfs_vfs_close(void *file_ctx) {
    frogfs_vfs_file_t *file = (frogfs_vfs_file_t *)file_ctx;
    for (int i = 0; i < 2; i++) {
        if (&frogfs_vfs_files[i] == file) {
            frogfs_vfs_files_used[i] = false;
            break;
        }
    }
    return 0;
}

static int frogfs_vfs_read(void *file_ctx, void *buf, size_t len, size_t *read_len) {
    frogfs_vfs_file_t *file = (frogfs_vfs_file_t *)file_ctx;
    if (file->offset >= file->size) {
        *read_len = 0;
        return 0;
    }
    uint32_t avail = file->size - file->offset;
    if (len > avail) len = avail;
    memcpy(buf, file->data + file->offset, len);
    file->offset += len;
    *read_len = len;
    return 0;
}

static int frogfs_vfs_write(void *file_ctx, const void *buf, size_t len, size_t *written_len) {
    return -1;
}

static int frogfs_vfs_unlink(const char *path) {
    return -1;
}

static vfs_driver_t frogfs_vfs_driver = {
    .name = "FROGFS",
    .mount = frogfs_vfs_mount,
    .unmount = frogfs_vfs_unmount,
    .opendir = frogfs_vfs_opendir,
    .readdir = frogfs_vfs_readdir,
    .closedir = frogfs_vfs_closedir,
    .open = frogfs_vfs_open,
    .close = frogfs_vfs_close,
    .read = frogfs_vfs_read,
    .write = frogfs_vfs_write,
    .unlink = frogfs_vfs_unlink,
    .statfs = frogfs_vfs_statfs
};

void frogfs_vfs_init(void) {
    vfs_register_driver(&frogfs_vfs_driver);
}

