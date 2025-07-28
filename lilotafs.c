#include "lilotafs.h"
#include <dirent.h>
#include <stdint.h>
#include <fcntl.h>

#ifdef LILOTAFS_LOCAL
#include <sys/mman.h>
#include <sys/types.h>
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "flash.h"
#include "util.h"

// mmap to instruction memory requires reads to be 4 byte aligned
// this takes advantage of the fact that rec_headers are 8 byte aligned
// and the data_len is forced to be on a 4 byte boundary

// magic is stored on the lowest 2 bytes
// in little endian, that would be the lowest 16 bits of the header if read as integer
#define REC_HEADER_MAGIC(header) (*((volatile uint32_t *) (header)) & 0xFFFF)
#define REC_HEADER_STATUS(header) ((*((volatile uint32_t *) header) >> 16) & 0xFF)
#define REC_HEADER_DATA_LEN(header) (*((volatile uint32_t *) header + 1))

uint32_t u32_min(uint32_t a, uint32_t b) {
	return a < b ? a : b;
}
uint32_t u32_max(uint32_t a, uint32_t b) {
	return a > b ? a : b;
}

bool lilotafs_is_ptr_mmaped(struct lilotafs_context *ctx, const void *str) {
	POINTER_SIZE mmap_start = (POINTER_SIZE) ctx->flash_mmap;
	POINTER_SIZE mmap_end = mmap_start + lilotafs_flash_get_partition_size(ctx);
	POINTER_SIZE str_addr = (POINTER_SIZE) str;
	return str_addr >= mmap_start && str_addr < mmap_end;
}

uint32_t aligned_strnlen(struct lilotafs_context *ctx, const char *str, uint32_t maxlen) {
	// if string not in mmap'ed instruction region, can use normal strnlen
	if (!lilotafs_is_ptr_mmaped(ctx, str))
		return strnlen(str, maxlen);

	uint32_t len = 0, cur_group = 0;
	for (uint32_t i = 0; i < maxlen; i++) {
		if (i % 4 == 0)
			cur_group = *((uint32_t *) lilotafs_align_down_ptr((uint8_t *) str + i, 4));
		uint32_t group_index = i % 4;
		uint8_t cur = (cur_group >> (group_index * 8)) & 0xFF;

		if (cur == 0)
			break;
		len++;
	}
	return len;
}

// behavior different from strcmp: only checks for equality, does not compare lexicographically
// return 0 for equal, anything other than 0 means not equal (and no other meaning)
uint32_t aligned_strneq(struct lilotafs_context *ctx, const char *str1, const char *str2, uint32_t maxlen) {
	// both not mmap'ed, can use normal unaligned strncmp
	if (!lilotafs_is_ptr_mmaped(ctx, str1) && !lilotafs_is_ptr_mmaped(ctx, str2))
		return strncmp(str1, str2, maxlen);

	uint32_t str1_len = aligned_strnlen(ctx, str1, maxlen);
	uint32_t str2_len = aligned_strnlen(ctx, str2, maxlen);

	if (str1_len != str2_len)
		return -1;

	if (str1_len == 64)
		return -1;

	// both mmap'ed, can read more bytes than required
	if (lilotafs_is_ptr_mmaped(ctx, str1) && lilotafs_is_ptr_mmaped(ctx, str2)) {
		uint32_t cur_group1 = 0, cur_group2 = 0;
		for (uint32_t i = 0; i < maxlen; i++) {
			if (i % 4 == 0) {
				cur_group1 = *((uint32_t *) lilotafs_align_down_ptr((uint8_t *) str1 + i, 4));
				cur_group2 = *((uint32_t *) lilotafs_align_down_ptr((uint8_t *) str2 + i, 4));
			}
			if (cur_group1 != cur_group2)
				return -1;
		}
		return 0;
	}

	// one mmap'ed, one not, this case is annoying
	const char *mmaped, *not_mmaped;
	if (lilotafs_is_ptr_mmaped(ctx, str1) && !lilotafs_is_ptr_mmaped(ctx, str2))
		mmaped = str1, not_mmaped = str2;
	else
		mmaped = str2, not_mmaped = str1;

	uint32_t cur_group = 0;
	for (uint32_t i = 0; i < str1_len; i++) {
		if (i % 4 == 0)
			cur_group = *((uint32_t *) lilotafs_align_down_ptr((uint8_t *) mmaped + i, 4));
		uint32_t group_index = i % 4;
		uint8_t cur_mmaped = (cur_group >> (group_index * 8)) & 0xFF;

		if (cur_mmaped != not_mmaped[i])
			return -1;
	}
	return 0;
}

// dest is not mmap'ed (cannot write there)
void *lilotafs_aligned_memcpy(struct lilotafs_context *ctx, void *restrict dest, const void *restrict src, uint32_t n) {
	if (!lilotafs_is_ptr_mmaped(ctx, src))
		return memcpy(dest, src, n);

	uint32_t cur_group = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (i % 4 == 0)
			cur_group = *((uint32_t *) lilotafs_align_down_ptr((uint8_t *) src + i, 4));
		uint32_t group_index = i % 4;
		uint8_t cur = (cur_group >> (group_index * 8)) & 0xFF;
		((uint8_t *) dest)[i] = cur;
	}
	return dest;
}

uint8_t lilotafs_mmap_read_byte(const void *ptr) {
	POINTER_SIZE byte_addr = (POINTER_SIZE) ptr;
	uint32_t *aligned_ptr = (uint32_t *) lilotafs_align_down_ptr((void *) ptr, 4);
	uint32_t val = *aligned_ptr;
	POINTER_SIZE aligned_addr = (POINTER_SIZE) aligned_ptr;
	return (val >> (8 * (byte_addr - aligned_addr))) & 0xFF;
}

struct lilotafs_rec_header *scan_for_header(void *ctx, uint32_t start, uint32_t partition_size) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	start = lilotafs_align_up_32(start, LILOTAFS_HEADER_ALIGN);
	for (uint32_t i = start; i <= partition_size - sizeof(struct lilotafs_rec_header); i += LILOTAFS_HEADER_ALIGN) {
		struct lilotafs_rec_header *header = (struct lilotafs_rec_header *) (context->flash_mmap + i);
		if (REC_HEADER_MAGIC(header) == LILOTAFS_START || REC_HEADER_MAGIC(header) == LILOTAFS_START_CLEAN)
			return header;
	}
	return NULL;
}

int change_file_magic(void *ctx, struct lilotafs_rec_header *file_header, uint16_t magic) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	uint32_t file_offset = (POINTER_SIZE) file_header - (POINTER_SIZE) context->flash_mmap;
	uint32_t magic_addr = file_offset + offsetof(struct lilotafs_rec_header, magic);

	if (lilotafs_flash_write(context, &magic, magic_addr, 2))
		return LILOTAFS_EFLASH;

	return LILOTAFS_SUCCESS;
}
int change_file_status(void *ctx, struct lilotafs_rec_header *file_header, uint8_t status) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t file_offset = (POINTER_SIZE) file_header - (POINTER_SIZE) context->flash_mmap;
	uint32_t status_addr = file_offset + offsetof(struct lilotafs_rec_header, status);

	if (lilotafs_flash_write(context, &status, status_addr, 1))
		return LILOTAFS_EFLASH;

	return LILOTAFS_SUCCESS;
}
int change_file_data_len(void *ctx, struct lilotafs_rec_header *file_header, uint32_t data_len) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t file_offset = (POINTER_SIZE) file_header - (POINTER_SIZE) context->flash_mmap;
	uint32_t data_len_addr = file_offset + offsetof(struct lilotafs_rec_header, data_len);

	if (lilotafs_flash_write(context, &data_len, data_len_addr, 4))
		return LILOTAFS_EFLASH;

	return LILOTAFS_SUCCESS;
}

bool magic_is_wrap_marker(struct lilotafs_rec_header *file) {
	// the magic number for the wrap marker is 0x5AFA
	uint16_t magic = REC_HEADER_MAGIC(file);
	if (magic == LILOTAFS_WRAP_MARKER)
		return true;
	// check first byte of magic number
	if ((magic & 0xFF) == 0xFA)
		return true;
	return false;
}


struct lilotafs_rec_header *process_advance_header(void *ctx, struct lilotafs_rec_header *cur_header,
											 uint32_t partition_size) {

	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	// if wrap header, back to start of partition
	// the wrap marker magic is FA FA, it's possible we crash between writing these bytes
	// resulting in FA FF, so we'll read the first byte
	// since the two bytes of FA FA are equal, this will work on little and big endian
	if (magic_is_wrap_marker(cur_header) || REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_WRAP_MARKER) {
		// if we crash while writing the data_len field of the wrap marker
		if (REC_HEADER_DATA_LEN(cur_header) != 0) {
			if (change_file_magic(context, cur_header, LILOTAFS_WRAP_MARKER))
				return NULL;
			if (change_file_status(context, cur_header, LILOTAFS_STATUS_WRAP_MARKER))
				return NULL;
			if (change_file_data_len(context, cur_header, 0))
				return NULL;
		}
		return (struct lilotafs_rec_header *) context->flash_mmap;
	}

	uint32_t current_offset = (POINTER_SIZE) cur_header - (POINTER_SIZE) context->flash_mmap;

	if (current_offset + sizeof(struct lilotafs_rec_header) >= partition_size)
		return NULL;

	// file name immediately after header
	char *filename = (char *) cur_header + sizeof(struct lilotafs_rec_header);
	uint32_t filename_len_padded = aligned_strnlen(ctx, filename, 64);

	// unterminated filename
	if (filename_len_padded == 64)
		return NULL;

	// add 1 for null terminator
	filename_len_padded++;

	// Malformed record - treat as end of valid data
	if (REC_HEADER_DATA_LEN(cur_header) > partition_size - current_offset
			- sizeof(struct lilotafs_rec_header) - filename_len_padded) {

		return NULL;
	}

	uint32_t next_offset = current_offset + sizeof(struct lilotafs_rec_header) + filename_len_padded;
	next_offset = lilotafs_align_up_32(next_offset, LILOTAFS_DATA_ALIGN);
	next_offset += REC_HEADER_DATA_LEN(cur_header);
	next_offset = lilotafs_align_up_32(next_offset, LILOTAFS_HEADER_ALIGN);

	if (next_offset >= partition_size)
		return NULL;

	if (REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_COMMITTED) {
		// TODO: save to hash table
		if (REC_HEADER_DATA_LEN(cur_header) > context->largest_file_size) {
			context->largest_file_size = REC_HEADER_DATA_LEN(cur_header);
			context->largest_filename_len =  filename_len_padded - 1;
		}
	}

	return (struct lilotafs_rec_header *) (context->flash_mmap + next_offset);
}

