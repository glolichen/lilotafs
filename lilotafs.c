#include "lilotafs.h"
#include <dirent.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef LILOTAFS_LOCAL
#include <sys/mman.h>
#include <sys/types.h>
#else
#include "spi_flash_mmap.h"
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "flash.h"
#include "util.h"

LILOTAFS_FUNC_STR_ENDS_WITH
LILOTAFS_FUNC_GET_SMALLEST_COMPATIBLE

#define READ_FILENAME(filename, offset) (lilotafs_flash_read(ctx, filename, offset + sizeof(struct lilotafs_rec_header), 64))

uint16_t get_file_magic(struct lilotafs_context *ctx, uint32_t offset) {
	uint16_t magic;
	lilotafs_flash_read(ctx, &magic, offset + offsetof(struct lilotafs_rec_header, magic), 2);
	return magic;
}
uint8_t get_file_status(struct lilotafs_context *ctx, uint32_t offset) {
	uint8_t status;
	lilotafs_flash_read(ctx, &status, offset + offsetof(struct lilotafs_rec_header, status), 1);
	return status;
}
uint32_t get_file_data_len(struct lilotafs_context *ctx, uint32_t offset) {
	uint32_t data_len;
	lilotafs_flash_read(ctx, &data_len, offset + offsetof(struct lilotafs_rec_header, data_len), 4);
	return data_len;
}

void *copy_file_mmap(struct lilotafs_context *ctx, uint32_t offset, uint32_t len) {
	void *file_data = malloc(len); 
	lilotafs_flash_read(ctx, file_data, offset, len);
	return file_data;
}
void copy_file_munmap(void *file_data, uint32_t len) {
	(void) len;
	free(file_data);
}

uint32_t u32_min(uint32_t a, uint32_t b) {
	return a < b ? a : b;
}
uint32_t u32_max(uint32_t a, uint32_t b) {
	return a > b ? a : b;
}

uint32_t scan_for_header(void *ctx, uint32_t start, uint32_t partition_size) {
	start = lilotafs_align_up_32(start, LILOTAFS_HEADER_ALIGN);
	for (uint32_t i = start; i <= partition_size - sizeof(struct lilotafs_rec_header); i += LILOTAFS_HEADER_ALIGN) {
		if (get_file_magic(ctx, i) == LILOTAFS_START || get_file_magic(ctx, i) == LILOTAFS_START_CLEAN)
			return i;
	}
	return UINT32_MAX;
}

bool file_is_kernel(struct lilotafs_context *ctx, uint32_t offset) {
	char filename[64];
	READ_FILENAME(filename, offset);
	return str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64);
}

int change_file_magic(void *ctx, uint32_t file_offset, uint16_t magic) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	uint32_t magic_addr = file_offset + offsetof(struct lilotafs_rec_header, magic);
	if (lilotafs_flash_write(context, &magic, magic_addr, 2))
		return LILOTAFS_EFLASH;
	return LILOTAFS_SUCCESS;
}
int change_file_status(void *ctx, uint32_t file_offset, uint8_t status) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	uint32_t status_addr = file_offset + offsetof(struct lilotafs_rec_header, status);
	if (lilotafs_flash_write(context, &status, status_addr, 1))
		return LILOTAFS_EFLASH;
	return LILOTAFS_SUCCESS;
}

// for little endian: write the data_len in REVERSE (highest value byte first)
// this means that in case of a crash, we have more "flexibility" with the lower bits
int change_file_data_len(void *ctx, uint32_t file_offset, uint32_t data_len) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	uint32_t data_len_addr = file_offset + offsetof(struct lilotafs_rec_header, data_len);
	for (int i = 3; i >= 0; i--) {
		uint8_t byte = (data_len >> (8 * i)) & 0xFF;
		if (lilotafs_flash_write(context, &byte, data_len_addr + i, 1))
			return LILOTAFS_EFLASH;
	}
	return LILOTAFS_SUCCESS;
}

bool magic_is_wrap_marker(struct lilotafs_context *ctx, uint32_t file) {
	// the magic number for the wrap marker is 0x5AFA
	uint16_t magic = get_file_magic(ctx, file);
	if (magic == LILOTAFS_WRAP_MARKER)
		return true;
	// check first byte of magic number
	if ((magic & 0xFF) == 0xFA)
		return true;
	return false;
}


uint32_t process_advance_header(void *ctx, uint32_t current_offset, uint32_t partition_size) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	if (magic_is_wrap_marker(ctx, current_offset) || get_file_status(ctx, current_offset) == LILOTAFS_STATUS_WRAP_MARKER) {
		// if we crash while writing the data_len field of the wrap marker
		if (get_file_data_len(ctx, current_offset) != 0) {
			if (change_file_magic(context, current_offset, LILOTAFS_WRAP_MARKER))
				return UINT32_MAX;
			if (change_file_status(context, current_offset, LILOTAFS_STATUS_WRAP_MARKER))
				return UINT32_MAX;
			if (change_file_data_len(context, current_offset, 0))
				return UINT32_MAX;
		}
		return 0;
	}

	if (current_offset + sizeof(struct lilotafs_rec_header) >= partition_size)
		return UINT32_MAX;

	// file name immediately after header
	char filename[64];
	READ_FILENAME(filename, current_offset);
	uint32_t filename_len_padded = strnlen(filename, 64);

	// unterminated filename
	if (filename_len_padded == 64)
		return UINT32_MAX;

	// add 1 for null terminator
	filename_len_padded++;

	// Malformed record - treat as end of valid data
	if (get_file_data_len(ctx, current_offset) > partition_size - current_offset
			- sizeof(struct lilotafs_rec_header) - filename_len_padded) {

		return UINT32_MAX;
	}

	uint32_t next_offset = current_offset + sizeof(struct lilotafs_rec_header) + filename_len_padded;
	if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
		next_offset = lilotafs_align_up_32(next_offset, LILOTAFS_DATA_ALIGN_KERNEL);
	else
		next_offset = lilotafs_align_up_32(next_offset, LILOTAFS_DATA_ALIGN);
	next_offset += get_file_data_len(ctx, current_offset);
	next_offset = lilotafs_align_up_32(next_offset, LILOTAFS_HEADER_ALIGN);

	if (next_offset >= partition_size)
		return UINT32_MAX;

	if (get_file_status(ctx, current_offset) == LILOTAFS_STATUS_COMMITTED) {
		uint32_t worst_file_size = sizeof(struct lilotafs_rec_header) + filename_len_padded;
		worst_file_size += get_file_data_len(ctx, current_offset);
		if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
			worst_file_size += LILOTAFS_DATA_ALIGN_KERNEL;
		else
			worst_file_size += LILOTAFS_DATA_ALIGN;
		worst_file_size += LILOTAFS_HEADER_ALIGN;

		if (worst_file_size > context->largest_worst_file_size)
			context->largest_worst_file_size = worst_file_size;
	}

	return next_offset;
}

