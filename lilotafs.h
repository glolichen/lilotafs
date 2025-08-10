#ifndef LILOTAFS_H
#define LILOTAFS_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#if defined(LILOTAFS_LOCAL)
#include <sys/mman.h>
#elif !defined(LILOTA_IS_BOOTLOADER)
#include "esp_partition.h"
#endif

#define LILOTAFS_HEADER_ALIGN 8
#define LILOTAFS_DATA_ALIGN 8
#define LILOTAFS_DATA_ALIGN_KERNEL 0x10000
#define LILOTAFS_WEAR_LEVEL_MAX_RECORDS 5

#define LILOTAFS_KERNEL_EXT ".bin"

#define ALIGN_DOWN_FUNC(bits) \
static uint##bits##_t __attribute__((unused)) lilotafs_align_down_##bits(uint##bits##_t num, uint##bits##_t amount) { \
	return (num / amount) * amount; \
}
#define ALIGN_UP_FUNC(bits) \
static uint##bits##_t __attribute__((unused)) lilotafs_align_up_##bits(uint##bits##_t num, uint##bits##_t amount) { \
	return num % amount == 0 ? num : (lilotafs_align_down_##bits(num, amount) + amount); \
}
	
#define ALIGN_DOWN_PTR(bits) \
static void __attribute__((unused)) *lilotafs_align_down_ptr(void *ptr, uint##bits##_t amount) { \
	uint##bits##_t ptr_int = (uint##bits##_t) ptr; \
	return (void *) ((ptr_int / amount) * amount); \
}
#define ALIGN_UP_PTR(bits) \
static void __attribute__((unused)) *lilotafs_align_up_ptr(void *ptr, uint##bits##_t amount) { \
	uint##bits##_t ptr_int = (uint##bits##_t) ptr; \
	return ptr_int % amount == 0 ? ptr : (void *) ((uint##bits##_t) lilotafs_align_down_ptr(ptr, amount) + amount); \
}

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFF
	#define POINTER_SIZE uint64_t
	ALIGN_DOWN_PTR(64)
	ALIGN_UP_PTR(64)
#else
	#define POINTER_SIZE uint32_t
	ALIGN_DOWN_PTR(32)
	ALIGN_UP_PTR(32)
#endif

ALIGN_DOWN_FUNC(32)
ALIGN_UP_FUNC(32)
ALIGN_DOWN_FUNC(64)
ALIGN_UP_FUNC(64)

#define LILOTAFS_MAX_FILENAME_LEN 63
	
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
#if defined(LILOTAFS_LOCAL)
	uint32_t partition_size;
	uint8_t *flash_mmap;
#elif !defined(LILOTA_IS_BOOTLOADER)
	const esp_partition_t *partition;
	esp_partition_mmap_handle_t map_handle;
#endif

	uint32_t block_size;
	uint32_t fs_head, fs_tail;
	// worst case total size for a file
	// includes header, filename and data
	// assumes worse padding -- padding length == align requirement
	uint32_t largest_worst_file_size;
	bool has_wear_marker;
	int f_errno;
	int fd_list_size, fd_list_capacity;
	struct lilotafs_file_descriptor *fd_list;
};

struct lilotafs_rec_header {
	uint16_t magic;
	uint8_t status;
	uint8_t __reserved;
	uint32_t data_len;
} __attribute__((packed));;

struct lilotafs_file_descriptor {
	bool in_use;
	uint32_t position;
	uint32_t previous_position;
	uint32_t filename_len;
	off_t offset;
	int flags;
	int write_errno;
};

#define LILOTAFS_FUNC_STR_ENDS_WITH \
static bool str_ends_with(const char *str, const char *end, uint32_t len) { \
	uint32_t str_len = strnlen(str, len); \
	uint32_t end_len = strlen(end); \
	if (str_len == len || end_len == len || str_len < end_len) \
		return false; \
	for (uint32_t i = 0; i < end_len; i++) { \
		if (str[str_len - end_len + i] != end[i]) \
			return false; \
	} \
	return true; \
}

// calculate the smallest integer greater than min_value, but can be written
// into cur_value in flash without changing any 0 bits to 1
// check if the file_size can be written into data_len without changing 0 to 1
// num = file_size | data_len: num has every 1 bit from file_size and data_len
// num xor data_len: xor returns all bits that are different
// since every 1 in data_len is also 1 in num, if there is any difference, it is because
// that bit is 0 in data_len, but 1 in file_size, which we don't allow
// keep incrementing file_size until the difference/xor is 0
#define LILOTAFS_FUNC_GET_SMALLEST_COMPATIBLE \
uint32_t get_smallest_compatible(uint32_t min_value, uint32_t cur_value) { \
	min_value--; \
	while (((++min_value) | cur_value) ^ cur_value); \
	return min_value; \
}

uint32_t lilotafs_unmount(void *ctx);
#if defined(LILOTAFS_LOCAL)
int lilotafs_mount(void *ctx, uint32_t partition_size, int fd);
#elif !defined(LILOTA_IS_BOOTLOADER)
int lilotafs_mount(void *ctx, const esp_partition_t *partition);
#endif

int lilotafs_errno(void *ctx);

int lilotafs_open(void *ctx, const char *name, int flags, int mode);
int lilotafs_close(void *ctx, int fd);

ssize_t lilotafs_write(void *ctx, int fd, const void *buffer, unsigned int len);
ssize_t lilotafs_read(void *ctx, int fd, void *buffer, size_t len);
off_t lilotafs_lseek(void *ctx, int fd, off_t offset, int whence);
// int lilotafs_delete(void *ctx, int fd);

int lilotafs_mkdir(void *ctx, const char *name, mode_t mode);
DIR *lilotafs_opendir(void *ctx, const char *name);
struct dirent *lilotafs_readdir(void *ctx, DIR *pdir);
int lilotafs_closedir(void *ctx, DIR *pdir);

int lilotafs_stat(void *ctx, const char *path, struct stat *st);

#ifndef LILOTAFS_LOCAL
int lilotafs_ioctl(void *ctx, int fd, int cmd, va_list args);
#endif

uint32_t lilotafs_count_files(void *ctx);

uint32_t lilotafs_get_largest_file_size(void *ctx);
uint32_t lilotafs_get_head(void *ctx);
uint32_t lilotafs_get_tail(void *ctx);

#endif 