// returns information on the FIRST FREE SPACE immediately following the last valid record
// reserved files are not valid records, so this function will return reserved as the last_header
// also returns a pointer to the wear marker, wrap marker, migrating and reserved files
// as well as the number of files for use in wear leveling
// this function will also find the largest file size between the start and the tail
// and update that in ctx->largest_filename_len
struct scan_headers_result {
	struct lilotafs_rec_header *last_header, *wear_marker, *wrap_marker, *migrating, *reserved;
	uint32_t num_files;
} scan_headers(void *ctx, struct lilotafs_rec_header *start) {

	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	uint32_t partition_size = lilotafs_flash_get_partition_size(ctx);

	struct lilotafs_rec_header *cur_header = start;
	struct scan_headers_result ret = {
		.last_header = cur_header,
		.wear_marker = NULL,
		.wrap_marker = NULL,
		.migrating = NULL,
		.reserved = NULL,
		.num_files = 0
	};

	if (REC_HEADER_MAGIC(cur_header) != LILOTAFS_RECORD && REC_HEADER_MAGIC(cur_header) != LILOTAFS_START &&
			REC_HEADER_MAGIC(cur_header) != LILOTAFS_START_CLEAN && REC_HEADER_MAGIC(cur_header) != LILOTAFS_WRAP_MARKER) {

		return ret;
	}

	while (1) {
		// if mount creates a dummy header, do not count it as a file (it is empty)
		char *filename = (char *) cur_header + sizeof(struct lilotafs_rec_header);

		if (REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_COMMITTED && (REC_HEADER_MAGIC(cur_header) != LILOTAFS_START || aligned_strnlen(ctx, filename, 64) != 0))
			ret.num_files++;

		if (REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_WEAR_MARKER)
			ret.wear_marker = cur_header;
		if (magic_is_wrap_marker(cur_header) || REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_WRAP_MARKER)
			ret.wrap_marker = cur_header;
		if (REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_MIGRATING)
			ret.migrating = cur_header;

		// weird edge case: if the cur_header has status FF
		// becuse of a crash after writing magic but before writing reserved status
		// treat it like reserved
		if (REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_RESERVED || REC_HEADER_STATUS(cur_header) == 0xFF) {
			ret.reserved = cur_header;
			break;
		}

		struct lilotafs_rec_header *next_header = process_advance_header(context, cur_header, partition_size);
		if (!next_header)
			break;

		cur_header = next_header;
		if (REC_HEADER_MAGIC(cur_header) != LILOTAFS_RECORD && REC_HEADER_MAGIC(cur_header) != LILOTAFS_START &&
				REC_HEADER_MAGIC(cur_header) != LILOTAFS_START_CLEAN && REC_HEADER_MAGIC(cur_header) != LILOTAFS_WRAP_MARKER) {

			break;
		}
	}
	ret.last_header = cur_header;
	return ret;
}

uint32_t calculate_total_file_size(uint32_t filename_len, uint32_t data_len) {
	uint32_t new_file_total = sizeof(struct lilotafs_rec_header) + filename_len + 1;
	new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_DATA_ALIGN);
	new_file_total += data_len;
	new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_HEADER_ALIGN);
	return new_file_total;
}

// the total_size_size includes the header, filename and datalen
int check_free_space(void *ctx, uint32_t current_offset, uint32_t write_offset, uint32_t total_file_size) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t partition_size = lilotafs_flash_get_partition_size(ctx);

	// free space guarantee
	// check if: if we were to add this file (directly or with wear marker)
	// can we move the current largest file to the tail?
	// we need to be able to write the current largest file to the tail TWICE
	//
	// reasoning: if this is file system:
	// |=AAAAABBBBBBBBBB|B---------------|
	// after wear leveling becomes
	// |======BBBBBBBBBB|BAAAAA----------|
	//
	// the free space before and after the files can become fragmented
	// so we need to make sure BOTH of the areas, should they form that way,
	// can store the largest file
	//
	// in case we lose power / crash while writing the file, and the new content
	// is incompatible to the old content (i.e. new -> old requires changing 0 -> 1)
	// it is possible to end up with a situation where it is impossible to write
	// the old file back, since that would run out of space
	//
	// in total, we need to check there is space for
	// the old file (if we are modifying), the new file, and the largest file twice
	
	uint32_t old_file_total;
	if (current_offset == UINT32_MAX)
		old_file_total = 0;
	else {
		struct lilotafs_rec_header *old_file = (struct lilotafs_rec_header *) (context->flash_mmap + current_offset);

		// the old and new file names should really be the same, but checking to be safe
		uint32_t old_filename_len = aligned_strnlen(ctx, (char *) old_file + sizeof(struct lilotafs_rec_header), 64);

		old_file_total = sizeof(struct lilotafs_rec_header) + old_filename_len + 1;
		old_file_total = lilotafs_align_up_32(old_file_total, LILOTAFS_DATA_ALIGN);
		old_file_total += REC_HEADER_DATA_LEN(old_file);
		old_file_total = lilotafs_align_up_32(old_file_total, LILOTAFS_HEADER_ALIGN);
	}

	uint32_t wrap_marker_total = sizeof(struct lilotafs_rec_header);
	wrap_marker_total = lilotafs_align_up_32(wrap_marker_total, LILOTAFS_HEADER_ALIGN);

	uint32_t wear_marker_total = sizeof(struct lilotafs_rec_header) + 1;
	wrap_marker_total = lilotafs_align_up_32(wrap_marker_total, LILOTAFS_HEADER_ALIGN);

	uint32_t largest_file_total = sizeof(struct lilotafs_rec_header) + context->largest_filename_len + 1;
	largest_file_total = lilotafs_align_up_32(largest_file_total, LILOTAFS_DATA_ALIGN);
	largest_file_total += context->largest_file_size;
	largest_file_total = lilotafs_align_up_32(largest_file_total, LILOTAFS_HEADER_ALIGN);
	// the current file we're writing might be larger than current largest
	largest_file_total = u32_max(largest_file_total, u32_max(total_file_size, old_file_total));

	// hypothetical tail pointer if we added this file
	// log_wrap indicates whether there is already a wear marker,
	// or if one is needed in case we add the current file, and copy
	// the largest file twiec
	bool log_wrap = context->fs_head >= context->fs_tail;
	uint32_t current_position = write_offset;
	if (total_file_size + wrap_marker_total > partition_size - current_position) {
		if (log_wrap)
			return LILOTAFS_ENOSPC;
		log_wrap = true;
		current_position = 0;
	}
	current_position += total_file_size;
	current_position = lilotafs_align_up_32(current_position, LILOTAFS_HEADER_ALIGN);

	// check if we can write the old file
	if (old_file_total + wrap_marker_total > partition_size - current_position) {
		if (log_wrap)
			return LILOTAFS_ENOSPC;
		log_wrap = true;
		current_position = 0;
	}
	current_position += old_file_total;
	current_position = lilotafs_align_up_32(current_position, LILOTAFS_HEADER_ALIGN);

	// also check if we can add a wear marker...? could be helpful
	if (wear_marker_total + wrap_marker_total > partition_size - current_position) {
		if (log_wrap)
			return LILOTAFS_ENOSPC;
		log_wrap = true;
		current_position = 0;
	}
	current_position += wear_marker_total;
	current_position = lilotafs_align_up_32(current_position, LILOTAFS_HEADER_ALIGN);

	// now we try to add the largest file, twice
	for (uint32_t i = 0; i < 2; i++) {
		if (largest_file_total + wrap_marker_total > partition_size - current_position) {
			if (log_wrap)
				return LILOTAFS_ENOSPC;
			log_wrap = true;
			current_position = 0;
		}
		current_position += largest_file_total;
		current_position = lilotafs_align_up_32(current_position, LILOTAFS_HEADER_ALIGN);

		// we cannot end such that the tail is in the same sector as the head,
		uint32_t sector_size = context->block_size;
		if (log_wrap && current_position / sector_size == context->fs_head / sector_size)
			return LILOTAFS_ENOSPC;

		// if we wrapped the log / inserted a wear marker for any of the two files,
		// tail must be in front of head
		if (log_wrap && current_position >= context->fs_head)
			return LILOTAFS_ENOSPC;
	}

	// if no wrap, there is no problem since we still have free space
	// after tail and before the end of partition

	return LILOTAFS_SUCCESS;
}

// current_offset is updated with the new offset, provide pointer to UINT32_MAX if only creating
// updates fs_tail
int append_file(void *ctx, const char *filename, uint32_t *current_offset, const void *buffer,
				uint32_t len, uint8_t want_status, bool add_wear_marker, bool is_wear_level) {

	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t filename_len = aligned_strnlen(ctx, filename, 64);
	uint32_t partition_size = lilotafs_flash_get_partition_size(context);

	uint32_t new_file_total = calculate_total_file_size(filename_len, len);
	if (!is_wear_level && check_free_space(ctx, *current_offset, context->fs_tail, new_file_total))
		return LILOTAFS_ENOSPC;

	uint32_t wrap_marker_total = sizeof(struct lilotafs_rec_header);
	wrap_marker_total = lilotafs_align_up_32(wrap_marker_total, LILOTAFS_HEADER_ALIGN);

	if (is_wear_level) {
		bool log_wrap = context->fs_head >= context->fs_tail;
		uint32_t current_position = context->fs_tail;
		if (new_file_total + wrap_marker_total > partition_size - current_position) {
			if (log_wrap)
				return LILOTAFS_ENOSPC;
			log_wrap = true;
			current_position = 0;
		}
		current_position += new_file_total;
		current_position = lilotafs_align_up_32(current_position, LILOTAFS_HEADER_ALIGN);

		// check if we can write the old file
		// if (old_file_total + wrap_marker_total > partition_size - current_position) {
		// 	if (log_wrap)
		// 		return LILOTAFS_ENOSPC;
		// 	log_wrap = true;
		// 	current_position = 0;
		// }
		// current_position += old_file_total;
		// current_position = lilotafs_align_up_32(current_position, LILOTAFS_HEADER_ALIGN);

		uint32_t sector_size = context->block_size;
		if (log_wrap && current_position / sector_size == context->fs_head / sector_size)
			return LILOTAFS_ENOSPC;

		if (log_wrap && current_position >= context->fs_head)
			return LILOTAFS_ENOSPC;
	}

	// uint32_t new_file_total = sizeof(struct lilotafs_rec_header) + filename_len + 1;
	// new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_DATA_ALIGN);
	// new_file_total += len;
	// new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_HEADER_ALIGN);

	// mark old file as migrating
	if (*current_offset != UINT32_MAX) {
		struct lilotafs_rec_header *old_header = (struct lilotafs_rec_header *) (context->flash_mmap + *current_offset);
		if (change_file_status(context, old_header, LILOTAFS_STATUS_MIGRATING))
			return LILOTAFS_EFLASH;
	}

	uint32_t new_file_offset = context->fs_tail;
	// ensure we have space for a wrap marker afterwards
	if (new_file_total + wrap_marker_total > partition_size - context->fs_tail) {
		struct lilotafs_rec_header wrap_marker;
		memset(&wrap_marker, 0, sizeof(wrap_marker));
		wrap_marker.magic = LILOTAFS_WRAP_MARKER;
		wrap_marker.status = LILOTAFS_STATUS_WRAP_MARKER;
		wrap_marker.data_len = 0;

		if (lilotafs_flash_write(context, &wrap_marker, context->fs_tail, sizeof(struct lilotafs_rec_header)))
			return LILOTAFS_EFLASH;
		new_file_offset = 0;
	}

	// phase 1: reserve
	struct lilotafs_rec_header file_header;
	memset(&file_header, 0, sizeof(file_header));
	file_header.magic = LILOTAFS_RECORD;
	file_header.status = LILOTAFS_STATUS_RESERVED;
	file_header.data_len = 0xFFFFFFFF;

	if (lilotafs_flash_write(context, &file_header, new_file_offset, sizeof(struct lilotafs_rec_header)))
		return LILOTAFS_EFLASH;

	// phase 2: write data
	uint32_t data_offset = new_file_offset + sizeof(struct lilotafs_rec_header) + filename_len + 1;
	data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);
	if (lilotafs_flash_write(context, filename, new_file_offset + sizeof(struct lilotafs_rec_header), filename_len + 1))
		return LILOTAFS_EFLASH;
	if (len) {
		if (lilotafs_flash_write(context, buffer, data_offset, len))
			return LILOTAFS_EFLASH;
	}

	// phase 3: set size and commit
	struct lilotafs_rec_header *new_file_header = (struct lilotafs_rec_header *) (context->flash_mmap + new_file_offset);
	if (change_file_data_len(ctx, new_file_header, len))
		return LILOTAFS_EFLASH;
	if (change_file_status(ctx, new_file_header, want_status))
		return LILOTAFS_EFLASH;

	// phase 4: delete old
	if (*current_offset != UINT32_MAX) {
		struct lilotafs_rec_header *old_header = (struct lilotafs_rec_header *) (context->flash_mmap + *current_offset);
		if (change_file_status(context, old_header, LILOTAFS_STATUS_DELETED))
			return LILOTAFS_EFLASH;
	}
	
	context->fs_tail = lilotafs_align_up_32(data_offset + len, LILOTAFS_HEADER_ALIGN);

	if (!context->has_wear_marker && *current_offset != UINT32_MAX && add_wear_marker) {
		char empty = 0;
		uint32_t current_offset = UINT32_MAX;
		context->has_wear_marker = true;
		int code = append_file(context, &empty, &current_offset, NULL, 0, LILOTAFS_STATUS_WEAR_MARKER, false, false);
		if (code != LILOTAFS_SUCCESS)
			return code;
	}

	*current_offset = new_file_offset;

	return LILOTAFS_SUCCESS;
}