// returns information on the FIRST FREE SPACE immediately following the last valid record
// reserved files are not valid records, so this function will return reserved as the last_header
// also returns a pointer to the wear marker, wrap marker, migrating and reserved files
// as well as the number of files for use in wear leveling
// this function will also find the largest file size between the start and the tail
// and update that in ctx->largest_filename_len
struct scan_headers_result {
	uint32_t last_header, wear_marker, wrap_marker, migrating, reserved;
	uint32_t num_files;
} scan_headers(void *ctx, uint32_t start) {

	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	uint32_t partition_size = lilotafs_flash_get_partition_size(ctx);

	uint32_t cur_header = start;
	struct scan_headers_result ret = {
		.last_header = cur_header,
		.wear_marker = UINT32_MAX,
		.wrap_marker = UINT32_MAX,
		.migrating = UINT32_MAX,
		.reserved = UINT32_MAX,
		.num_files = 0
	};

	if (get_file_magic(ctx, cur_header) != LILOTAFS_RECORD &&
		get_file_magic(ctx, cur_header) != LILOTAFS_START &&
		get_file_magic(ctx, cur_header) != LILOTAFS_START_CLEAN &&
		get_file_magic(ctx, cur_header) != LILOTAFS_WRAP_MARKER) {

		return ret;
	}

	while (1) {
		// if mount creates a dummy header, do not count it as a file (it is empty)
		char filename[64];
		READ_FILENAME(filename, cur_header);

		if (get_file_status(ctx, cur_header) == LILOTAFS_STATUS_COMMITTED &&
				(get_file_magic(ctx, cur_header) != LILOTAFS_START || strnlen(filename, 64) != 0)) {

			ret.num_files++;
		}

		if (get_file_status(ctx, cur_header) == LILOTAFS_STATUS_WEAR_MARKER)
			ret.wear_marker = cur_header;
		if (magic_is_wrap_marker(ctx, cur_header) || get_file_status(ctx, cur_header) == LILOTAFS_STATUS_WRAP_MARKER)
			ret.wrap_marker = cur_header;
		if (get_file_status(ctx, cur_header) == LILOTAFS_STATUS_MIGRATING)
			ret.migrating = cur_header;

		// weird edge case: if the cur_header has status FF
		// becuse of a crash after writing magic but before writing reserved status
		// treat it like reserved
		if (get_file_status(ctx, cur_header) == LILOTAFS_STATUS_RESERVED || get_file_status(ctx, cur_header) == 0xFF) {
			ret.reserved = cur_header;
			break;
		}

		uint32_t next_header = process_advance_header(context, cur_header, partition_size);
		if (next_header == UINT32_MAX)
			break;

		cur_header = next_header;
		if (get_file_magic(ctx, cur_header) != LILOTAFS_RECORD &&
			get_file_magic(ctx, cur_header) != LILOTAFS_START &&
			get_file_magic(ctx, cur_header) != LILOTAFS_START_CLEAN &&
			get_file_magic(ctx, cur_header) != LILOTAFS_WRAP_MARKER) {
			break;
		}
	}
	ret.last_header = cur_header;
	return ret;
}

uint32_t calculate_total_file_size(uint32_t filename_len, uint32_t data_len, bool is_kernel) {
	uint32_t new_file_total = sizeof(struct lilotafs_rec_header) + filename_len + 1;
	if (is_kernel)
		new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_DATA_ALIGN_KERNEL);
	else
		new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_DATA_ALIGN);
	new_file_total += data_len;
	new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_HEADER_ALIGN);
	return new_file_total;
}

