#ifndef LILOTAFS_H
#define LILOTAFS_H

#include <stdint.h>
#include <stdbool.h>

#define FS_HEADER_ALIGN 8
#define FS_DATA_ALIGN 8
#define WEAR_LEVEL_MAX_RECORDS 5

#define ALIGN_DOWN_FUNC(bits) \
static inline uint##bits##_t align_down_##bits(uint##bits##_t num, uint##bits##_t amount) { \
	return (num / amount) * amount; \
}
#define ALIGN_UP_FUNC(bits) \
static inline uint##bits##_t align_up_##bits(uint##bits##_t num, uint##bits##_t amount) { \
	return num % amount == 0 ? num : (align_down_##bits(num, amount) + amount); \
}
ALIGN_DOWN_FUNC(32)
ALIGN_UP_FUNC(32)
ALIGN_DOWN_FUNC(64)
ALIGN_UP_FUNC(64)

#define FS_SUCCESS 0 // Operation successful
#define FS_ENOSPC (UINT32_MAX - 1) // Insufficient space (4294967294)
#define FS_EINVAL (UINT32_MAX - 2) // Invalid parameters (...93)
#define FS_EEXIST (UINT32_MAX - 3) // File already exists (create-only) (...92)
#define FS_ENOENT (UINT32_MAX - 4) // File not found (...91)
#define FS_ESPIPE (UINT32_MAX - 5) // Seek not supported on writable files (...90)
#define FS_EBADF (UINT32_MAX - 6) // Invalid file descriptor (...89)
#define FS_EMFILE (UINT32_MAX - 7) // Too many open files (...88)
#define FS_EPERM (UINT32_MAX - 8) // Operation not permitted (...87)
#define FS_EFLASH (UINT32_MAX - 9) // Flash write problem (...86)
#define FS_EUNKNOWN (UINT32_MAX - 10) // ??? (...85)

#define FS_MAX_FILENAME_LEN 63

#define FS_CREATE 1
#define FS_READABLE 2
#define FS_WRITABLE 4

enum fs_record_status {
	STATUS_ERASED = 0xFF, // 11111111
	STATUS_RESERVED = 0xFE, // 11111110
	STATUS_COMMITTED = 0xFC, // 11111100
	STATUS_MIGRATING = 0xF8, // 11111000
	STATUS_WRAP_MARKER = 0xF0, // 11110000
	STATUS_WEAR_MARKER = 0xE0, // 11100000
	STATUS_DELETED = 0x00, // 00000000
};

enum fs_magic {
	FS_RECORD = 0x5AA5,
	FS_WRAP_MARKER = 0x5AFA,
	FS_START = 0x5A00,
	FS_START_CLEAN = 0x5AA0
};

struct fs_context {
	uint8_t *flash_mmap;
	uint32_t fs_head, fs_tail;
	uint32_t largest_file_size, largest_filename_len;
	bool has_wear_marker;
	uint32_t vfs_open_error;
	uint32_t fd_list_size, fd_list_capacity;
	struct fs_file_descriptor *fd_list;
};

struct fs_rec_header {
	uint16_t magic;
	uint8_t status;
	uint32_t data_len;
};

struct fs_file_descriptor {
	bool in_use;
	uint32_t offset;
};

uint32_t lfs_set_file(struct fs_context *ctx, int fd);
uint32_t lfs_unmount(struct fs_context *ctx);
uint32_t lfs_mount(struct fs_context *ctx);

uint32_t vfs_open_errno(struct fs_context *ctx);
uint32_t vfs_open(struct fs_context *ctx, const char *name, int flags);
uint32_t vfs_close(struct fs_context *ctx, uint32_t fd);
uint32_t vfs_write(struct fs_context *ctx, uint32_t fd, void *buffer, uint32_t len);
uint32_t vfs_get_size(struct fs_context *ctx, uint32_t fd);
uint32_t vfs_read(struct fs_context *ctx, uint32_t fd, void *buffer, uint32_t addr, uint32_t len);
uint32_t vfs_delete(struct fs_context *ctx, uint32_t fd);

uint32_t lfs_count_files(struct fs_context *ctx);

uint32_t lfs_get_largest_file_size(struct fs_context *ctx);
uint32_t lfs_get_largest_filename_len(struct fs_context *ctx);
uint32_t lfs_get_head(struct fs_context *ctx);
uint32_t lfs_get_tail(struct fs_context *ctx);

#endif 