// move the current file to migrating, and add a blank file
int reserve_file(void *ctx, const char *filename, uint32_t *current_offset) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t filename_len = aligned_strnlen(ctx, filename, 64);
	uint32_t partition_size = lilotafs_flash_get_partition_size(context);

	uint32_t total_file_size = sizeof(struct lilotafs_rec_header) + filename_len + 1;
	if (check_free_space(ctx, *current_offset, context->fs_tail, total_file_size))
		return LILOTAFS_ENOSPC;

	uint32_t new_file_total = sizeof(struct lilotafs_rec_header) + filename_len + 1;
	new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_DATA_ALIGN);
	new_file_total += 0;
	new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_HEADER_ALIGN);

	uint32_t wrap_marker_total = sizeof(struct lilotafs_rec_header);
	wrap_marker_total = lilotafs_align_up_32(wrap_marker_total, LILOTAFS_HEADER_ALIGN);

	// mark old file as migrating
	if (*current_offset != UINT32_MAX) {
		struct lilotafs_rec_header *old_header = (struct lilotafs_rec_header *) (context->flash_mmap + *current_offset);
		change_file_status(context, old_header, LILOTAFS_STATUS_MIGRATING);
	}

	uint32_t new_file_offset = context->fs_tail;
	// ensure we have space for a wrap marker afterwards
	if (new_file_total + wrap_marker_total > partition_size - context->fs_tail) {
		struct lilotafs_rec_header wrap_marker;
		memset(&wrap_marker, 0, sizeof(wrap_marker));
		wrap_marker.magic = LILOTAFS_WRAP_MARKER;
		wrap_marker.status = LILOTAFS_STATUS_WRAP_MARKER;
		wrap_marker.data_len = 0;

		if (lilotafs_flash_write(context, &wrap_marker, context->fs_tail, sizeof(struct lilotafs_rec_header)))
			return LILOTAFS_EFLASH;
		new_file_offset = 0;
	}

	struct lilotafs_rec_header file_header;
	memset(&file_header, 0, sizeof(file_header));
	file_header.magic = LILOTAFS_RECORD;
	file_header.status = LILOTAFS_STATUS_RESERVED;
	file_header.data_len = 0xFFFFFFFF;

	if (lilotafs_flash_write(context, &file_header, new_file_offset, sizeof(struct lilotafs_rec_header)))
		return LILOTAFS_EFLASH;

	uint32_t data_offset = new_file_offset + sizeof(struct lilotafs_rec_header) + filename_len + 1;
	data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);
	if (lilotafs_flash_write(context, filename, new_file_offset + sizeof(struct lilotafs_rec_header), filename_len + 1))
		return LILOTAFS_EFLASH;

	context->fs_tail = lilotafs_align_up_32(data_offset, LILOTAFS_HEADER_ALIGN);
	*current_offset = new_file_offset;

	return LILOTAFS_SUCCESS;
}

int remove_false_magic(void *ctx, uint8_t *start, uint32_t size) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t start_offset = (POINTER_SIZE) start - (POINTER_SIZE) context->flash_mmap;
	uint32_t end_offset = start_offset + size;
	start_offset = lilotafs_align_up_32(start_offset, LILOTAFS_HEADER_ALIGN);

	for (uint32_t i = start_offset; i < end_offset; i += LILOTAFS_HEADER_ALIGN) {
		uint16_t magic = REC_HEADER_MAGIC(context->flash_mmap + i);
		// if data is any magic number, set it to 0 to avoid picking it up by mistake
		if (magic == LILOTAFS_RECORD || magic == LILOTAFS_START || magic == LILOTAFS_START_CLEAN) {
			uint16_t zero = 0;
			if (lilotafs_flash_write(context, &zero, i, 2))
				return LILOTAFS_EFLASH;
		}
	}

	return LILOTAFS_SUCCESS;
}

int clobber_file_data(void *ctx, struct lilotafs_rec_header *file) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	if (magic_is_wrap_marker(file) || REC_HEADER_STATUS(file) == LILOTAFS_STATUS_WRAP_MARKER)
		return LILOTAFS_SUCCESS;

	char *filename = (char *) file + sizeof(struct lilotafs_rec_header);
	uint32_t filename_offset = (POINTER_SIZE) filename - (POINTER_SIZE) context->flash_mmap;

	uint32_t data_offset = filename_offset + aligned_strnlen(ctx, filename, 64) + 1;
	data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);
	data_offset += REC_HEADER_DATA_LEN(file);

	return remove_false_magic(context, (uint8_t *) filename, data_offset - filename_offset);
}

int erase_file(void *ctx, uint32_t cur_offset, uint32_t next_offset) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t sector_size = context->block_size;

	// even though we can now erase the sectors the front of the file occupies
	// we still need to be careful about what happens in case of a crash
	// we need to perform the magic number removal above, in case we suffer
	// a power loss while erasing

	// |-----RFF|FFFFFFFF|FF-----| (| = erase boundary, F = file, R = record)
	// all memory before R has already been clobbered, so we can the first sector
	// second sector is only used for file, so it can be erased
	// we CANNOT erase the third sector, as it contains another file
	// do not need to clobber parts of the sector that file occupies
	// that is already dealt with by removing its ;magic number

	// clobber part of block the new file is in, that is before the file
	if (next_offset % sector_size != 0) {
		uint32_t sector_start = lilotafs_align_down_32(next_offset, sector_size);
		if (remove_false_magic(context, context->flash_mmap + sector_start, next_offset % sector_size))
			return LILOTAFS_EFLASH;
	}

	// previous offset cannot be in the same sector as tail
	if (context->fs_tail / sector_size == cur_offset / sector_size)
		return LILOTAFS_ENOSPC;

	// erase old blocks, in reverse order (large address -> small address)
	for (uint32_t sector = next_offset / sector_size; sector >= cur_offset / sector_size + 1; sector--) {
		if (lilotafs_flash_erase_region(context, (sector - 1) * sector_size, sector_size))
			return LILOTAFS_EFLASH;
	}

	return LILOTAFS_SUCCESS;
}

// this function has caused sooo many problems
int wear_level_compact(void *ctx, struct lilotafs_rec_header *wear_marker, uint32_t num_files) {
	PRINTF("wear leveling start...\n");

	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t partition_size = lilotafs_flash_get_partition_size(context);
	uint32_t sector_size = context->block_size;

	// delete wear marker
	if (change_file_status(context, wear_marker, LILOTAFS_STATUS_DELETED))
		return LILOTAFS_EFLASH;

	// this is the number of files we will copy to the tail
	// we set a limit defined by the macro, but also do not move more than the number of files
	// this is to avoid moving the same more than onec
	uint32_t count = u32_min(LILOTAFS_WEAR_LEVEL_MAX_RECORDS, num_files);

	struct lilotafs_rec_header *cur_header = (struct lilotafs_rec_header *) (context->flash_mmap + context->fs_head);

	// this should be impossible
	// if (REC_HEADER_MAGIC(cur_header) != LILOTAFS_RECORD && REC_HEADER_MAGIC(cur_header) != LILOTAFS_START && REC_HEADER_MAGIC(cur_header) != LILOTAFS_START_CLEAN) {
	// 	PRINTF("??????????\n");
	// 	return LILOTAFS_EUNKNOWN;
	// }

	while (1) {
		struct lilotafs_rec_header *next_header = process_advance_header(context, cur_header, partition_size);

		if (!next_header)
			break;

		if (REC_HEADER_MAGIC(next_header) != LILOTAFS_RECORD && REC_HEADER_MAGIC(next_header) != LILOTAFS_START &&
				REC_HEADER_MAGIC(next_header) != LILOTAFS_START_CLEAN && REC_HEADER_MAGIC(next_header) != LILOTAFS_WRAP_MARKER) {

			break;
		}

		uint32_t cur_offset = (POINTER_SIZE) cur_header - (POINTER_SIZE) context->flash_mmap;
		uint32_t next_offset = (POINTER_SIZE) next_header - (POINTER_SIZE) context->flash_mmap;

		// move the current file to the tail
		uint32_t cur_offset_after_move = cur_offset;

		// NOTE: if we mount the filesystem and do not find a LILOTAFS_START record
		// we create a record at offset 0 with magic LILOTAFS_START and length 0
		// we do NOT want to move that record

		// if file is committed, append it to the tail
		if (!(REC_HEADER_DATA_LEN(cur_header) == 0 && cur_offset == 0) && REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_COMMITTED) {
			count--;

			char *filename = (char *) cur_header + sizeof(struct lilotafs_rec_header);

			uint32_t data_offset = cur_offset + sizeof(struct lilotafs_rec_header) + aligned_strnlen(ctx, filename, 64) + 1;
			data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);
			uint8_t *data_start = (uint8_t *) context->flash_mmap + data_offset;

			int code = append_file(
				context, filename, &cur_offset_after_move, data_start,
				REC_HEADER_DATA_LEN(cur_header), LILOTAFS_STATUS_COMMITTED, false, true
			);
			if (code != LILOTAFS_SUCCESS)
				return code;
		}

		// we need to be deliberate about the order to make modifications in case of a crash
		// first, set the current file to LILOTAFS_START_CLEAN, indicating this file is the old LILOTAFS_START
		if (change_file_magic(context, next_header, LILOTAFS_START_CLEAN))
			return LILOTAFS_EFLASH;

		// we cannot have any magic numbers in 32 byte boundaries, or they will be detected as files
		// we want to clobber the filename too, so data start is the file name
		int code = clobber_file_data(context, cur_header);
		if (code != LILOTAFS_SUCCESS)
			return code;

		// if the file crosses a flash boundary, or we are wrapping
		if (next_offset / sector_size != cur_offset / sector_size) {
			// if next_offset is 0, then we are processing a wrap marker
			// the wrap marker does not actually cross the flash erase boundar
			// we can simply erase all of the last sector
			code = erase_file(context, cur_offset, next_offset == 0 ? lilotafs_flash_get_partition_size(context) : next_offset);
			if (code != LILOTAFS_SUCCESS)
				return code;
		}

		// NOTE: if we crash during wear leveling, there are two failure cases:
		// If here or earlier, then there is an LILOTAFS_START_CLEAN (next)
		// and maybe an LILOTAFS_START, depending on whether the current file's header is erased

		// done with processing the current file
		// now advance LILOTAFS_START and delete the just-moved file
		if (change_file_magic(context, next_header, LILOTAFS_START))
			return LILOTAFS_EFLASH;

		// NOTE: if we crash here, there is one or two LILOTAFS_START's
		// next is definitely LILOTAFS_START, and current may have been erased

		// do not set magic to 0 if the sector has been erased
		if (next_offset / sector_size == cur_offset / sector_size) {
			if (change_file_magic(context, cur_header, 0))
				return LILOTAFS_EFLASH;
		}

		context->fs_head = next_offset;
		cur_header = next_header;

		if (count == 0)
			break;
	}

	return LILOTAFS_SUCCESS;
}