// the total_size_size includes the header, filename and datalen
int check_free_space(void *ctx, uint32_t current_offset, uint32_t write_offset,
					 const char *new_filename, uint32_t new_data_len) {


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
		// the old and new file names should really be the same, but checking to be safe
		char old_filename[64];
		READ_FILENAME(old_filename, current_offset);
		uint32_t old_filename_len = strnlen(old_filename, 64);

		old_file_total = sizeof(struct lilotafs_rec_header) + old_filename_len + 1;
		old_file_total += get_file_data_len(ctx, current_offset);
		if (file_is_kernel(ctx, current_offset))
			old_file_total += LILOTAFS_DATA_ALIGN_KERNEL;
		else
			old_file_total += LILOTAFS_DATA_ALIGN;
		old_file_total += LILOTAFS_HEADER_ALIGN;
	}

	uint32_t new_file_total = sizeof(struct lilotafs_rec_header) + strnlen(new_filename, 64) + 1;
	new_file_total += new_data_len;
	if (str_ends_with(new_filename, LILOTAFS_KERNEL_EXT, 64))
		new_file_total += LILOTAFS_DATA_ALIGN_KERNEL;
	else
		new_file_total += LILOTAFS_DATA_ALIGN;
	new_file_total += LILOTAFS_HEADER_ALIGN;

	uint32_t wrap_marker_total = sizeof(struct lilotafs_rec_header);
	wrap_marker_total = lilotafs_align_up_32(wrap_marker_total, LILOTAFS_HEADER_ALIGN);

	uint32_t wear_marker_total = sizeof(struct lilotafs_rec_header) + 1;
	wrap_marker_total = lilotafs_align_up_32(wrap_marker_total, LILOTAFS_HEADER_ALIGN);

	// the current file we're writing might be larger than current largest
	uint32_t largest_file_total = u32_max(context->largest_worst_file_size, u32_max(new_file_total, old_file_total));

	// hypothetical tail pointer if we added this file
	// log_wrap indicates whether there is already a wear marker,
	// or if one is needed in case we add the current file, and copy
	// the largest file twiec
	bool log_wrap = context->fs_head >= context->fs_tail;
	uint32_t current_position = write_offset;
	if (new_file_total + wrap_marker_total > partition_size - current_position) {
		if (log_wrap)
			return LILOTAFS_ENOSPC;
		log_wrap = true;
		current_position = 0;
	}
	current_position += new_file_total;
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

	uint32_t filename_len = strnlen(filename, 64);
	uint32_t partition_size = lilotafs_flash_get_partition_size(context);

	bool is_kernel = str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64);
	if (!is_wear_level && check_free_space(ctx, *current_offset, context->fs_tail, filename, len))
		return LILOTAFS_ENOSPC;

	uint32_t new_file_total = calculate_total_file_size(is_kernel, filename_len, len);

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
		if (change_file_status(context, *current_offset, LILOTAFS_STATUS_MIGRATING))
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
	if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
		data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN_KERNEL);
	else
		data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);
	if (lilotafs_flash_write(context, filename, new_file_offset + sizeof(struct lilotafs_rec_header), filename_len + 1))
		return LILOTAFS_EFLASH;
	if (len) {
		if (lilotafs_flash_write(context, buffer, data_offset, len))
			return LILOTAFS_EFLASH;
	}

	// phase 3: set size and commit
	if (change_file_data_len(ctx, new_file_offset, len))
		return LILOTAFS_EFLASH;
	if (change_file_status(ctx, new_file_offset, want_status))
		return LILOTAFS_EFLASH;

	// phase 4: delete old
	if (*current_offset != UINT32_MAX) {
		if (change_file_status(context, *current_offset, LILOTAFS_STATUS_DELETED))
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

	uint32_t filename_len = strnlen(filename, 64);
	uint32_t partition_size = lilotafs_flash_get_partition_size(context);

	if (check_free_space(ctx, *current_offset, context->fs_tail, filename, 0))
		return LILOTAFS_ENOSPC;

	uint32_t new_file_total = sizeof(struct lilotafs_rec_header) + filename_len + 1;
	if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
		new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_DATA_ALIGN_KERNEL);
	else
		new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_DATA_ALIGN);
	new_file_total += 0;
	new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_HEADER_ALIGN);

	uint32_t wrap_marker_total = sizeof(struct lilotafs_rec_header);
	wrap_marker_total = lilotafs_align_up_32(wrap_marker_total, LILOTAFS_HEADER_ALIGN);

	// mark old file as migrating
	if (*current_offset != UINT32_MAX) {
		if (change_file_status(context, *current_offset, LILOTAFS_STATUS_MIGRATING))
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

	struct lilotafs_rec_header file_header;
	memset(&file_header, 0, sizeof(file_header));
	file_header.magic = LILOTAFS_RECORD;
	file_header.status = LILOTAFS_STATUS_RESERVED;
	file_header.data_len = 0xFFFFFFFF;

	if (lilotafs_flash_write(context, &file_header, new_file_offset, sizeof(struct lilotafs_rec_header)))
		return LILOTAFS_EFLASH;

	uint32_t data_offset = new_file_offset + sizeof(struct lilotafs_rec_header) + filename_len + 1;
	if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
		data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN_KERNEL);
	else
		data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);
	if (lilotafs_flash_write(context, filename, new_file_offset + sizeof(struct lilotafs_rec_header), filename_len + 1))
		return LILOTAFS_EFLASH;

	context->fs_tail = lilotafs_align_up_32(data_offset, LILOTAFS_HEADER_ALIGN);
	*current_offset = new_file_offset;

	return LILOTAFS_SUCCESS;
}

int remove_false_magic(void *ctx, uint32_t start, uint32_t size) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t end_offset = start + size;
	start = lilotafs_align_up_32(start, LILOTAFS_HEADER_ALIGN);

	for (uint32_t i = start; i < end_offset; i += LILOTAFS_HEADER_ALIGN) {
		uint16_t magic = get_file_magic(ctx, i);
		// if data is any magic number, set it to 0 to avoid picking it up by mistake
		if (magic == LILOTAFS_RECORD || magic == LILOTAFS_START || magic == LILOTAFS_START_CLEAN) {
			uint16_t zero = 0;
			if (lilotafs_flash_write(context, &zero, i, 2))
				return LILOTAFS_EFLASH;
		}
	}

	return LILOTAFS_SUCCESS;
}

int clobber_file_data(void *ctx, uint32_t file) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	if (magic_is_wrap_marker(ctx, file) || get_file_status(ctx, file) == LILOTAFS_STATUS_WRAP_MARKER)
		return LILOTAFS_SUCCESS;

	char filename[64];
	uint32_t filename_offset = file + sizeof(struct lilotafs_rec_header);
	lilotafs_flash_read(ctx, filename, filename_offset, 64);

	uint32_t data_offset = filename_offset + strnlen(filename, 64) + 1;
	if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
		data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN_KERNEL);
	else
		data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);
	data_offset += get_file_data_len(ctx, file);

	return remove_false_magic(context, filename_offset, data_offset - filename_offset);
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
		if (remove_false_magic(context, sector_start, next_offset % sector_size))
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
int wear_level_compact(void *ctx, uint32_t wear_marker, uint32_t num_files) {
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

	uint32_t cur_header = context->fs_head;
	while (1) {
		uint32_t next_header = process_advance_header(context, cur_header, partition_size);

		if (next_header == UINT32_MAX)
			break;

		if (get_file_magic(ctx, next_header) != LILOTAFS_RECORD &&
			get_file_magic(ctx, next_header) != LILOTAFS_START &&
			get_file_magic(ctx, next_header) != LILOTAFS_START_CLEAN &&
			get_file_magic(ctx, next_header) != LILOTAFS_WRAP_MARKER
		) {
			break;
		}

		// uint32_t cur_offset = GET_OFFSET(cur_header, context);
		// uint32_t next_offset = GET_OFFSET(next_header, context);

		// move the current file to the tail
		uint32_t cur_offset_after_move = cur_header;

		// NOTE: if we mount the filesystem and do not find a LILOTAFS_START record
		// we create a record at offset 0 with magic LILOTAFS_START and length 0
		// we do NOT want to move that record

		// if file is committed, append it to the tail
		if (!(get_file_data_len(ctx, cur_header) == 0 && cur_header == 0) &&
				get_file_status(ctx, cur_header) == LILOTAFS_STATUS_COMMITTED) {

			count--;

			char filename[64];
			READ_FILENAME(filename, cur_header);

			uint32_t data_offset = cur_header + sizeof(struct lilotafs_rec_header) + strnlen(filename, 64) + 1;
			if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
				data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN_KERNEL);
			else
				data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);

			uint32_t data_len = get_file_data_len(ctx, cur_header);
			void *file_data = copy_file_mmap(ctx, data_offset, data_len);
			int code = append_file(
				context, filename, &cur_offset_after_move, file_data,
				data_len, LILOTAFS_STATUS_COMMITTED, false, true
			);
			copy_file_munmap(file_data, data_len);
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
		if (next_header / sector_size != cur_header / sector_size) {
			// if next_offset is 0, then we are processing a wrap marker
			// the wrap marker does not actually cross the flash erase boundar
			// we can simply erase all of the last sector
			code = erase_file(context, cur_header, next_header == 0 ? lilotafs_flash_get_partition_size(context) : next_header);
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
		if (next_header / sector_size == cur_header / sector_size) {
			if (change_file_magic(context, cur_header, 0))
				return LILOTAFS_EFLASH;
		}

		context->fs_head = next_header;
		cur_header = next_header;

		if (count == 0)
			break;
	}

	return LILOTAFS_SUCCESS;
}

