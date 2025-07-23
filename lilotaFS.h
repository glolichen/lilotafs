#ifndef LILOTAFS_H
#define LILOTAFS_H

#include <stdint.h>
#include <stdbool.h>

#ifndef LILOTAFS_LOCAL
#include "esp_partition.h"
#endif

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
#define LILOTAFS_ENOSPC (-1) // Insufficient space (4294967294)
#define LILOTAFS_EINVAL (-2) // Invalid parameters (...93)
#define LILOTAFS_EEXIST (-3) // File already exists (create-only) (...92)
#define LILOTAFS_ENOENT (-4) // File not found (...91)
#define LILOTAFS_ESPIPE (-5) // Seek not supported on writable files (...90)
#define LILOTAFS_EBADF (-6) // Invalid file descriptor (...89)
#define LILOTAFS_EMFILE (-7) // Too many open files (...88)
#define LILOTAFS_EPERM (-8) // Operation not permitted (...87)
#define LILOTAFS_EFLASH (-9) // Flash write problem (...86)
#define LILOTAFS_EUNKNOWN (-10) // ??? (...85)

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
#ifdef LILOTAFS_LOCAL
	uint32_t partition_size;
#else
	const esp_partition_t *partition;
	esp_partition_mmap_handle_t map_handle;
#endif

	uint32_t flash_sector_size;
	uint8_t *flash_mmap;
	uint32_t fs_head, fs_tail;
	uint32_t largest_file_size, largest_filename_len;
	bool has_wear_marker;
	int vfs_open_error;
	int fd_list_size, fd_list_capacity;
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

uint32_t lilotafs_unmount(void *ctx);
#ifdef LILOTAFS_LOCAL
int lilotafs_mount(void *ctx, uint32_t partition_size, int fd);
#else
int lilotafs_mount(void *ctx, const esp_partition_t *partition);
#endif

int lilotafs_open_errno(void *ctx);

int lilotafs_open(void *ctx, const char *name, int flags, int mode);
int lilotafs_close(void *ctx, int fd);
int lilotafs_write(void *ctx, int fd, void *buffer, uint32_t len);
int lilotafs_get_size(void *ctx, int fd);
int lilotafs_read(void *ctx, int fd, void *buffer, uint32_t len);
int lilotafs_delete(void *ctx, int fd);

uint32_t lilotafs_count_files(void *ctx);

uint32_t lilotafs_get_largest_file_size(void *ctx);
uint32_t lilotafs_get_largest_filename_len(void *ctx);
uint32_t lilotafs_get_head(void *ctx);
uint32_t lilotafs_get_tail(void *ctx);

#endif 