struct lilotafs_rec_header *find_file_name(void *ctx, const char *name, bool accept_migrating) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t filename_len = aligned_strnlen(ctx, name, 64);
	uint32_t partition_size = lilotafs_flash_get_partition_size(context);

	struct lilotafs_rec_header *cur_header = (struct lilotafs_rec_header *) (context->flash_mmap + context->fs_head);
	while (1) {
		if (REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_COMMITTED ||
				(accept_migrating && REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_MIGRATING)) {

			char *cur_filename = (char *) cur_header + sizeof(struct lilotafs_rec_header);
			// PRINTF("found file %s\n", cur_filename);
			if (aligned_strnlen(ctx, cur_filename, 64) == filename_len &&
					aligned_strneq(ctx, cur_filename, name, 64) == 0) {

				return cur_header;
			}
		}

		struct lilotafs_rec_header *next_header = process_advance_header(context, cur_header, partition_size);
		if (!next_header)
			break;
		cur_header = next_header;
		if (REC_HEADER_MAGIC(cur_header) != LILOTAFS_RECORD && REC_HEADER_MAGIC(cur_header) != LILOTAFS_WRAP_MARKER)
			break;
	}
	return NULL;
}

// simulate graceful computer shutdown
uint32_t lilotafs_unmount(void *ctx) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	context->largest_file_size = 0, context->largest_filename_len = 0;
	context->fs_head = 0, context->fs_tail = 0;
	context->has_wear_marker = false;

	// close all open FDs
	if (context->fd_list_capacity != 0 && context->fd_list != NULL)
		free(context->fd_list);
	context->fd_list_size = 0, context->fd_list_capacity = 0;

#ifdef LILOTAFS_LOCAL
	munmap(context->flash_mmap, lilotafs_flash_get_partition_size(context));
#else
	esp_partition_munmap(context->map_handle);
#endif

	return LILOTAFS_SUCCESS;
}

// calculate the smallest integer greater than min_value, but can be written
// into cur_value in flash without changing any 0 bits to 1
uint32_t get_smallest_compatible(uint32_t min_value, uint32_t cur_value) {
	// check if the file_size can be written into data_len without changing 0 to 1
	// num = file_size | data_len: num has every 1 bit from file_size and data_len
	// num xor data_len: xor returns all bits that are different
	// since every 1 in data_len is also 1 in num, if there is any difference, it is because
	// that bit is 0 in data_len, but 1 in file_size, which we don't allow
	// keep incrementing file_size until the difference/xor is 0
	min_value--;
	while (((++min_value) | cur_value) ^ cur_value);
	return min_value;
}

#ifdef LILOTAFS_LOCAL
int lilotafs_mount(void *ctx, uint32_t partition_size, int fd) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	context->flash_mmap = mmap(NULL, partition_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	context->partition_size = partition_size;
	context->block_size = SECTOR_SIZE;
	if (context->flash_mmap == MAP_FAILED)
		return -1;

#else
int lilotafs_mount(void *ctx, const esp_partition_t *partition) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	
	context->partition = partition;
	context->block_size = partition->erase_size;
	
	const void **flash_mmap_addr = (const void **) &context->flash_mmap;
	esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_INST, flash_mmap_addr, &context->map_handle);