uint32_t find_file_name(void *ctx, const char *name, bool accept_migrating) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t filename_len = strnlen(name, 64);
	uint32_t partition_size = lilotafs_flash_get_partition_size(context);

	uint32_t cur_header = context->fs_head;
	while (1) {
		if (get_file_status(ctx, cur_header) == LILOTAFS_STATUS_COMMITTED ||
				(accept_migrating && get_file_status(ctx, cur_header) == LILOTAFS_STATUS_MIGRATING)) {

			char cur_filename[64];
			READ_FILENAME(cur_filename, cur_header);

			// PRINTF("found file %s\n", cur_filename);
			if (strnlen(cur_filename, 64) == filename_len &&
					strncmp(cur_filename, name, 64) == 0) {

				return cur_header;
			}
		}

		uint32_t next_header = process_advance_header(context, cur_header, partition_size);
		if (next_header == UINT32_MAX)
			break;
		cur_header = next_header;
		if (get_file_magic(ctx, cur_header) != LILOTAFS_RECORD && get_file_magic(ctx, cur_header) != LILOTAFS_WRAP_MARKER)
			break;
	}
	return UINT32_MAX;
}

// simulate graceful computer shutdown
uint32_t lilotafs_unmount(void *ctx) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	context->largest_worst_file_size = 0;
	context->fs_head = 0, context->fs_tail = 0;
	context->has_wear_marker = false;

	// close all open FDs
	if (context->fd_list_capacity != 0 && context->fd_list != NULL)
		free(context->fd_list);
	context->fd_list_size = 0, context->fd_list_capacity = 0;

#ifdef LILOTAFS_LOCAL
	munmap(context->flash_mmap, lilotafs_flash_get_partition_size(context));
#else
	// esp_partition_munmap(context->map_handle);
	spi_flash_munmap(context->map_handle);
#endif

	return LILOTAFS_SUCCESS;
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
	
	// const void **flash_mmap_addr = (const void **) &context->flash_mmap;
	// esp_partition_mmap(partition, 0, 0x180000, ESP_PARTITION_MMAP_INST, flash_mmap_addr, &context->map_handle);
	// esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_INST, flash_mmap_addr, &context->map_handle);
	//
	// fprintf(stderr, "mmap addr: %p\n", context->flash_mmap);
