#ifndef FROGFS_READER_H
#define FROGFS_READER_H

#include <stdint.h>
#include <stdbool.h>

#define FROGFS_TYPE_DIR 1
#define FROGFS_TYPE_REG 2

typedef struct {
    uint8_t type;
    char name[64];
    uint32_t size;
} frogfs_info_t;

typedef struct {
    const void *fs_base;
    const void *root_dir;
    const void *current_dir;
    uint16_t child_idx;
    uint16_t child_count;
} frogfs_dir_reader_t;

int frogfs_mount(const void *base_addr);
int frogfs_dir_open(frogfs_dir_reader_t *dir, const char *path);
int frogfs_dir_read(frogfs_dir_reader_t *dir, frogfs_info_t *info);
int frogfs_dir_close(frogfs_dir_reader_t *dir);
const void* frogfs_get_file_data(const void *fs_base, const char *path, uint32_t *out_size);

#endif // FROGFS_READER_H