#endif

	context->largest_file_size = 0, context->largest_filename_len = 0;
	context->fs_head = 0, context->fs_tail = 0;
	context->has_wear_marker = false;
	context->f_errno = 0;
	
	struct lilotafs_rec_header *cur_header = (struct lilotafs_rec_header *) context->flash_mmap;
	struct lilotafs_rec_header *wear_marker = NULL;
	uint32_t num_files = 0;
	
	// if position 0 is an normal file, not the head, or there is nothing there
	// if it is LILOTAFS_START_CLEAN, we must check if there is an LILOTAFS_START
	// if the last thing processed before the crash is a wrap marker
	// we may crash before or after the 00 5A (LILOTAFS_START) of the wrap marker is erased
	// so we need to see whether we can find an LILOTAFS_START that is the wrap marker
	// and if we can, deal with that
	// see wear_level_compact, and observe that before we start erasing the sector
	// containing the wrap marker (LILOTAFS_START), we clobber the file info and remove
	// false magics of all files before the wrap marker
	// so we can advance to the tail and search for headers from there
	if (REC_HEADER_MAGIC(cur_header) != LILOTAFS_START) {
		PRINTF("offset 0 is not FS_START\n");

		// follow current file until no more
		// (if no file at 0 this will return cur_header immediately, then offset = 0)
		struct scan_headers_result scan_result = scan_headers(ctx, cur_header);
		cur_header = scan_result.last_header;
		if (scan_result.wear_marker)
			wear_marker = scan_result.wear_marker;

		// scan_headers will return either the space directly after the last file,
		// or the header of the last file in case the last file is a reserved file
		// this is because we do not know when we crashed previously when writing it
		// so we cannot accurately determine when the reserved file ends
		//
		// (a) if we crash while or before writing the filename, then the filename is not terminated
		// so we know no actual data has been written, since the max file length is 64,
		// we have an upper bound on the space taken up by this file
		//
		// (b) if we crash afterward, this is really annoying to deal with
		// we have no upper bound on file size, since data_len is written on close
		// unfortunately, we have to traverse the whole (most of) the file, and there is
		// nothing to guarantee that any headers we do find are actual headers, and not data
		//
		// we start searching for the magic number FS_START (5a00) in the flash sector
		// immediately after the data_start of this file, due to the requirement that the
		// head and tail cannot be in the same sector
		//
		//  1. calculate the new data_len of this file, which is whatever value places
		//     the end of the current file immediately before the next flash erase boundary
		//     but do not set it, in case we crash before finishing
		//  2. scan for a magic number 5a00 on an alignment boundary
		//  3. if we find a "header", we perform these checks to verify it:
		//      1. the status byte is valid
		//      2. the file name is terminated with a zero (if the file is not a wrap marker)
		//      3. "pretend" that file is FS_START, and use the scan_headers function to verify
		//         if the file returned by scan_header is the reserved file, it is correct
		//         if not, continue, until we find a file is
		//  4. as we go along, if a sector does not contain FS_START, erase it
		//  5. set the data_len to what was calculated in step 1
		//  6. if we do not find the header, that is either because
		//
		// (we deal with this crash again later on)

		uint32_t offset = (POINTER_SIZE) cur_header - (POINTER_SIZE) context->flash_mmap;
		if (cur_header == scan_result.reserved && (REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_RESERVED || REC_HEADER_STATUS(cur_header) == 0xFF)) {
			char *reserved_filename = (char *) scan_result.reserved + sizeof(struct lilotafs_rec_header);
			uint32_t reserved_filename_len = aligned_strnlen(ctx, reserved_filename, 64);

			// (a) crash while or before writing the file name
			if (reserved_filename_len == 64) {
				offset += sizeof(struct lilotafs_rec_header) + 64;
				offset = lilotafs_align_up_32(offset, LILOTAFS_HEADER_ALIGN);
				cur_header = scan_for_header(context, offset, lilotafs_flash_get_partition_size(ctx));
			}
			// (b) crash while writing file data
			else {
				uint32_t block_size = context->block_size;

				uint32_t data_start = offset + sizeof(struct lilotafs_rec_header) + reserved_filename_len + 1;
				data_start = lilotafs_align_up_32(data_start, LILOTAFS_DATA_ALIGN);

				uint32_t starting_block = lilotafs_align_up_32(data_start, block_size);
				uint32_t data_len = starting_block - data_start;

				// possibly crashed while writing data_len, need to make sure is compatible
				data_len = get_smallest_compatible(data_len, REC_HEADER_DATA_LEN(scan_result.reserved));

				cur_header = NULL;
				uint32_t last_addr = lilotafs_flash_get_partition_size(ctx) - sizeof(struct lilotafs_rec_header);
				for (uint32_t addr = starting_block; addr <= last_addr; addr += LILOTAFS_HEADER_ALIGN) {
					// check if we have moved on to the next block
					// and the previous block is not the block we started on
					// if both true, then erase the previous block
					uint32_t prev_block = lilotafs_align_down_32(addr - LILOTAFS_HEADER_ALIGN, block_size);
					uint32_t cur_block = lilotafs_align_down_32(addr, block_size);
					if (prev_block != cur_block && cur_block != starting_block) {
						if (lilotafs_flash_erase_region(context, prev_block, block_size)) {
							context->f_errno = LILOTAFS_EFLASH;
							return LILOTAFS_EFLASH;
						}
					}

					struct lilotafs_rec_header *header = (struct lilotafs_rec_header *) (context->flash_mmap + addr);
					if (REC_HEADER_MAGIC(header) != LILOTAFS_START && REC_HEADER_MAGIC(header) != LILOTAFS_START_CLEAN)
						continue;

					if (REC_HEADER_STATUS(header) != LILOTAFS_STATUS_RESERVED && 
						REC_HEADER_STATUS(header) != LILOTAFS_STATUS_COMMITTED &&
						REC_HEADER_STATUS(header) != LILOTAFS_STATUS_MIGRATING &&
						REC_HEADER_STATUS(header) != LILOTAFS_STATUS_WRAP_MARKER &&
						REC_HEADER_STATUS(header) != LILOTAFS_STATUS_WEAR_MARKER &&
						REC_HEADER_STATUS(header) != LILOTAFS_STATUS_DELETED) {
						continue;
					}

					uint32_t cur_filename_len = 0;
					if (!(magic_is_wrap_marker(header) || REC_HEADER_STATUS(header) == LILOTAFS_STATUS_WRAP_MARKER)) {
						char *cur_filename = (char *) header + sizeof(struct lilotafs_rec_header);
						cur_filename_len = aligned_strnlen(ctx, cur_filename, 64);
					}

					// equal 64 = no null terminator
					if (cur_filename_len == 64)
						continue;

					struct scan_headers_result loop_scan_result = scan_headers(ctx, header);
					if (loop_scan_result.last_header == scan_result.last_header) {
						cur_header = header;
						break;
					}
				}

				if (change_file_data_len(ctx, scan_result.reserved, data_len)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}
				if (change_file_status(ctx, scan_result.reserved, LILOTAFS_STATUS_DELETED)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}
			}
		}
		else
			cur_header = scan_for_header(context, offset, lilotafs_flash_get_partition_size(ctx));

		// if cur_header is NULL: no LILOTAFS_START
		// if offset 0 is LILOTAFS_START_CLEAN, then this is the result of the crash described earlier
		// if not, we need to write a LILOTAFS_START
		// (merely setting cur_header to 0 is enough, as this case is handled later)
		if (!cur_header && REC_HEADER_MAGIC((struct lilotafs_rec_header *) context->flash_mmap) == LILOTAFS_START_CLEAN)
			cur_header = (struct lilotafs_rec_header *) context->flash_mmap;
	}

	// if none found, write a LILOTAFS_START header at 0
	if (!cur_header) {
		PRINTF("no FS_START\n");

		struct lilotafs_rec_header new_header;
		memset(&new_header, 0, sizeof(new_header));
		new_header.magic = LILOTAFS_START;
		new_header.status = LILOTAFS_STATUS_COMMITTED;
		new_header.data_len = 0;


		char empty = 0;
		// write record and empty filename
		if (lilotafs_flash_write(context, &new_header, 0, sizeof(struct lilotafs_rec_header))) {
			context->f_errno = LILOTAFS_EFLASH;
			return LILOTAFS_EFLASH;
		}
		if (lilotafs_flash_write(context, &empty, sizeof(struct lilotafs_rec_header), 1)) {
			context->f_errno = LILOTAFS_EFLASH;
			return LILOTAFS_EFLASH;
		}

		context->fs_head = 0;
		context->fs_tail = sizeof(struct lilotafs_rec_header) + 1;
		context->fs_tail = lilotafs_align_up_32(context->fs_tail, LILOTAFS_HEADER_ALIGN);

		PRINTF("head = 0x%lx\n", (POINTER_SIZE) context->fs_head);
		PRINTF("tail = 0x%lx\n", (POINTER_SIZE) context->fs_tail);

		context->f_errno = LILOTAFS_SUCCESS;
		return LILOTAFS_SUCCESS;
	}

	// first, check for crash

	// there are multiple cases of crash during wear leveling
	// if the file processed during the crash is across multiple erase sectors,
	// and erase_file erased the header of that file:
	//   1. first record is LILOTAFS_START_CLEAN, no LILOTAFS_START
	// else (if the file is in only one erase sector, or crash before the record is erased)
	//   2. first record is LILOTAFS_START, second record is LILOTAFS_START
	//      in this case, the first file has already been cleaned up
	//      we can simply set the first record magic to 00, and done
	//   3. first record is LILOTAFS_START, second record is LILOTAFS_START_CLEAN
	//      do not know that we are done cleaning up the first file
	//      we clean up the first file, set the second record to LILOTAFS_START
	//     a nd set the first record to 00

	// 1. if the first is LILOTAFS_START_CLEAN
	if (REC_HEADER_MAGIC(cur_header) == LILOTAFS_START_CLEAN) {
		if (change_file_magic(context, cur_header, LILOTAFS_START)) {
			context->f_errno = LILOTAFS_EFLASH;
			return LILOTAFS_EFLASH;
		}
	}
	else {
		PRINTF("first is not start clean\n");

		struct lilotafs_rec_header *next_header = process_advance_header(context, cur_header, lilotafs_flash_get_partition_size(ctx));
		if (next_header) {
			// 2. crash after the file has been cleaned up, simply delete the old file
			if (REC_HEADER_MAGIC(next_header) == LILOTAFS_START) {
				PRINTF("next afterwards is FS_START\n");
				if (change_file_magic(context, cur_header, 0)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}
				cur_header = next_header;
			}
			// 3. complicated
			if (REC_HEADER_MAGIC(next_header) == LILOTAFS_START_CLEAN) {
				PRINTF("next afterwards is FS_START_CLEAN\n");

				uint32_t cur_offset = (POINTER_SIZE) cur_header - (POINTER_SIZE) context->flash_mmap;
				uint32_t next_offset = (POINTER_SIZE) next_header - (POINTER_SIZE) context->flash_mmap;

				// we cannot have any magic numbers in 32 byte boundaries, or they will be detected as files
				int code = clobber_file_data(context, cur_header);
				if (code != LILOTAFS_SUCCESS) {
					context->f_errno = code;
					return code;
				}

				// if the file crosses a flash boundary, or we are wrapping
				uint32_t sector_size = context->block_size;
				if (next_offset / sector_size != cur_offset / sector_size) {
					// if next_offset is 0, then we are processing a wrap marker
					// the wrap marker does not actually cross the flash erase boundar
					// we can simply erase all of the last sector
					code = erase_file(context, cur_offset, next_offset == 0 ? lilotafs_flash_get_partition_size(context) : next_offset);
					if (code != LILOTAFS_SUCCESS) {
						context->f_errno = code;
						return code;
					}
				}

				// done with processing the current file
				// now advance LILOTAFS_START and delete the just-moved file
				if (change_file_magic(context, next_header, LILOTAFS_START)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}

				// do not set magic to 0 if the sector has been erased
				if (next_offset / sector_size == cur_offset / sector_size) {
					if (change_file_magic(context, cur_header, 0)) {
						context->f_errno = LILOTAFS_EFLASH;
						return LILOTAFS_EFLASH;
					}
				}

				cur_header = next_header;
			}
		}
	}
	
	context->fs_head = (POINTER_SIZE) cur_header - (POINTER_SIZE) context->flash_mmap;
	
	// scan until no more files -- set that to tail pointer
	struct scan_headers_result scan_result = scan_headers(ctx, cur_header);
	cur_header = scan_result.last_header;
	num_files += scan_result.num_files;
	if (scan_result.wear_marker)
		wear_marker = scan_result.wear_marker;

	// this is not actually the tail! scan_headers ends on a reserved file
	// because we do not know how much of that file was successfully written
	context->fs_tail = (POINTER_SIZE) cur_header - (POINTER_SIZE) context->flash_mmap;

	// FIXME: we redid how files are written
	// need to fix crash recovery

	if (scan_result.reserved) {
		PRINTF("crash recovery: reserved\n");

		// (a) crash while writing data, or crash while writing data_len
		//
		//  1. locate the flash sector immediately above data start of the reserved file
		//  2. erase sectors until we reach the end of flash, or we are at the head
		//  3. within the same sector as the data start, find the last byte that is not FF
		//  4. claim that byte as the end, and calculate the file size
		//  5. if data_len is not FFFFFFFF (crash while writing data_len),
		//     increment file size until we can write it into data_len
		//  6. write data_len and delete reserved file reserved file
		//  7. (if there is migrating) copy the migrating file to the end
		//
		// (b) crash while writing the file name -- file name is not terminated
		//
		//  (i)  if there is migrating:
		//   1. fill in the file name from the migrating file
		//   2. copy the content of the migrating file directly
		//   3. set the data len
		//   4. commit reserved, delete migrating
		//  (ii) if there is not migrating:
		//   1. locate the 64 byte space where file name should be
		//   2. counting from the back, find the first character that is not FF
		//   3. set the firstr byte to 0 (empty file name)
		//   4. set data_len such that next file is after the "junk file name"
		//   5. delete that file

		struct lilotafs_rec_header *migrating = scan_result.migrating;
		struct lilotafs_rec_header *reserved = scan_result.reserved;

		uint32_t mig_offset = (POINTER_SIZE) migrating - (POINTER_SIZE) context->flash_mmap;
		uint32_t res_offset = (POINTER_SIZE) reserved - (POINTER_SIZE) context->flash_mmap;

		// check if the file name is completely written
		char *res_filename = (char *) reserved + sizeof(struct lilotafs_rec_header);
		uint32_t res_filename_len = aligned_strnlen(ctx, res_filename, 64);

		// (a) crash while writing file data
		if (res_filename_len != 64) {
			// erase from sector immediately after data start

			// 1. erase every sector higher than the reserved file, until either fs_head or end of partition
			// 2. calculate the last non-FF byte, and calculate the file size
			// 3. write file size into data_len, or the lowest possible value if data_len is not FFFFFFFF
			// 4. set new fs_tail into context struct

			uint32_t res_offset = (POINTER_SIZE) reserved - (POINTER_SIZE) context->flash_mmap;

			uint32_t data_start = res_offset + sizeof(struct lilotafs_rec_header);
			data_start += res_filename_len + 1;
			data_start = lilotafs_align_up_32(data_start, LILOTAFS_DATA_ALIGN);

			uint32_t first_sector = lilotafs_align_up_32(data_start, context->block_size);

			uint32_t last_sector;
			if (context->fs_head > res_offset) {
				last_sector = context->fs_head;
				last_sector = lilotafs_align_down_32(last_sector, context->block_size);
			}
			else
				last_sector = lilotafs_flash_get_partition_size(ctx);

			for (uint32_t sector = first_sector; sector < last_sector; sector += context->block_size) {
				if (lilotafs_flash_erase_region(context, sector, context->block_size)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}
			}

			// find the first non-FF byte
			uint32_t file_end = first_sector;
			while (context->flash_mmap[--file_end] == 0xFF);

			// calculate the actual size of the file
			// if file_end <= data_start, the file is all 0xFF and we are reading into padding
			uint32_t file_data_len = file_end < data_start ? 0 : file_end - data_start + 1;

			// if there's already something in data_len, increment until compatible
			if (REC_HEADER_DATA_LEN(reserved) != 0xFFFFFFFF)
				file_data_len = get_smallest_compatible(file_data_len, REC_HEADER_DATA_LEN(reserved));

			if (change_file_data_len(ctx, reserved, file_data_len)) {
				context->f_errno = LILOTAFS_EFLASH;
				return LILOTAFS_EFLASH;
			}
			if (change_file_status(context, reserved, LILOTAFS_STATUS_DELETED)) {
				context->f_errno = LILOTAFS_EFLASH;
				return LILOTAFS_EFLASH;
			}

			// context->fs_tail = lilotafs_align_up_32(data_start + file_data_len, LILOTAFS_DATA_ALIGN);
			context->fs_tail = lilotafs_align_up_32(data_start + file_data_len, LILOTAFS_DATA_ALIGN);
			scan_result.reserved = NULL;

			// if there is migrating: proceed to (scan_result.migrating && !scan_result.reserved)
			// which copies migrating to a new file at the tail if no committed file of the same name is found
		}
		// (b) crash while writing file name
		else {
			// (i) copy the contents of migrating directly to reserved file
			if (migrating) {
				char *mig_filename = (char *) migrating + sizeof(struct lilotafs_rec_header);

				// finish writing file name
				uint32_t write_offset = res_offset + sizeof(struct lilotafs_rec_header);
				if (lilotafs_flash_write(context, mig_filename, write_offset, aligned_strnlen(ctx, mig_filename, 64) + 1)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}

				// copy data from migrating to reserved
				write_offset += aligned_strnlen(ctx, mig_filename, 64) + 1;
				write_offset = lilotafs_align_up_32(write_offset, LILOTAFS_DATA_ALIGN);

				uint32_t mig_data_offset = mig_offset + sizeof(struct lilotafs_rec_header);
				mig_data_offset += aligned_strnlen(ctx, mig_filename, 64) + 1;
				mig_data_offset = lilotafs_align_up_32(mig_data_offset, LILOTAFS_DATA_ALIGN);
				uint8_t *mig_data = (uint8_t *) context->flash_mmap + mig_data_offset;

				if (lilotafs_flash_write(context, mig_data, write_offset, REC_HEADER_DATA_LEN(migrating))) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}

				// write data_len, commit and delete migrating
				if (change_file_data_len(context, reserved, REC_HEADER_DATA_LEN(migrating))) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}
				if (change_file_status(context, reserved, LILOTAFS_STATUS_COMMITTED)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}
				if (change_file_status(context, migrating, LILOTAFS_STATUS_DELETED)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}

				// write_offset is this file's start of data
				context->fs_tail = lilotafs_align_up_32(write_offset + REC_HEADER_DATA_LEN(migrating), LILOTAFS_DATA_ALIGN);
				scan_result.migrating = NULL;
				scan_result.reserved = NULL;
			}
			// (ii) clear filename, set data size, delete
			else {
				// keep decrementing last_non_ff until we find a byte that is not FF
				// or last_non_ff is 0, then we stop
				uint32_t last_non_ff = 64;
				while (((uint8_t *) res_filename)[--last_non_ff] == 0xFF && last_non_ff == 0);

				uint32_t res_filename_offset = res_offset + sizeof(struct lilotafs_rec_header);

				char zero = 0;
				if (lilotafs_flash_write(ctx, &zero, res_filename_offset, 1)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}

				// officially, the file name length is 0, because of null terminator
				uint32_t data_start = res_filename_offset + 1;
				data_start = lilotafs_align_up_32(data_start, LILOTAFS_DATA_ALIGN);

				uint32_t data_len = res_filename_offset + last_non_ff - data_start + 1;

				if (change_file_data_len(context, reserved, data_len)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}
				if (change_file_status(context, reserved, LILOTAFS_STATUS_DELETED)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}

				context->fs_tail = lilotafs_align_up_32(data_start + data_len, LILOTAFS_DATA_ALIGN);
				scan_result.migrating = NULL;
				scan_result.reserved = NULL;
			}
		}
	}

	// found migrating, no reserved: there may or may not be a commited file of the same name
	// possible there is a committed file of the same name since we commit file before delete migrating
	// (migrating and reserved case (a) also goes here to copy migrating file)
	if (scan_result.migrating && !scan_result.reserved) {
		PRINTF("crash recovery: migrating and not reserved\n");

		char *filename = (char *) scan_result.migrating + sizeof(struct lilotafs_rec_header);

		struct lilotafs_rec_header *found_file = find_file_name(context, filename, false);

		// if we find a committed file of the same name, only need to delete migrating file
		// append file crashed right before setting the migrating file to deleted, complete this

		if (!found_file) {
			// there is no committed file of the same name
			// append a file with the same contents

			uint32_t migrating_offset = (POINTER_SIZE) scan_result.migrating - (POINTER_SIZE) context->flash_mmap;
			uint32_t migrating_data_offset = migrating_offset + sizeof(struct lilotafs_rec_header);
			migrating_data_offset += aligned_strnlen(ctx, filename, 64) + 1;
			migrating_data_offset = lilotafs_align_up_32(migrating_data_offset, LILOTAFS_DATA_ALIGN);

			int code = append_file(
				context, filename, &migrating_offset, context->flash_mmap + migrating_data_offset,
				REC_HEADER_DATA_LEN(scan_result.migrating), LILOTAFS_STATUS_COMMITTED, false, false
			);

			if (code != LILOTAFS_SUCCESS) {
				context->f_errno = code;
				return code;
			}
		}

		if (change_file_status(context, scan_result.migrating, LILOTAFS_STATUS_DELETED)) {
			context->f_errno = LILOTAFS_EFLASH;
			return LILOTAFS_EFLASH;
		}
	}

	PRINTF("head = 0x%lx\n", (POINTER_SIZE) context->fs_head);
	PRINTF("tail = 0x%lx\n", (POINTER_SIZE) context->fs_tail);

	// if we find wear marker, need to perform wear leveling
	if (wear_marker) {
		PRINTF("wear marker\n");
		int code = wear_level_compact(context, wear_marker, num_files);
		if (code != LILOTAFS_SUCCESS) {
			context->f_errno = code;
			return code;
		}

		PRINTF("new head = 0x%lx\n", (POINTER_SIZE) context->fs_head);
		PRINTF("new tail = 0x%lx\n", (POINTER_SIZE) context->fs_tail);
	}

	PRINTF("mount complete\n");
	context->f_errno = LILOTAFS_SUCCESS;
	return LILOTAFS_SUCCESS;
}






