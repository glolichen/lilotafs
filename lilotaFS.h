#ifndef LILOTAFS_H
#define LILOTAFS_H

#include <stdint.h>
#include <stdbool.h>

#define LILOTAFS_HEADER_ALIGN 8
#define LILOTAFS_DATA_ALIGN 8
#define LILOTAFS_WEAR_LEVEL_MAX_RECORDS 5

#define ALIGN_DOWN_FUNC(bits) \
static inline uint##bits##_t lilotafs_align_down_##bits(uint##bits##_t num, uint##bits##_t amount) { \
	return (num / amount) * amount; \
}
#define ALIGN_UP_FUNC(bits) \
static inline uint##bits##_t lilotafs_align_up_##bits(uint##bits##_t num, uint##bits##_t amount) { \
	return num % amount == 0 ? num : (lilotafs_align_down_##bits(num, amount) + amount); \
}
ALIGN_DOWN_FUNC(32)
ALIGN_UP_FUNC(32)
ALIGN_DOWN_FUNC(64)
ALIGN_UP_FUNC(64)

#define LILOTAFS_SUCCESS 0 // Operation successful
#define LILOTAFS_ENOSPC (UINT32_MAX - 1) // Insufficient space (4294967294)
#define LILOTAFS_EINVAL (UINT32_MAX - 2) // Invalid parameters (...93)
#define LILOTAFS_EEXIST (UINT32_MAX - 3) // File already exists (create-only) (...92)
#define LILOTAFS_ENOENT (UINT32_MAX - 4) // File not found (...91)
#define LILOTAFS_ESPIPE (UINT32_MAX - 5) // Seek not supported on writable files (...90)
#define LILOTAFS_EBADF (UINT32_MAX - 6) // Invalid file descriptor (...89)
#define LILOTAFS_EMFILE (UINT32_MAX - 7) // Too many open files (...88)
#define LILOTAFS_EPERM (UINT32_MAX - 8) // Operation not permitted (...87)
#define LILOTAFS_EFLASH (UINT32_MAX - 9) // Flash write problem (...86)
#define LILOTAFS_EUNKNOWN (UINT32_MAX - 10) // ??? (...85)
#define LILOTAFS_MAX_FILENAME_LEN 63
#define LILOTAFS_CREATE 1
#define LILOTAFS_READABLE 2
#define LILOTAFS_WRITABLE 4

enum lilotafs_record_status {
	LILOTAFS_STATUS_ERASED = 0xFF, // 11111111
	LILOTAFS_STATUS_RESERVED = 0xFE, // 11111110
	LILOTAFS_STATUS_COMMITTED = 0xFC, // 11111100
	LILOTAFS_STATUS_MIGRATING = 0xF8, // 11111000
	LILOTAFS_STATUS_WRAP_MARKER = 0xF0, // 11110000
	LILOTAFS_STATUS_WEAR_MARKER = 0xE0, // 11100000
	LILOTAFS_STATUS_DELETED = 0x00, // 00000000
};

enum lilotafs_magic {
	LILOTAFS_RECORD = 0x5AA5,
	LILOTAFS_WRAP_MARKER = 0x5AFA,
	LILOTAFS_START = 0x5A00,
	LILOTAFS_START_CLEAN = 0x5AA0
};

struct lilotafs_context {
	uint8_t *flash_mmap;
	uint32_t fs_head, fs_tail;
	uint32_t largest_file_size, largest_filename_len;
	bool has_wear_marker;
	uint32_t vfs_open_error;
	uint32_t fd_list_size, fd_list_capacity;
	struct lilotafs_file_descriptor *fd_list;
};

struct lilotafs_rec_header {
	uint16_t magic;
	uint8_t status;
	uint32_t data_len;
};

struct lilotafs_file_descriptor {
	bool in_use;
	uint32_t offset;
};

uint32_t lilotafs_test_set_file(struct lilotafs_context *ctx, int fd);
uint32_t lilotafs_unmount(struct lilotafs_context *ctx);
uint32_t lilotafs_mount(struct lilotafs_context *ctx);

uint32_t lilotafs_open_errno(struct lilotafs_context *ctx);

uint32_t lilotafs_open(struct lilotafs_context *ctx, const char *name, int flags);
uint32_t lilotafs_close(struct lilotafs_context *ctx, uint32_t fd);
uint32_t lilotafs_write(struct lilotafs_context *ctx, uint32_t fd, void *buffer, uint32_t len);
uint32_t lilotafs_get_size(struct lilotafs_context *ctx, uint32_t fd);
uint32_t lilotafs_read(struct lilotafs_context *ctx, uint32_t fd, void *buffer, uint32_t addr, uint32_t len);
uint32_t lilotafs_delete(struct lilotafs_context *ctx, uint32_t fd);

uint32_t lilotafs_count_files(struct lilotafs_context *ctx);

uint32_t lilotafs_get_largest_file_size(struct lilotafs_context *ctx);
uint32_t lilotafs_get_largest_filename_len(struct lilotafs_context *ctx);
uint32_t lilotafs_get_head(struct lilotafs_context *ctx);
uint32_t lilotafs_get_tail(struct lilotafs_context *ctx);

#endif 