#endif

	context->largest_worst_file_size = 0;
	context->fs_head = 0, context->fs_tail = 0;
	context->has_wear_marker = false;
	context->f_errno = 0;
	
	uint32_t cur_header = 0, wear_marker = UINT32_MAX, num_files = 0;
	
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
	if (get_file_magic(ctx, cur_header) != LILOTAFS_START) {
		PRINTF("offset 0 is not FS_START\n");

		// follow current file until no more
		// (if no file at 0 this will return cur_header immediately, then offset = 0)
		struct scan_headers_result scan_result = scan_headers(ctx, cur_header);
		cur_header = scan_result.last_header;
		if (scan_result.wear_marker != UINT32_MAX)
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

		uint32_t offset = cur_header;
		if (cur_header == scan_result.reserved && (get_file_status(ctx, cur_header) == LILOTAFS_STATUS_RESERVED || get_file_status(ctx, cur_header) == 0xFF)) {
			char reserved_filename[64];
			READ_FILENAME(reserved_filename, scan_result.reserved);

			uint32_t reserved_filename_len = strnlen(reserved_filename, 64);

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
				if (str_ends_with(reserved_filename, LILOTAFS_KERNEL_EXT, 64))
					data_start = lilotafs_align_up_32(data_start, LILOTAFS_DATA_ALIGN_KERNEL);
				else
					data_start = lilotafs_align_up_32(data_start, LILOTAFS_DATA_ALIGN);

				uint32_t starting_block = lilotafs_align_up_32(data_start, block_size);
				uint32_t data_len = starting_block - data_start;

				// possibly crashed while writing data_len, need to make sure is compatible
				
				// cases to consider: if the original file did NOT cross the next flash erase boundary
				// and enough of it was written such that the current value in there is less than
				// the value we calculated (data_len), then we can believe there is an upper bound on
				// the size of that file, since we know not more was written (data_len starts at FF)
				// we can make this assumption because data_len writes from the HIGHEST value byte first
				// if (get_file_data_len(ctx, scan_result.reserved) <= data_len)
				// 	data_len = get_file_data_len(ctx, scan_result.reserved);
				// else
				data_len = get_smallest_compatible(data_len, get_file_data_len(ctx, scan_result.reserved));

				cur_header = UINT32_MAX;
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

					if (get_file_magic(ctx, addr) != LILOTAFS_START && get_file_magic(ctx, addr) != LILOTAFS_START_CLEAN)
						continue;

					if (get_file_status(ctx, addr) != LILOTAFS_STATUS_RESERVED && 
						get_file_status(ctx, addr) != LILOTAFS_STATUS_COMMITTED &&
						get_file_status(ctx, addr) != LILOTAFS_STATUS_MIGRATING &&
						get_file_status(ctx, addr) != LILOTAFS_STATUS_WRAP_MARKER &&
						get_file_status(ctx, addr) != LILOTAFS_STATUS_WEAR_MARKER &&
						get_file_status(ctx, addr) != LILOTAFS_STATUS_DELETED) {
						continue;
					}

					uint32_t cur_filename_len = 0;
					if (magic_is_wrap_marker(ctx, addr) || get_file_status(ctx, addr) == LILOTAFS_STATUS_WRAP_MARKER) {
						bool is_real_wrap_marker = true;
						for (uint32_t i = addr + sizeof(struct lilotafs_rec_header);
								i < lilotafs_flash_get_partition_size(ctx); i++) {

							uint8_t byte;
							lilotafs_flash_read(ctx, &byte, i, 1);
							if (byte != 0xFF) {
								is_real_wrap_marker = false;
								break;
							}
						}

						if (!is_real_wrap_marker)
							continue;
					}
					else {
						char cur_filename[64];
						READ_FILENAME(cur_filename, addr);
						cur_filename_len = strnlen(cur_filename, 64);
					}

					// equal 64 = no null terminator
					if (cur_filename_len == 64)
						continue;

					struct scan_headers_result loop_scan_result = scan_headers(ctx, addr);
					if (loop_scan_result.last_header == scan_result.last_header) {
						cur_header = addr;
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
		if (cur_header == UINT32_MAX && get_file_magic(ctx, 0) == LILOTAFS_START_CLEAN)
			cur_header = 0;
	}

	// if none found, write a LILOTAFS_START header at 0
	if (cur_header == UINT32_MAX) {
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
	//      and set the first record to 00

	// 1. if the first is LILOTAFS_START_CLEAN
	if (get_file_magic(ctx, cur_header) == LILOTAFS_START_CLEAN) {
		if (change_file_magic(context, cur_header, LILOTAFS_START)) {
			context->f_errno = LILOTAFS_EFLASH;
			return LILOTAFS_EFLASH;
		}
	}
	else {
		PRINTF("first is not start clean\n");

		uint32_t next_header = process_advance_header(context, cur_header, lilotafs_flash_get_partition_size(ctx));
		if (next_header != UINT32_MAX) {
			// 2. crash after the file has been cleaned up, simply delete the old file
			if (get_file_magic(ctx, next_header) == LILOTAFS_START) {
				PRINTF("next afterwards is FS_START\n");
				if (change_file_magic(context, cur_header, 0)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}
				cur_header = next_header;
			}
			// 3. complicated
			if (get_file_magic(ctx, next_header) == LILOTAFS_START_CLEAN) {
				PRINTF("next afterwards is FS_START_CLEAN\n");

				// we cannot have any magic numbers in 32 byte boundaries, or they will be detected as files
				int code = clobber_file_data(context, cur_header);
				if (code != LILOTAFS_SUCCESS) {
					context->f_errno = code;
					return code;
				}

				// if the file crosses a flash boundary, or we are wrapping
				uint32_t sector_size = context->block_size;
				if (next_header / sector_size != cur_header / sector_size) {
					// if next_header is 0, then we are processing a wrap marker
					// the wrap marker does not actually cross the flash erase boundar
					// we can simply erase all of the last sector
					code = erase_file(context, cur_header, next_header == 0 ? lilotafs_flash_get_partition_size(context) : next_header);
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
				if (next_header / sector_size == cur_header / sector_size) {
					if (change_file_magic(context, cur_header, 0)) {
						context->f_errno = LILOTAFS_EFLASH;
						return LILOTAFS_EFLASH;
					}
				}

				cur_header = next_header;
			}
		}
	}
	
	context->fs_head = cur_header;
	
	// scan until no more files -- set that to tail pointer
	struct scan_headers_result scan_result = scan_headers(ctx, cur_header);
	cur_header = scan_result.last_header;
	num_files += scan_result.num_files;
	if (scan_result.wear_marker != UINT32_MAX)
		wear_marker = scan_result.wear_marker;

	// this is not actually the tail! scan_headers ends on a reserved file
	// because we do not know how much of that file was successfully written
	context->fs_tail = cur_header;

	uint32_t old_reserved = scan_result.reserved;
	uint32_t old_migrating = scan_result.migrating;

	if (scan_result.reserved != UINT32_MAX) {
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

		uint32_t migrating = scan_result.migrating;
		uint32_t reserved = scan_result.reserved;

		uint32_t mig_offset = migrating;
		uint32_t res_offset = reserved;

		// check if the file name is completely written
		char res_filename[64];
		READ_FILENAME(res_filename, reserved);
		uint32_t res_filename_len = strnlen(res_filename, 64);

		// (a) crash while writing file data
		if (res_filename_len != 64) {
			// erase from sector immediately after data start

			// 1. erase every sector higher than the reserved file, until either fs_head or end of partition
			// 2. calculate the last non-FF byte, and calculate the file size
			// 3. write file size into data_len, or the lowest possible value if data_len is not FFFFFFFF
			// 4. set new fs_tail into context struct

			uint32_t data_start = res_offset + sizeof(struct lilotafs_rec_header);
			data_start += res_filename_len + 1;
			if (str_ends_with(res_filename, LILOTAFS_KERNEL_EXT, 64))
				data_start = lilotafs_align_up_32(data_start, LILOTAFS_DATA_ALIGN_KERNEL);
			else
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
			// while (context->flash_mmap[--file_end] == 0xFF);
			uint8_t last_byte = 0xFF;
			while (last_byte == 0xFF)
				lilotafs_flash_read(ctx, &last_byte, --file_end, 1);

			// calculate the actual size of the file
			// if file_end <= data_start, the file is all 0xFF and we are reading into padding
			uint32_t file_data_len = file_end < data_start ? 0 : file_end - data_start + 1;

			// if there's already something in data_len, increment until compatible
			if (get_file_data_len(ctx, reserved) != 0xFFFFFFFF)
				file_data_len = get_smallest_compatible(file_data_len, get_file_data_len(ctx, reserved));

			if (change_file_data_len(ctx, reserved, file_data_len)) {
				context->f_errno = LILOTAFS_EFLASH;
				return LILOTAFS_EFLASH;
			}
			if (change_file_status(context, reserved, LILOTAFS_STATUS_DELETED)) {
				context->f_errno = LILOTAFS_EFLASH;
				return LILOTAFS_EFLASH;
			}

			// context->fs_tail = lilotafs_align_up_32(data_start + file_data_len, LILOTAFS_DATA_ALIGN);
			context->fs_tail = lilotafs_align_up_32(data_start + file_data_len, LILOTAFS_HEADER_ALIGN);
			scan_result.reserved = UINT32_MAX;

			// if there is migrating: proceed to (scan_result.migrating && !scan_result.reserved)
			// which copies migrating to a new file at the tail if no committed file of the same name is found
		}
		// (b) crash while writing file name
		else {
			// (i) copy the contents of migrating directly to reserved file
			if (migrating != UINT32_MAX) {
				char mig_filename[64];
				READ_FILENAME(mig_filename, migrating);

				// finish writing file name
				uint32_t write_offset = res_offset + sizeof(struct lilotafs_rec_header);
				if (lilotafs_flash_write(context, mig_filename, write_offset, strnlen(mig_filename, 64) + 1)) {
					context->f_errno = LILOTAFS_EFLASH;
					return LILOTAFS_EFLASH;
				}

				// copy data from migrating to reserved
				write_offset += strnlen(mig_filename, 64) + 1;
				if (str_ends_with(mig_filename, LILOTAFS_KERNEL_EXT, 64))
					write_offset = lilotafs_align_up_32(write_offset, LILOTAFS_DATA_ALIGN_KERNEL);
				else
					write_offset = lilotafs_align_up_32(write_offset, LILOTAFS_DATA_ALIGN);

				uint32_t mig_data_offset = mig_offset + sizeof(struct lilotafs_rec_header);
				mig_data_offset += strnlen(mig_filename, 64) + 1;
				if (str_ends_with(mig_filename, LILOTAFS_KERNEL_EXT, 64))
					mig_data_offset = lilotafs_align_up_32(mig_data_offset, LILOTAFS_DATA_ALIGN_KERNEL);
				else
					mig_data_offset = lilotafs_align_up_32(mig_data_offset, LILOTAFS_DATA_ALIGN);

				uint32_t mig_data_len = get_file_data_len(ctx, migrating);
				void *mig_data = copy_file_mmap(ctx, mig_data_offset, mig_data_len);

				// check if there's space to write here, without using a wrap marker
				uint32_t wrap_marker_total = sizeof(struct lilotafs_rec_header);
				if (write_offset + get_file_data_len(ctx, migrating) + wrap_marker_total > lilotafs_flash_get_partition_size(ctx)) {
					// no space to write here, close the current file and append
					if (change_file_data_len(context, reserved, 0)) {
						copy_file_munmap(mig_data, mig_data_len);
						context->f_errno = LILOTAFS_EFLASH;
						return LILOTAFS_EFLASH;
					}
					if (change_file_status(context, reserved, LILOTAFS_STATUS_DELETED)) {
						copy_file_munmap(mig_data, mig_data_len);
						context->f_errno = LILOTAFS_EFLASH;
						return LILOTAFS_EFLASH;
					}

					context->fs_tail = lilotafs_align_up_32(write_offset, LILOTAFS_HEADER_ALIGN);

					uint32_t u32_max = UINT32_MAX;
					int code = append_file(
						ctx, mig_filename, &u32_max, mig_data, mig_data_len,
						LILOTAFS_STATUS_COMMITTED, false, false
					);
					copy_file_munmap(mig_data, mig_data_len);
					if (code != LILOTAFS_STATUS_WEAR_MARKER) {
						context->f_errno = code;
						return code;
					}
				}
				else {
					if (lilotafs_flash_write(context, mig_data, write_offset, mig_data_len)) {
						copy_file_munmap(mig_data, mig_data_len);
						context->f_errno = LILOTAFS_EFLASH;
						return LILOTAFS_EFLASH;
					}
					copy_file_munmap(mig_data, mig_data_len);

					// write data_len, commit and delete migrating
					if (change_file_data_len(context, reserved, get_file_data_len(ctx, migrating))) {
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
					context->fs_tail = lilotafs_align_up_32(write_offset + get_file_data_len(ctx, migrating), LILOTAFS_HEADER_ALIGN);
				}

				scan_result.migrating = UINT32_MAX;
				scan_result.reserved = UINT32_MAX;
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
				// do not need to check if kernel, since nothingw as written
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

				context->fs_tail = lilotafs_align_up_32(data_start + data_len, LILOTAFS_HEADER_ALIGN);
				scan_result.migrating = UINT32_MAX;
				scan_result.reserved = UINT32_MAX;
			}
		}
	}

	// found migrating, no reserved: there may or may not be a commited file of the same name
	// possible there is a committed file of the same name since we commit file before delete migrating
	// (migrating and reserved case (a) also goes here to copy migrating file)
	if (scan_result.migrating != UINT32_MAX && scan_result.reserved == UINT32_MAX) {
		PRINTF("crash recovery: migrating and not reserved\n");

		char filename[64];
		READ_FILENAME(filename, scan_result.migrating);

		uint32_t found_file = find_file_name(context, filename, false);

		// if we find a committed file of the same name, only need to delete migrating file
		// append file crashed right before setting the migrating file to deleted, complete this

		if (found_file == UINT32_MAX) {
			// there is no committed file of the same name
			// append a file with the same contents

			uint32_t migrating_offset = scan_result.migrating;
			uint32_t migrating_data_offset = migrating_offset + sizeof(struct lilotafs_rec_header);
			migrating_data_offset += strnlen(filename, 64) + 1;
			if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
				migrating_data_offset = lilotafs_align_up_32(migrating_data_offset, LILOTAFS_DATA_ALIGN_KERNEL);
			else
				migrating_data_offset = lilotafs_align_up_32(migrating_data_offset, LILOTAFS_DATA_ALIGN);

			uint32_t mig_data_len = get_file_data_len(ctx, scan_result.migrating);
			void *mig_data = copy_file_mmap(ctx, migrating_data_offset, mig_data_len);
			int code = append_file(
				context, filename, &migrating_offset, mig_data,
				mig_data_len, LILOTAFS_STATUS_COMMITTED, false, false
			);
			copy_file_munmap(mig_data, mig_data_len);
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
	if (wear_marker != UINT32_MAX) {
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

char *remove_prefix_slash(const char *filename) {
	uint32_t filename_len = strnlen(filename, 64);
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

char *get_file_directory(const char *file) {
	int last_slash = -1;

	for (int i = strnlen(file, 64) - 1; i >= 0; i--) {
		if (file[i] == '/') {
			last_slash = i;
			break;
		}
	}

	if (last_slash == -1)
		return NULL;

	// plus one for / character, plus one for null terminator
	char *file_dir = (char *) malloc(last_slash + 2);
	memcpy(file_dir, file, last_slash + 1);
	file_dir[last_slash + 1] = 0;

	return file_dir;
}

int lilotafs_open(void *ctx, const char *name, int flags, int mode) {
	(void) mode;
	
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	uint32_t filename_len_raw = strnlen(name, 64);
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
	
	char *actual_name = remove_prefix_slash(name);

	char *file_dir = get_file_directory(actual_name);
	if (file_dir != NULL) {
		uint32_t found_dir = find_file_name(ctx, file_dir, false);
		free(file_dir);
		if (found_dir == UINT32_MAX) {
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
		.filename_len = strnlen(actual_name, 64),
		.offset = 0,
		.flags = flags,
		.write_errno = LILOTAFS_SUCCESS,
	};

	if (flags == O_RDONLY) {
		uint32_t file_found = find_file_name(context, actual_name, true);
		if (file_found == UINT32_MAX) {
			if (actual_name != name)
				free(actual_name);
			context->f_errno = LILOTAFS_ENOENT;
			return -1;
		}
		fd.position = file_found;
	}
	else {
		uint32_t file_found = find_file_name(context, actual_name, false);
		if (file_found != UINT32_MAX) {
			// if file found, save the previous file's permission
			// reserve_file will set the old file to migrating, and reserve a new file
			// also set previous_position, which will be deleted on close
			fd.position = file_found;
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
	scan_headers(ctx, context->fs_head);

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

	uint32_t file_header = desc.position;

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

		uint32_t previous_offset = desc.previous_position;

		uint32_t previous_filename_offset = previous_offset + sizeof(struct lilotafs_rec_header);

		char filename[64];
		READ_FILENAME(filename, previous_filename_offset);

		context->fs_tail = desc.position + sizeof(struct lilotafs_rec_header) + desc.filename_len + 1;
		if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
			context->fs_tail = lilotafs_align_up_32(context->fs_tail, LILOTAFS_DATA_ALIGN_KERNEL);
		else
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

		uint32_t previous_data_offset = previous_filename_offset + strnlen(filename, 64) + 1;
		if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
			previous_data_offset = lilotafs_align_up_32(previous_data_offset, LILOTAFS_DATA_ALIGN_KERNEL);
		else
			previous_data_offset = lilotafs_align_up_32(previous_data_offset, LILOTAFS_DATA_ALIGN);

		uint32_t previous_data_len = get_file_data_len(ctx, previous_data_offset);
		void *previous_data = copy_file_mmap(ctx, previous_data_offset, previous_data_len);

		// hacky solution:
		// we pass in u32_max because that indicates we are "appending a new file"
		// this tells the free space guarantee to not check if the old file can be migrated
		// after writing the new file, because we do not care since we are moving the old file
		uint32_t u32_max = UINT32_MAX;
		int code = append_file(
			context, filename, &u32_max, previous_data,
			previous_data_len, LILOTAFS_STATUS_COMMITTED, false, false
		);
		copy_file_munmap(previous_data, previous_data_len);

		if (code != LILOTAFS_SUCCESS) {
			context->f_errno = code;
			return code;
		}

		if (change_file_status(ctx, previous_offset, LILOTAFS_STATUS_DELETED)) {
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
		uint32_t old_file = desc.previous_position;
		if (change_file_status(context, old_file, LILOTAFS_STATUS_DELETED)) {
			context->f_errno = LILOTAFS_EFLASH;
			return LILOTAFS_EFLASH;
		}
	}

	context->fs_tail = desc.position + sizeof(struct lilotafs_rec_header) + desc.filename_len + 1;
	if (file_is_kernel(ctx, desc.position))
		context->fs_tail = lilotafs_align_up_32(context->fs_tail, LILOTAFS_DATA_ALIGN_KERNEL);
	else
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

	uint32_t header = desc->position;

	char filename[64];
	READ_FILENAME(filename, header);

	// we need to check there is space for
	// the migrating file (if one exists), the current file, and the largest file twice
	// simulate the effects of writing the buffer/len to the file

	uint32_t file_data_len = desc->offset + len;

	int error_code = check_free_space(ctx, desc->position, desc->position, filename, file_data_len);
	if (error_code != LILOTAFS_SUCCESS) {
		context->f_errno = error_code;
		desc->write_errno = context->f_errno;
		return -1;
	}
	
	uint32_t data_start = desc->position + sizeof(struct lilotafs_rec_header);
	data_start += strnlen(filename, 64) + 1;
	if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
		data_start = lilotafs_align_up_32(data_start, LILOTAFS_DATA_ALIGN_KERNEL);
	else
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
		// uint8_t *old_data = (uint8_t *) context->flash_mmap + data_start;

		void *old_data = copy_file_mmap(ctx, data_start, desc->offset);

		// update data_start to the new location of the file, at offset 0
		data_start = sizeof(struct lilotafs_rec_header) + strnlen(filename, 64) + 1;
		if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
			data_start = lilotafs_align_up_32(data_start, LILOTAFS_DATA_ALIGN_KERNEL);
		else
			data_start = lilotafs_align_up_32(data_start, LILOTAFS_DATA_ALIGN);

		if (lilotafs_flash_write(ctx, old_data, data_start, desc->offset)) {
			copy_file_munmap(old_data, desc->offset);
			context->f_errno = LILOTAFS_EFLASH;
			desc->write_errno = context->f_errno;
			return -1;
		}
		copy_file_munmap(old_data, desc->offset);

		desc->position = 0;
	}
 
	// write buffer data starting from the offset stored in FD table
	if (lilotafs_flash_write(context, buffer, data_start + desc->offset, len)) {
		context->f_errno = LILOTAFS_EFLASH;
		desc->write_errno = context->f_errno;
		return -1;
	}

	uint32_t worst_file_size = sizeof(struct lilotafs_rec_header) + strnlen(filename, 64) + 1;
	worst_file_size += desc->offset + len;
	if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
		worst_file_size += LILOTAFS_DATA_ALIGN_KERNEL;
	else
		worst_file_size += LILOTAFS_DATA_ALIGN;
	worst_file_size += LILOTAFS_HEADER_ALIGN;

	if (worst_file_size > context->largest_worst_file_size)
		context->largest_worst_file_size = worst_file_size;

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

	context->f_errno = LILOTAFS_SUCCESS;
	return get_file_data_len(ctx, descriptor.position);
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

	char filename[64];
	READ_FILENAME(filename, desc->position);

	uint32_t data_offset = desc->position + sizeof(struct lilotafs_rec_header) + strnlen(filename, 64) + 1;
	if (str_ends_with(filename, LILOTAFS_KERNEL_EXT, 64))
		data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN_KERNEL);
	else
		data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);

	// uint8_t *data_start = (uint8_t *) context->flash_mmap + data_offset;
	// memcpy(buffer, data_start + desc->offset, len);

	lilotafs_flash_read(ctx, buffer, data_offset + desc->offset, len);
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
// 	if (get_file_data_len(ctx, header) == context->largest_filename_len) {
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
	uint32_t cur_file;
};

char *add_slash(const char *name) {
	int name_len = strnlen(name, 64);
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

	char *name_no_prefix = remove_prefix_slash(name);
	char *actual_name = add_slash(name_no_prefix);
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
	if (strnlen(name, 64) == 0)
		actual_name = (char *) name;
	else {
		char *name_no_prefix = remove_prefix_slash(name);
		actual_name = add_slash(name_no_prefix);
		if (name_no_prefix != actual_name && name_no_prefix != name)
			free(name_no_prefix);

		if (strncmp(actual_name, "./", 64) == 0) {
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
		int name_len = strnlen(name, 64);
		dir->name = (char *) malloc(name_len + 1);
		for (int i = 0; i < name_len; i++)
			dir->name[i] = name[i];
		dir->name[name_len] = 0;
	}
	else
		dir->name = actual_name;

	dir->cur_file = context->fs_head;

	if (lilotafs_flash_flush(ctx, 0, lilotafs_flash_get_partition_size(ctx))) {
		context->f_errno = LILOTAFS_EFLASH;
		return NULL;
	}

	return (DIR *) dir;
}

// check if a file is "directly under" a directory and should be returned by readdir
bool is_dir_child(const char *dir, const char *file) {
	// if dir is "" (length 0) then we are listing the root:
	//  - files do not have slash anywhere
	//  - directories only have one slash, which is the last character
	// fprintf(stderr, "check is %s child of %s?\n", file, dir);

	if (strnlen(dir, 64) == 0) {
		// exclude the last character, because might be / for directories
		// if returns -1, that means no / found before the last character
		// which means the file is under the root directory
		uint32_t cur_group = 0;
		for (uint32_t i = 0; i < strnlen(file, 64) - 1; i++) {
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
	char *file_dir = get_file_directory(file);
	if (file_dir == NULL)
		return false;

	// fprintf(stderr, "dir %s\n", file_dir);
	// fprintf(stderr, "%lu %lu\n", strnlen(file_dir, 64), strnlen(file, 64));

	int cmp = strncmp(dir, file_dir, 64);
	free(file_dir);

	// fprintf(stderr, "good? %u\n", cmp);
	return cmp == 0;
}

struct dirent *lilotafs_readdir(void *ctx, DIR *pdir) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	struct lilotafs_dir *dir = (struct lilotafs_dir *) pdir;

	char full_path[64];
	uint32_t cur_header = dir->cur_file;
	while (1) {
		// full_path = (char *) cur_header + sizeof(struct lilotafs_rec_header);
		READ_FILENAME(full_path, cur_header);

		if (get_file_status(ctx, cur_header) == LILOTAFS_STATUS_COMMITTED &&
			strnlen(full_path, 64) != 0 &&
			is_dir_child(dir->name, full_path) &&
			cur_header != dir->cur_file &&
			strncmp(full_path, dir->name, 64) != 0
		) {
			// printf("file %s ok\n", full_path);
			break;
		}

		uint32_t next_header = process_advance_header(context, cur_header, lilotafs_flash_get_partition_size(ctx));
		if (next_header == UINT32_MAX)
			return NULL;

		cur_header = next_header;
		if (get_file_magic(ctx, cur_header) != LILOTAFS_RECORD &&
			get_file_magic(ctx, cur_header) != LILOTAFS_START &&
			get_file_magic(ctx, cur_header) != LILOTAFS_START_CLEAN &&
			get_file_magic(ctx, cur_header) != LILOTAFS_WRAP_MARKER) {
			return NULL;
		}
	}

	int full_path_len = strnlen(full_path, 64);

	lilotafs_de.d_ino = cur_header;
	lilotafs_de.d_type = full_path[full_path_len - 1] == '/' ? DT_DIR : DT_REG;

	// clear the filename to all 0
	memset(&lilotafs_de.d_name, 0, sizeof(lilotafs_de.d_name));

	// the file we return must have the directory stripped
	int path_len = strnlen(dir->name, 64);
	int filename_len = full_path_len - path_len;
	memcpy(lilotafs_de.d_name, full_path + path_len, filename_len + 1);
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
	uint32_t cur_header = context->fs_head;

	if (get_file_magic(ctx, cur_header) != LILOTAFS_RECORD &&
		get_file_magic(ctx, cur_header) != LILOTAFS_START &&
		get_file_magic(ctx, cur_header) != LILOTAFS_START_CLEAN &&
		get_file_magic(ctx, cur_header) != LILOTAFS_WRAP_MARKER) {
		return count;
	}

	char filename[64];
	READ_FILENAME(filename, cur_header);
	if (get_file_status(ctx, cur_header) == LILOTAFS_STATUS_COMMITTED &&
			(get_file_magic(ctx, cur_header) != LILOTAFS_START || strnlen(filename, 64) != 0)) {

		count++;
	}

	while (1) {
		uint32_t next_header = process_advance_header(context, cur_header, partition_size);
		if (next_header == UINT32_MAX)
			break;
		cur_header = next_header;

		if (get_file_magic(ctx, cur_header) != LILOTAFS_RECORD &&
			get_file_magic(ctx, cur_header) != LILOTAFS_START &&
			get_file_magic(ctx, cur_header) != LILOTAFS_START_CLEAN &&
			get_file_magic(ctx, cur_header) != LILOTAFS_WRAP_MARKER) {

			return count;
		}

		READ_FILENAME(filename, cur_header);
		if (get_file_status(ctx, cur_header) == LILOTAFS_STATUS_COMMITTED && (get_file_magic(ctx, cur_header) != LILOTAFS_START || strnlen(filename, 64) != 0))
			count++;
	}

	return count;
}

int lilotafs_stat(void *ctx, const char *path, struct stat *st) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;

	if (st == NULL) {
		context->f_errno = LILOTAFS_EINVAL;
		return -1;
	}

	uint32_t filename_len_raw = strnlen(path, 64);
	if (filename_len_raw > LILOTAFS_MAX_FILENAME_LEN) {
		context->f_errno = LILOTAFS_EINVAL;
		return -1;
	}

	char *actual_name = remove_prefix_slash(path);

	uint32_t file_found = find_file_name(context, actual_name, true);
	if (file_found == UINT32_MAX) {
		if (actual_name != path)
			free(actual_name);
		context->f_errno = LILOTAFS_ENOENT;
		return -1;
	}

	memset(st, 0, sizeof(*st));
	st->st_ino = file_found;
	st->st_size = get_file_data_len(ctx, file_found);
	st->st_mode = path[filename_len_raw - 1] == '/' ? S_IFDIR : S_IFREG;

	context->f_errno = LILOTAFS_SUCCESS;
	return 0;
}

uint32_t lilotafs_get_largest_file_size(void *ctx) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	return context->largest_worst_file_size;
}
uint32_t lilotafs_get_head(void *ctx) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	return context->fs_head;
}
uint32_t lilotafs_get_tail(void *ctx) {
	struct lilotafs_context *context = (struct lilotafs_context *) ctx;
	return context->fs_tail;
}