int fd_list_append(void *ctx, struct lilotafs_file_descriptor fd) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	if (context->fd_list_size < context->fd_list_capacity) {
		context->fd_list[context->fd_list_size++] = fd;
		return 0;
	}

	if (context->fd_list_capacity == 0) {
		context->fd_list_capacity = 1;
		context->fd_list = (struct lilotafs_file_descriptor *) malloc(sizeof(struct lilotafs_file_descriptor));
	}
	else {
		context->fd_list_capacity *= 2;
		context->fd_list = (struct lilotafs_file_descriptor *) realloc(
			context->fd_list,
			context->fd_list_capacity * sizeof(struct lilotafs_file_descriptor)
		);
	}
	if (!context->fd_list)
		return 1;

	context->fd_list[context->fd_list_size++] = fd;
	return 0;
}

// returns index in list, or UINT32_MAX on failure
int fd_list_add(void *ctx, struct lilotafs_file_descriptor fd) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	for (int i = 0; i < context->fd_list_size; i++) {
		if (!context->fd_list[i].in_use) {
			context->fd_list[i] = fd;
			return i;
		}
	}
	if (fd_list_append(context, fd))
		return -1;
	return context->fd_list_size - 1;
}

bool check_fd(const struct lilotafs_context *context, int fd) {
	return fd >= context->fd_list_size && context->fd_list[fd].in_use;
}

int lilotafs_errno(void *ctx) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	return context->f_errno;
}

char *remove_prefix_slash(struct lilotafs_context *ctx, const char *filename) {
	uint32_t filename_len = aligned_strnlen(ctx, filename, 64);
	char *actual_name = (char *) filename;

	// continuously remove '/' prefix (lilota likes to ask for "//file")
	while (actual_name[0] == '/') {
		char *previous_name = actual_name;
		actual_name = (char *) malloc(filename_len * sizeof(char));

		for (uint32_t i = 0; i < filename_len - 1; i++)
			actual_name[i] = previous_name[i + 1];
		actual_name[filename_len - 1] = 0;

		if (previous_name != filename)
			free(previous_name);
	}

	return actual_name;
}

char *get_file_directory(struct lilotafs_context *ctx, const char *file) {
	int last_slash = -1;

	uint32_t cur_group = 0;
	for (uint32_t i = 0; i < aligned_strnlen(ctx, file, 64) - 1; i++) {
		if (i % 4 == 0)
			cur_group = *((uint32_t *) lilotafs_align_down_ptr((uint8_t *) file + i, 4));
		uint32_t group_index = i % 4;
		uint8_t cur = (cur_group >> (group_index * 8)) & 0xFF;
		if (cur == '/')
			last_slash = i;
	}

	if (last_slash == -1)
		return NULL;

	// plus one for / character, plus one for null terminator
	char *file_dir = (char *) malloc(last_slash + 2);
	lilotafs_aligned_memcpy(ctx, file_dir, file, last_slash + 1);
	file_dir[last_slash + 1] = 0;

	return file_dir;
}

int lilotafs_open(void *ctx, const char *name, int flags, int mode) {
	(void) mode;
	
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t filename_len_raw = aligned_strnlen(ctx, name, 64);
	if (filename_len_raw > LILOTAFS_MAX_FILENAME_LEN) {
		context->f_errno = LILOTAFS_EINVAL;
		return -1;
	}

	// cannot end with slash (end with slash are directories)
	if (name[filename_len_raw - 1] == '/') {
		context->f_errno = LILOTAFS_EINVAL;
		return -1;
	}

	if (flags != O_RDONLY && flags != O_WRONLY && flags != (O_WRONLY | O_CREAT)) {
		context->f_errno = LILOTAFS_EPERM;
		return -1;
	}

	// if there is a file open for write, cannot open another file for write
	for (int i = 0; i < context->fd_list_size; i++) {
		struct lilotafs_file_descriptor *descriptor = &context->fd_list[i];
		if (!descriptor->in_use)
			continue;
		if ((descriptor->flags & O_WRONLY) && (flags & O_WRONLY)) {
			context->f_errno = LILOTAFS_EPERM;
			return -1;
		}
	}
	
	char *actual_name = remove_prefix_slash(ctx, name);

	char *file_dir = get_file_directory(ctx, actual_name);
	if (file_dir != NULL) {
		struct lilotafs_rec_header *found_dir = find_file_name(ctx, file_dir, false);
		free(file_dir);
		if (found_dir == NULL) {
			if (actual_name != name)
				free(actual_name);
			context->f_errno = LILOTAFS_ENOENT;
			return -1;
		}
	}

	// if we open an existing file for write,
	// previous_position stores the location of the migrating file
	// so that we can set it to deleted on close
	struct lilotafs_file_descriptor fd = {
		.in_use = true,
		.position = UINT32_MAX,
		.previous_position = UINT32_MAX,
		.filename_len = aligned_strnlen(ctx, actual_name, 64),
		.offset = 0,
		.flags = flags,
		.write_errno = LILOTAFS_SUCCESS,
	};

	if (flags == O_RDONLY) {
		struct lilotafs_rec_header *file_found = find_file_name(context, actual_name, true);
		if (!file_found) {
			if (actual_name != name)
				free(actual_name);
			context->f_errno = LILOTAFS_ENOENT;
			return -1;
		}
		fd.position = (POINTER_SIZE) file_found - (POINTER_SIZE) context->flash_mmap;
	}
	else {
		struct lilotafs_rec_header *file_found = find_file_name(context, actual_name, false);
		if (file_found) {
			// if file found, save the previous file's permission
			// reserve_file will set the old file to migrating, and reserve a new file
			// also set previous_position, which will be deleted on close
			fd.position = (POINTER_SIZE) file_found - (POINTER_SIZE) context->flash_mmap;
			fd.previous_position = fd.position;
		}
		else if (!(flags & O_CREAT)) {
			if (actual_name != name)
				free(actual_name);
			context->f_errno = LILOTAFS_ENOENT;
			return -1;
		}

		// reserve a file at the end, keep status at reserved
		// commit file on close
		uint32_t code = reserve_file(context, actual_name, &fd.position);
		if (code != LILOTAFS_SUCCESS) {
			if (actual_name != name)
				free(actual_name);
			context->f_errno = code;
			return -1;
		}
	}

	int fd_index = fd_list_add(context, fd);
	if (fd_index == -1) {
		if (actual_name != name)
			free(actual_name);
		context->f_errno = LILOTAFS_EUNKNOWN;
		return -1;
	}

	// scan again to get the largest file
	scan_headers(ctx, (struct lilotafs_rec_header *) (context->flash_mmap + context->fs_head));

	context->f_errno = LILOTAFS_SUCCESS;
	return fd_index;
}

int lilotafs_close(void *ctx, int fd) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	if (check_fd(context, fd)) {
		context->f_errno = LILOTAFS_EBADF;
		return LILOTAFS_EBADF;
	}

	struct lilotafs_file_descriptor desc = context->fd_list[fd];

	context->fd_list[fd].in_use = false;
	context->fd_list[fd].position = 0;
	context->fd_list[fd].previous_position = 0;
	context->fd_list[fd].filename_len = 0;
	context->fd_list[fd].offset = 0;
	context->fd_list[fd].flags = 0;

	// reading file -> do nothing else
	if (!(desc.flags & O_WRONLY)) {
		context->f_errno = LILOTAFS_SUCCESS;
		return LILOTAFS_SUCCESS;
	}

	struct lilotafs_rec_header *file_header = (struct lilotafs_rec_header *) (context->flash_mmap + desc.position);

	// length of a file remains FFFFFFFF (-1) until closed
	if (change_file_data_len(ctx, file_header, desc.offset)) {
		context->f_errno = LILOTAFS_EFLASH;
		return LILOTAFS_EFLASH;
	}

	// writing a file, and an error occurred in lilotafs_write
	// the new file is hopeless, restore the old/migrating file
	// but only after setting data_len
	if (desc.write_errno != LILOTAFS_SUCCESS) {
		// delete this file
		// note that if we crash afterward immediately afterwards, we will be in the
		// "migrating but no reserved" case, which does exactly what we are doing now anyway
		if (change_file_status(ctx, file_header, LILOTAFS_STATUS_DELETED)) {
			context->f_errno = LILOTAFS_EFLASH;
			return LILOTAFS_EFLASH;
		}

		context->fs_tail = desc.position + sizeof(struct lilotafs_rec_header) + desc.filename_len + 1;
		context->fs_tail = lilotafs_align_up_32(context->fs_tail, LILOTAFS_DATA_ALIGN);
		context->fs_tail += desc.offset;
		context->fs_tail = lilotafs_align_up_32(context->fs_tail, LILOTAFS_HEADER_ALIGN);

		// add a wear marker here to ensure wear leveling on next boot
		if (!context->has_wear_marker) {
			char empty = 0;
			uint32_t current_offset = UINT32_MAX;
			context->has_wear_marker = true;
			int code = append_file(context, &empty, &current_offset, NULL, 0, LILOTAFS_STATUS_WEAR_MARKER, false, false);
			if (code != LILOTAFS_SUCCESS) {
				context->f_errno = code;
				return code;
			}
		}

		if (desc.previous_position == UINT32_MAX) {
			context->f_errno = LILOTAFS_SUCCESS;
			return LILOTAFS_SUCCESS;
		}

		uint32_t previous_offset = desc.previous_position;
		struct lilotafs_rec_header *previous_header = (struct lilotafs_rec_header *) (context->flash_mmap + previous_offset);

		uint32_t previous_filename_offset = previous_offset + sizeof(struct lilotafs_rec_header);
		char *filename = (char *) (context->flash_mmap + previous_filename_offset);

		uint32_t previous_data_offset = previous_filename_offset + aligned_strnlen(ctx, filename, 64) + 1;
		previous_data_offset = lilotafs_align_up_32(previous_data_offset, LILOTAFS_DATA_ALIGN);

		// hacky solution:
		// we pass in u32_max because that indicates we are "appending a new file"
		// this tells the free space guarantee to not check if the old file can be migrated
		// after writing the new file, because we do not care since we are moving the old file
		uint32_t u32_max = UINT32_MAX;
		int code = append_file(
			context, filename, &u32_max, context->flash_mmap + previous_data_offset,
			REC_HEADER_DATA_LEN(previous_header), LILOTAFS_STATUS_COMMITTED, false, false
		);

		if (code != LILOTAFS_SUCCESS) {
			context->f_errno = code;
			return code;
		}

		if (change_file_status(ctx, previous_header, LILOTAFS_STATUS_DELETED)) {
			context->f_errno = LILOTAFS_EFLASH;
			return LILOTAFS_EFLASH;
		}

		context->f_errno = LILOTAFS_SUCCESS;
		return LILOTAFS_SUCCESS;
	}

	if (change_file_status(ctx, file_header, LILOTAFS_STATUS_COMMITTED)) {
		context->f_errno = LILOTAFS_EFLASH;
		return LILOTAFS_EFLASH;
	}

	// delete the previous file
	if (desc.previous_position != UINT32_MAX) {
		struct lilotafs_rec_header *old_file = (struct lilotafs_rec_header *) (context->flash_mmap + desc.previous_position);
		if (change_file_status(context, old_file, LILOTAFS_STATUS_DELETED)) {
			context->f_errno = LILOTAFS_EFLASH;
			return LILOTAFS_EFLASH;
		}
	}

	context->fs_tail = desc.position + sizeof(struct lilotafs_rec_header) + desc.filename_len + 1;
	context->fs_tail = lilotafs_align_up_32(context->fs_tail, LILOTAFS_DATA_ALIGN);
	context->fs_tail += desc.offset;
	context->fs_tail = lilotafs_align_up_32(context->fs_tail, LILOTAFS_HEADER_ALIGN);

	// if the previous_position is not UINT32_MAX, then we are writing to an existing file
	// and we need to append a wear marker
	if (!context->has_wear_marker && desc.previous_position != UINT32_MAX) {
		char empty = 0;
		uint32_t current_offset = UINT32_MAX;
		context->has_wear_marker = true;
		int code = append_file(context, &empty, &current_offset, NULL, 0, LILOTAFS_STATUS_WEAR_MARKER, false, false);
		if (code != LILOTAFS_SUCCESS) {
			context->f_errno = code;
			return code;
		}
	}

	context->f_errno = LILOTAFS_SUCCESS;
	return LILOTAFS_SUCCESS;
}

ssize_t lilotafs_write(void *ctx, int fd, const void *buffer, unsigned int len) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	if (check_fd(context, fd)) {
		context->f_errno = LILOTAFS_EBADF;
		return -1;
	}

	struct lilotafs_file_descriptor *desc = &context->fd_list[fd];

	// if there's been a failure before, cannot proceed
	if (desc->write_errno != LILOTAFS_SUCCESS)
		return -1;

	if (!(desc->flags & O_WRONLY)) {
		context->f_errno = LILOTAFS_EPERM;
		desc->write_errno = context->f_errno;
		return -1;
	}

	struct lilotafs_rec_header *header = (struct lilotafs_rec_header *) (context->flash_mmap + desc->position);
	char *filename = (char *) header + sizeof(struct lilotafs_rec_header);

	// we need to check there is space for
	// the migrating file (if one exists), the current file, and the largest file twice
	// simulate the effects of writing the buffer/len to the file

	uint32_t file_data_len = desc->offset + len;
	uint32_t total_file_size = calculate_total_file_size(aligned_strnlen(ctx, filename, 64), file_data_len);

	int error_code = check_free_space(ctx, desc->position, desc->position, total_file_size);
	if (error_code != LILOTAFS_SUCCESS) {
		context->f_errno = error_code;
		desc->write_errno = context->f_errno;
		return -1;
	}
	
	uint32_t data_start = desc->position + sizeof(struct lilotafs_rec_header);
	data_start += aligned_strnlen(ctx, filename, 64) + 1;
	data_start = lilotafs_align_up_32(data_start, LILOTAFS_DATA_ALIGN);

	// need to add a wrap marker
	uint32_t data_end = data_start + desc->offset;
	if (data_end + len + sizeof(struct lilotafs_rec_header) > lilotafs_flash_get_partition_size(ctx)) {
		// there's a migrating file (F8), the file we're currently writing/reserved (FE)
	
		// 1. add a wrap marker, it will have header FA A5 F0
		uint32_t wrap_offset = lilotafs_align_up_32(data_end, LILOTAFS_HEADER_ALIGN);

		struct lilotafs_rec_header wrap_marker;
		memset(&wrap_marker, 0, sizeof(wrap_marker));
		wrap_marker.magic = LILOTAFS_WRAP_MARKER;
		wrap_marker.status = LILOTAFS_STATUS_WRAP_MARKER;
		wrap_marker.data_len = 0;

		if (lilotafs_flash_write(context, &wrap_marker, wrap_offset, sizeof(struct lilotafs_rec_header))) {
			context->f_errno = LILOTAFS_EFLASH;
			desc->write_errno = context->f_errno;
			return -1;
		}

		// 2. write the data len to the reserved file, and set its status to 0 / deleted
		if (change_file_data_len(ctx, header, desc->offset)) {
			context->f_errno = LILOTAFS_EFLASH;
			desc->write_errno = context->f_errno;
			return -1;
		}
		if (change_file_status(ctx, header, LILOTAFS_STATUS_DELETED)) {
			context->f_errno = LILOTAFS_EFLASH;
			desc->write_errno = context->f_errno;
			return -1;
		}

		context->fs_tail = 0;

		// 3. at offset 0, add a new reserved file (FE) and leave its datalen FF FF FF FF
		uint32_t new_offset = UINT32_MAX;
		error_code = reserve_file(ctx, filename, &new_offset);
		if (error_code != LILOTAFS_SUCCESS) {
			context->f_errno = error_code;
			desc->write_errno = context->f_errno;
			return -1;
		}

		// 4. copy the content of the now-deleted file over to the new location
		// this is a pointer to the data in the old file (right before wrap marker)
		uint8_t *old_data = (uint8_t *) context->flash_mmap + data_start;

		// update data_start to the new location of the file, at offset 0
		data_start = sizeof(struct lilotafs_rec_header) + aligned_strnlen(ctx, filename, 64) + 1;
		data_start = lilotafs_align_up_32(data_start, LILOTAFS_DATA_ALIGN);
		lilotafs_flash_write(ctx, old_data, data_start, desc->offset);

		desc->position = 0;
	}
 
	// write buffer data starting from the offset stored in FD table
	if (lilotafs_flash_write(context, buffer, data_start + desc->offset, len)) {
		context->f_errno = LILOTAFS_EFLASH;
		desc->write_errno = context->f_errno;
		return -1;
	}

	if (file_data_len > context->largest_file_size) {
		context->largest_file_size = file_data_len;
		context->largest_filename_len = aligned_strnlen(ctx, filename, 64);
	}

	// if (desc->offset + len == context->largest_file_size && len < REC_HEADER_DATA_LEN(header)) {
	// 	context->largest_file_size = 0, context->largest_filename_len = 0;
	// 	struct lilotafs_rec_header *head_header = (struct lilotafs_rec_header *) (context->flash_mmap + context->fs_head);
	// 	scan_headers(context, head_header, lilotafs_flash_get_partition_size(context));
	// }
	// else if (len >= context->largest_file_size) {
	// 	context->largest_file_size = len;
	// 	context->largest_filename_len = aligned_strnlen(ctx, filename, 64);
	// }

	desc->offset += len;
	return len;
}

int lilotafs_get_size(void *ctx, int fd) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	if (check_fd(context, fd)) {
		context->f_errno = LILOTAFS_EBADF;
		return -1;
	}

	struct lilotafs_file_descriptor descriptor = context->fd_list[fd];
	struct lilotafs_rec_header *header = (struct lilotafs_rec_header *) (context->flash_mmap + descriptor.position);

	context->f_errno = LILOTAFS_SUCCESS;
	return REC_HEADER_DATA_LEN(header);
}

ssize_t lilotafs_read(void *ctx, int fd, void *buffer, size_t len) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	if (check_fd(context, fd)) {
		context->f_errno = LILOTAFS_EBADF;
		return -1;
	}

	struct lilotafs_file_descriptor *desc = &context->fd_list[fd];
	if (desc->flags != O_RDONLY) {
		context->f_errno = LILOTAFS_EPERM;
		return -1;
	}

	char *filename = (char *) context->flash_mmap + desc->position + sizeof(struct lilotafs_rec_header);
	uint32_t data_offset = desc->position + sizeof(struct lilotafs_rec_header) + aligned_strnlen(ctx, filename, 64) + 1;
	data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);
	uint8_t *data_start = (uint8_t *) context->flash_mmap + data_offset;

	lilotafs_aligned_memcpy(ctx, buffer, data_start + desc->offset, len);
	desc->offset += len;

	context->f_errno = LILOTAFS_SUCCESS;
	return len;
}

off_t lilotafs_lseek(void *ctx, int fd, off_t offset, int whence) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	if (check_fd(context, fd)) {
		context->f_errno = LILOTAFS_EBADF;
		return -1;
	}

	struct lilotafs_file_descriptor *descriptor = &context->fd_list[fd];
	if (descriptor->flags != O_RDONLY) {
		context->f_errno = LILOTAFS_EPERM;
		return -1;
	}

	if (whence == SEEK_SET)
		descriptor->offset = offset;
	else if (whence == SEEK_CUR)
		descriptor->offset += offset;
	else if (whence == SEEK_END) {
		int size = lilotafs_get_size(ctx, fd);
		// get_size already sets errno on error
		if (size == -1)
			return -1;
		descriptor->offset = size + offset;
	}
	else {
		context->f_errno = LILOTAFS_EINVAL;
		return -1;
	}

	context->f_errno = LILOTAFS_SUCCESS;
	return descriptor->offset;
}

// int lilotafs_delete(void *ctx, int fd) {
// 	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
//
// 	if (check_fd(context, fd)) {
// 		return LILOTAFS_EBADF;
// 	}
//
// 	struct lilotafs_file_descriptor descriptor = context->fd_list[fd];
// 	struct lilotafs_rec_header *header = (struct lilotafs_rec_header *) (context->flash_mmap + descriptor.position);
// 	if (change_file_status(context, header, LILOTAFS_STATUS_DELETED))
// 		return LILOTAFS_EFLASH;
//
// 	if (REC_HEADER_DATA_LEN(header) == context->largest_filename_len) {
// 		context->largest_file_size = 0, context->largest_filename_len = 0;
// 		struct lilotafs_rec_header *head_header = (struct lilotafs_rec_header *) (context->flash_mmap + context->fs_head);
// 		scan_headers(context, head_header);
// 	}
//
// 	if (!context->has_wear_marker) {
// 		char empty = 0;
// 		uint32_t current_offset = UINT32_MAX;
// 		context->has_wear_marker = true;
// 		return append_file(context, &empty, &current_offset, NULL, 0, LILOTAFS_STATUS_WEAR_MARKER, false);
// 	}
//
// 	return LILOTAFS_SUCCESS;
// }

struct dirent lilotafs_de;

struct lilotafs_dir {
#ifndef LILOTAFS_LOCAL
	DIR dir;
#endif
	char *name;
	struct lilotafs_rec_header *cur_file;
};

char *add_slash(struct lilotafs_context *ctx, const char *name) {
	int name_len = aligned_strnlen(ctx, name, 64);
	if (name_len == 0)
		return (char *) name;
	char *name_slash = (char *) name;
	if (name[name_len - 1] != '/') {
		name_slash = (char *) malloc(name_len + 2);
		for (int i = 0; i < name_len; i++)
			name_slash[i] = name[i];
		name_slash[name_len] = '/';
		name_slash[name_len + 1] = 0;
	}
	return name_slash;
}

int lilotafs_mkdir(void *ctx, const char *name, mode_t mode) {
	(void) mode;

	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	for (int i = 0; i < context->fd_list_size; i++) {
		struct lilotafs_file_descriptor *descriptor = &context->fd_list[i];
		if (!descriptor->in_use)
			continue;
		if (descriptor->flags & O_WRONLY) {
			context->f_errno = LILOTAFS_EPERM;
			return -1;
		}
	}

	char *name_no_prefix = remove_prefix_slash(ctx, name);
	char *actual_name = add_slash(ctx, name_no_prefix);
	if (name_no_prefix != actual_name && name_no_prefix != name)
		free(name_no_prefix);

	uint32_t offset = UINT32_MAX;
	int code = append_file(ctx, actual_name, &offset, NULL, 0, LILOTAFS_STATUS_COMMITTED, false, false);
	if (code != LILOTAFS_SUCCESS) {
		if (actual_name != name)
			free(actual_name);
		context->f_errno = code;
		return -1;
	}
	
	if (actual_name != name)
		free(actual_name);

	context->f_errno = LILOTAFS_SUCCESS;
	return LILOTAFS_SUCCESS;
}

DIR *lilotafs_opendir(void *ctx, const char *name) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	char *actual_name;
	if (aligned_strnlen(ctx, name, 64) == 0)
		actual_name = (char *) name;
	else {
		char *name_no_prefix = remove_prefix_slash(ctx, name);
		actual_name = add_slash(ctx, name_no_prefix);
		if (name_no_prefix != actual_name && name_no_prefix != name)
			free(name_no_prefix);

		if (aligned_strneq(ctx, actual_name, "./", 64) == 0) {
			if (actual_name != name)
				free(actual_name);
			actual_name = (char *) malloc(1);
			actual_name[0] = 0;
		}
	}

	struct lilotafs_dir *dir = (struct lilotafs_dir *) calloc(1, sizeof(struct lilotafs_dir));
	if (dir == NULL) {
		if (actual_name != name)
			free(actual_name);
		context->f_errno = LILOTAFS_EUNKNOWN;
		return NULL;
	}

	if (actual_name == name) {
		int name_len = aligned_strnlen(ctx, name, 64);
		dir->name = (char *) malloc(name_len + 1);
		for (int i = 0; i < name_len; i++)
			dir->name[i] = name[i];
		dir->name[name_len] = 0;
	}
	else
		dir->name = actual_name;

	dir->cur_file = (struct lilotafs_rec_header *) (context->flash_mmap + context->fs_head);

	if (lilotafs_flash_flush(ctx, 0, lilotafs_flash_get_partition_size(ctx))) {
		context->f_errno = LILOTAFS_EFLASH;
		return NULL;
	}

	return (DIR *) dir;
}

// check if a file is "directly under" a directory and should be returned by readdir
bool is_dir_child(struct lilotafs_context *ctx, const char *dir, const char *file) {
	// if dir is "" (length 0) then we are listing the root:
	//  - files do not have slash anywhere
	//  - directories only have one slash, which is the last character
	// fprintf(stderr, "check is %s child of %s?\n", file, dir);

	if (aligned_strnlen(ctx, dir, 64) == 0) {
		// exclude the last character, because might be / for directories
		// if returns -1, that means no / found before the last character
		// which means the file is under the root directory
		uint32_t cur_group = 0;
		for (uint32_t i = 0; i < aligned_strnlen(ctx, file, 64) - 1; i++) {
			if (i % 4 == 0)
				cur_group = *((uint32_t *) lilotafs_align_down_ptr((uint8_t *) file + i, 4));
			uint32_t group_index = i % 4;
			uint8_t cur = (cur_group >> (group_index * 8)) & 0xFF;
			if (cur == '/')
				return false;
		}
		return true;
	}

	// check that everything up to the last slash (including the slash)
	// is equal to the directory
	char *file_dir = get_file_directory(ctx, file);
	if (file_dir == NULL)
		return false;

	// fprintf(stderr, "dir %s\n", file_dir);
	// fprintf(stderr, "%lu %lu\n", aligned_strnlen(ctx, file_dir, 64), aligned_strnlen(ctx, file, 64));

	int cmp = aligned_strneq(ctx, dir, file_dir, 64);
	free(file_dir);

	// fprintf(stderr, "good? %u\n", cmp);
	return cmp == 0;
}

struct dirent *lilotafs_readdir(void *ctx, DIR *pdir) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	struct lilotafs_dir *dir = (struct lilotafs_dir *) pdir;

	char *full_path;
	struct lilotafs_rec_header *cur_header = dir->cur_file;
	while (1) {
		full_path = (char *) cur_header + sizeof(struct lilotafs_rec_header);

		if (
			REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_COMMITTED &&
			aligned_strnlen(ctx, full_path, 64) != 0 &&
			is_dir_child(ctx, dir->name, full_path) &&
			cur_header != dir->cur_file &&
			aligned_strneq(ctx, full_path, dir->name, 64) != 0
		) {
			// printf("file %s ok\n", full_path);
			break;
		}

		struct lilotafs_rec_header *next_header = process_advance_header(context, cur_header, lilotafs_flash_get_partition_size(ctx));
		if (!next_header)
			return NULL;

		cur_header = next_header;
		if (REC_HEADER_MAGIC(cur_header) != LILOTAFS_RECORD && REC_HEADER_MAGIC(cur_header) != LILOTAFS_START &&
				REC_HEADER_MAGIC(cur_header) != LILOTAFS_START_CLEAN && REC_HEADER_MAGIC(cur_header) != LILOTAFS_WRAP_MARKER) {

			return NULL;
		}
	}

	int full_path_len = aligned_strnlen(ctx, full_path, 64);

	lilotafs_de.d_ino = (POINTER_SIZE) cur_header - (POINTER_SIZE) context->flash_mmap;
	lilotafs_de.d_type = lilotafs_mmap_read_byte(&full_path[full_path_len - 1]) == '/' ? DT_DIR : DT_REG;

	// clear the filename to all 0
	memset(&lilotafs_de.d_name, 0, sizeof(lilotafs_de.d_name));

	// the file we return must have the directory stripped
	int path_len = aligned_strnlen(ctx, dir->name, 64);
	int filename_len = full_path_len - path_len;
	lilotafs_aligned_memcpy(ctx, lilotafs_de.d_name, full_path + path_len, filename_len + 1);
	dir->cur_file = cur_header;

	return &lilotafs_de;
}

int lilotafs_closedir(void *ctx, DIR *pdir) {
	(void) ctx;

	free(((struct lilotafs_dir *) pdir)->name);
	free(pdir);
	return 0;
}


uint32_t lilotafs_count_files(void *ctx) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t count = 0;
	uint32_t partition_size = lilotafs_flash_get_partition_size(context);
	struct lilotafs_rec_header *cur_header = (struct lilotafs_rec_header *) (context->flash_mmap + context->fs_head);

	if (REC_HEADER_MAGIC(cur_header) != LILOTAFS_RECORD && REC_HEADER_MAGIC(cur_header) != LILOTAFS_START &&
			REC_HEADER_MAGIC(cur_header) != LILOTAFS_START_CLEAN && REC_HEADER_MAGIC(cur_header) != LILOTAFS_WRAP_MARKER) {

		return count;
	}

	char *filename = (char *) cur_header + sizeof(struct lilotafs_rec_header);
	if (REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_COMMITTED && (REC_HEADER_MAGIC(cur_header) != LILOTAFS_START || aligned_strnlen(ctx, filename, 64) != 0))
		count++;

	while (1) {
		struct lilotafs_rec_header *next_header = process_advance_header(context, cur_header, partition_size);
		if (!next_header)
			break;
		cur_header = next_header;

		if (REC_HEADER_MAGIC(cur_header) != LILOTAFS_RECORD && REC_HEADER_MAGIC(cur_header) != LILOTAFS_START &&
			REC_HEADER_MAGIC(cur_header) != LILOTAFS_START_CLEAN && REC_HEADER_MAGIC(cur_header) != LILOTAFS_WRAP_MARKER) {

			return count;
		}

		filename = (char *) cur_header + sizeof(struct lilotafs_rec_header);
		if (REC_HEADER_STATUS(cur_header) == LILOTAFS_STATUS_COMMITTED && (REC_HEADER_MAGIC(cur_header) != LILOTAFS_START || aligned_strnlen(ctx, filename, 64) != 0))
			count++;
	}

	return count;
}

uint32_t lilotafs_get_largest_file_size(void *ctx) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	return context->largest_file_size;
}
uint32_t lilotafs_get_largest_filename_len(void *ctx) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	return context->largest_filename_len;
}
uint32_t lilotafs_get_head(void *ctx) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	return context->fs_head;
}
uint32_t lilotafs_get_tail(void *ctx) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	return context->fs_tail;
}

