#include "lilotaFS.h"

#include <bits/pthreadtypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "flash.h"
#include "util.h"

#define FLASH_CAN_WRITE(want, current) (!((current) ^ ((want) | (current))))

uint32_t u32_min(uint32_t a, uint32_t b) {
	return a < b ? a : b;
}
uint32_t u32_max(uint32_t a, uint32_t b) {
	return a > b ? a : b;
}

struct lilotafs_rec_header *scan_for_header(struct lilotafs_context *ctx, uint32_t start, uint32_t partition_size) {
	start = lilotafs_align_up_32(start, LILOTAFS_HEADER_ALIGN);
	for (uint32_t i = start; i <= partition_size - sizeof(struct lilotafs_rec_header); i += LILOTAFS_HEADER_ALIGN) {
		struct lilotafs_rec_header *header = (struct lilotafs_rec_header *) (ctx->flash_mmap + i);
		if (header->magic == LILOTAFS_START || header->magic == LILOTAFS_START_CLEAN)
			return header;
	}
	return NULL;
}

uint32_t change_file_magic(struct lilotafs_context *ctx, struct lilotafs_rec_header *file_header, uint16_t magic) {
	uint32_t file_offset = (uint64_t) file_header - (uint64_t) ctx->flash_mmap;
	uint32_t magic_addr = file_offset + offsetof(struct lilotafs_rec_header, magic);

	int out = flash_write(ctx->flash_mmap, &magic, magic_addr, 2);
	if (out)
		return LILOTAFS_EFLASH;

	file_header->magic = magic;
	return 0;
}
uint32_t change_file_status(struct lilotafs_context *ctx, struct lilotafs_rec_header *file_header, uint8_t status) {
	uint32_t file_offset = (uint64_t) file_header - (uint64_t) ctx->flash_mmap;
	uint32_t status_addr = file_offset + offsetof(struct lilotafs_rec_header, status);

	int out = flash_write(ctx->flash_mmap, &status, status_addr, 1);
	if (out)
		return LILOTAFS_EFLASH;

	file_header->status = status;
	return 0;
}
uint32_t change_file_data_len(struct lilotafs_context *ctx, struct lilotafs_rec_header *file_header, uint32_t data_len) {
	uint32_t file_offset = (uint64_t) file_header - (uint64_t) ctx->flash_mmap;
	uint32_t data_len_addr = file_offset + offsetof(struct lilotafs_rec_header, data_len);

	int out = flash_write(ctx->flash_mmap, &data_len, data_len_addr, 4);
	if (out)
		return LILOTAFS_EFLASH;

	file_header->data_len = data_len;
	return 0;
}

bool magic_is_wrap_marker(struct lilotafs_rec_header *file) {
	// the magic number for the wrap marker is 0x5AFA
	if (file->magic == LILOTAFS_WRAP_MARKER)
		return true;
	if (*((uint8_t *) file + offsetof(struct lilotafs_rec_header, magic)) == 0xFA)
		return true;
	return false;
}


struct lilotafs_rec_header *process_advance_header(struct lilotafs_context *ctx, struct lilotafs_rec_header *cur_header,
											 uint32_t partition_size) {

	// if wrap header, back to start of partition
	// the wrap marker magic is FA FA, it's possible we crash between writing these bytes
	// resulting in FA FF, so we'll read the first byte
	// since the two bytes of FA FA are equal, this will work on little and big endian
	if (magic_is_wrap_marker(cur_header) || cur_header->status == LILOTAFS_STATUS_WRAP_MARKER) {
		// if we crash while writing the data_len field of the wrap marker
		if (cur_header->data_len != 0) {
			if (change_file_magic(ctx, cur_header, LILOTAFS_WRAP_MARKER))
				return NULL;
			if (change_file_status(ctx, cur_header, LILOTAFS_STATUS_WRAP_MARKER))
				return NULL;
			if (change_file_data_len(ctx, cur_header, 0))
				return NULL;
		}
		return (struct lilotafs_rec_header *) ctx->flash_mmap;
	}

	uint64_t current_offset = (uint64_t) cur_header - (uint64_t) ctx->flash_mmap;

	if (current_offset + sizeof(struct lilotafs_rec_header) >= partition_size)
		return NULL;

	// file name immediately after header
	char *filename = (char *) cur_header + sizeof(struct lilotafs_rec_header);
	// add 1 for null terminator
	uint32_t filename_len_padded = strlen(filename) + 1;

	// Malformed record - treat as end of valid data
	if (cur_header->data_len > partition_size - current_offset
			- sizeof(struct lilotafs_rec_header) - filename_len_padded) {

		return NULL;
	}

	uint32_t next_offset = current_offset + sizeof(struct lilotafs_rec_header) + filename_len_padded;
	next_offset = lilotafs_align_up_32(next_offset, LILOTAFS_DATA_ALIGN);
	next_offset += cur_header->data_len;
	next_offset = lilotafs_align_up_32(next_offset, LILOTAFS_HEADER_ALIGN);

	if (next_offset >= partition_size)
		return NULL;

	if (cur_header->status == LILOTAFS_STATUS_COMMITTED) {
		// TODO: save to hash table
		if (cur_header->data_len > ctx->largest_file_size) {
			ctx->largest_file_size = cur_header->data_len;
			ctx->largest_filename_len =  filename_len_padded - 1;
		}
	}

	return (struct lilotafs_rec_header *) (ctx->flash_mmap + next_offset);
}

struct scan_headers_result {
	struct lilotafs_rec_header *last_header, *wear_marker, *wrap_marker, *migrating, *reserved;
	uint32_t num_files;
} scan_headers(struct lilotafs_context *ctx, struct lilotafs_rec_header *start, uint32_t partition_size) {
	struct lilotafs_rec_header *cur_header = start;
	struct scan_headers_result ret = {
		.last_header = cur_header,
		.wear_marker = NULL,
		.wrap_marker = NULL,
		.migrating = NULL,
		.reserved = NULL,
		.num_files = 0
	};

	if (cur_header->magic != LILOTAFS_RECORD && cur_header->magic != LILOTAFS_START &&
			cur_header->magic != LILOTAFS_START_CLEAN && cur_header->magic != LILOTAFS_WRAP_MARKER) {

		return ret;
	}

	while (1) {
		// if mount creates a dummy header, do not count it as a file (it is empty)
		char *filename = (char *) cur_header + sizeof(struct lilotafs_rec_header);
		if (cur_header->status == LILOTAFS_STATUS_COMMITTED && (cur_header->magic != LILOTAFS_START || strlen(filename) != 0))
			ret.num_files++;

		if (cur_header->status == LILOTAFS_STATUS_WEAR_MARKER)
			ret.wear_marker = cur_header;
		if (magic_is_wrap_marker(cur_header) || cur_header->status == LILOTAFS_STATUS_WRAP_MARKER)
			ret.wrap_marker = cur_header;
		if (cur_header->status == LILOTAFS_STATUS_MIGRATING)
			ret.migrating = cur_header;

		// weird edge case: if the cur_header has status FF
		// becuse of a crash after writing magic but before writing reserved status
		// treat it like reserved
		if (cur_header->status == LILOTAFS_STATUS_RESERVED || cur_header->status == 0xFF) {
			ret.reserved = cur_header;
			break;
		}

		struct lilotafs_rec_header *next_header = process_advance_header(ctx, cur_header, partition_size);
		if (!next_header)
			break;

		cur_header = next_header;
		if (cur_header->magic != LILOTAFS_RECORD && cur_header->magic != LILOTAFS_START &&
				cur_header->magic != LILOTAFS_START_CLEAN && cur_header->magic != LILOTAFS_WRAP_MARKER) {

			break;
		}
	}
	ret.last_header = cur_header;
	return ret;
}

uint32_t check_free_space(struct lilotafs_context *ctx, uint32_t current_offset,
						  uint32_t write_offset, uint32_t filename_len, uint32_t len) {

	uint32_t partition_size = flash_get_total_size();

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
		struct lilotafs_rec_header *old_file = (struct lilotafs_rec_header *) (ctx->flash_mmap + current_offset);

		// the old and new file names should really be the same, but checking to be safe
		uint32_t old_filename_len = strlen((char *) old_file + sizeof(struct lilotafs_rec_header));

		old_file_total = sizeof(struct lilotafs_rec_header) + old_filename_len + 1;
		old_file_total = lilotafs_align_up_32(old_file_total, LILOTAFS_DATA_ALIGN);
		old_file_total += old_file->data_len;
		old_file_total = lilotafs_align_up_32(old_file_total, LILOTAFS_HEADER_ALIGN);
	}

	uint32_t new_file_total = sizeof(struct lilotafs_rec_header) + filename_len + 1;
	new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_DATA_ALIGN);
	new_file_total += len;
	new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_HEADER_ALIGN);

	uint32_t wrap_marker_total = sizeof(struct lilotafs_rec_header);
	wrap_marker_total = lilotafs_align_up_32(wrap_marker_total, LILOTAFS_HEADER_ALIGN);

	uint32_t largest_file_total = sizeof(struct lilotafs_rec_header) + ctx->largest_filename_len + 1;
	largest_file_total = lilotafs_align_up_32(largest_file_total, LILOTAFS_DATA_ALIGN);
	largest_file_total += ctx->largest_file_size;
	largest_file_total = lilotafs_align_up_32(largest_file_total, LILOTAFS_HEADER_ALIGN);
	// the current file we're writing might be larger than current largest
	largest_file_total = u32_max(largest_file_total, u32_max(new_file_total, old_file_total));

	// hypothetical tail pointer if we added this file
	// log_wrap indicates whether there is already a wear marker,
	// or if one is needed in case we add the current file, and copy
	// the largest file twiec
	bool log_wrap = ctx->fs_head >= ctx->fs_tail;
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

	// now we try to add the largerst file, twice
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
		uint32_t sector_size = flash_get_sector_size();
		if (log_wrap && current_position / sector_size == ctx->fs_head / sector_size)
			return LILOTAFS_ENOSPC;

		// if we wrapped the log / inserted a wear marker for any of the two files,
		// tail must be in front of head
		if (log_wrap && current_position >= ctx->fs_head)
			return LILOTAFS_ENOSPC;
	}

	// if no wrap, there is no problem since we still have free space
	// after tail and before the end of partition

	return LILOTAFS_SUCCESS;
}

// current_offset is updated with the new offset, provide pointer to UINT32_MAX if only creating
// updates fs_tail
uint32_t append_file(struct lilotafs_context *ctx, const char *filename, uint32_t *current_offset,
					 void *buffer, uint32_t len, uint8_t want_status, bool add_wear_marker) {

	uint32_t filename_len = strlen(filename);

	uint32_t partition_size = flash_get_total_size();

	if (check_free_space(ctx, *current_offset, ctx->fs_tail, filename_len, len))
		return LILOTAFS_ENOSPC;

	uint32_t new_file_total = sizeof(struct lilotafs_rec_header) + filename_len + 1;
	new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_DATA_ALIGN);
	new_file_total += len;
	new_file_total = lilotafs_align_up_32(new_file_total, LILOTAFS_HEADER_ALIGN);

	uint32_t wrap_marker_total = sizeof(struct lilotafs_rec_header);
	wrap_marker_total = lilotafs_align_up_32(wrap_marker_total, LILOTAFS_HEADER_ALIGN);

	// mark old file as migrating
	if (*current_offset != UINT32_MAX) {
		struct lilotafs_rec_header *old_header = (struct lilotafs_rec_header *) (ctx->flash_mmap + *current_offset);
		change_file_status(ctx, old_header, LILOTAFS_STATUS_MIGRATING);
	}

	uint32_t new_file_offset = ctx->fs_tail;
	// ensure we have space for a wrap marker afterwards
	if (new_file_total + wrap_marker_total > partition_size - ctx->fs_tail) {
		struct lilotafs_rec_header wrap_marker = {
			.magic = LILOTAFS_WRAP_MARKER,
			.status = LILOTAFS_STATUS_WRAP_MARKER,
			.data_len = 0
		};
		if (flash_write(ctx->flash_mmap, &wrap_marker, ctx->fs_tail, sizeof(struct lilotafs_rec_header)))
			return LILOTAFS_EFLASH;
		new_file_offset = 0;
	}

	// phase 1: reserve
	struct lilotafs_rec_header file_header = {
		.magic = LILOTAFS_RECORD,
		.status = LILOTAFS_STATUS_RESERVED,
		.data_len = len
	};
	if (flash_write(ctx->flash_mmap, &file_header, new_file_offset, sizeof(struct lilotafs_rec_header)))
		return LILOTAFS_EFLASH;

	// phase 2: write data
	uint32_t data_offset = new_file_offset + sizeof(struct lilotafs_rec_header) + filename_len + 1;
	data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);
	if (flash_write(ctx->flash_mmap, filename, new_file_offset + sizeof(struct lilotafs_rec_header), filename_len + 1))
		return LILOTAFS_EFLASH;
	if (len) {
		if (flash_write(ctx->flash_mmap, buffer, data_offset, len))
			return LILOTAFS_EFLASH;
	}

	// phase 3: commit
	if (change_file_status(ctx, (struct lilotafs_rec_header *) (ctx->flash_mmap + new_file_offset), want_status))
		return LILOTAFS_EFLASH;

	// invalidate old
	if (*current_offset != UINT32_MAX) {
		struct lilotafs_rec_header *old_header = (struct lilotafs_rec_header *) (ctx->flash_mmap + *current_offset);
		if (change_file_status(ctx, old_header, LILOTAFS_STATUS_DELETED))
			return LILOTAFS_EFLASH;
	}
	
	ctx->fs_tail = lilotafs_align_up_32(data_offset + len, LILOTAFS_HEADER_ALIGN);

	if (!ctx->has_wear_marker && *current_offset != UINT32_MAX && add_wear_marker) {
		char empty = 0;
		uint32_t current_offset = UINT32_MAX;
		ctx->has_wear_marker = true;
		uint32_t code = append_file(ctx, &empty, &current_offset, NULL, 0, LILOTAFS_STATUS_WEAR_MARKER, false);
		if (code != LILOTAFS_SUCCESS)
			return code;
	}

	*current_offset = new_file_offset;

	return LILOTAFS_SUCCESS;
}

uint32_t lilotafs_test_set_file(struct lilotafs_context *ctx, int fd) {
	ctx->flash_mmap = mmap(NULL, flash_get_total_size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ctx->flash_mmap == MAP_FAILED)
		return -1;
	return 0;
}

uint32_t remove_false_magic(struct lilotafs_context *ctx, uint8_t *start, uint32_t size) {
	for (uint8_t *p = (uint8_t *) lilotafs_align_up_64((uint64_t) start, LILOTAFS_HEADER_ALIGN); p < start + size; p += LILOTAFS_HEADER_ALIGN) {
		uint16_t magic = *((uint16_t *) p);
		// if data is any magic number, set it to 0 to avoid picking it up by mistake
		if (magic == LILOTAFS_RECORD || magic == LILOTAFS_START || magic == LILOTAFS_START_CLEAN) {
			uint32_t offset = (uint64_t) p - (uint64_t) ctx->flash_mmap;
			uint16_t zero = 0;
			if (flash_write(ctx->flash_mmap, &zero, offset, 2))
				return LILOTAFS_EFLASH;
		}
	}
	return LILOTAFS_SUCCESS;
}

uint32_t clobber_file_data(struct lilotafs_context *ctx, struct lilotafs_rec_header *file) {
	if (magic_is_wrap_marker(file) || file->status == LILOTAFS_STATUS_WRAP_MARKER)
		return LILOTAFS_SUCCESS;

	char *filename = (char *) file + sizeof(struct lilotafs_rec_header);
	uint32_t filename_offset = (uint64_t) filename - (uint64_t) ctx->flash_mmap;

	uint32_t data_offset = filename_offset + strlen(filename) + 1;
	data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);
	data_offset += file->data_len;

	return remove_false_magic(ctx, (uint8_t *) filename, data_offset - filename_offset);
}

uint32_t erase_file(struct lilotafs_context *ctx, uint32_t cur_offset, uint32_t next_offset) {
	uint32_t sector_size = flash_get_sector_size();

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
		if (remove_false_magic(ctx, ctx->flash_mmap + sector_start, next_offset % sector_size))
			return LILOTAFS_EFLASH;
	}

	// previous offset cannot be in the same sector as tail
	if (ctx->fs_tail / sector_size == cur_offset / sector_size)
		return LILOTAFS_ENOSPC;

	// erase old blocks, in reverse order (large address -> small address)
	for (uint32_t sector = next_offset / sector_size; sector >= cur_offset / sector_size + 1; sector--) {
		if (flash_erase_region(ctx->flash_mmap, (sector - 1) * sector_size, sector_size))
			return LILOTAFS_EFLASH;
	}

	return LILOTAFS_SUCCESS;
}

// this function has caused sooo many problems
uint32_t wear_level_compact(struct lilotafs_context *ctx, struct lilotafs_rec_header *wear_marker, uint32_t num_files) {
	uint32_t partition_size = flash_get_total_size();
	uint32_t sector_size = flash_get_sector_size();

	// delete wear marker
	if (change_file_status(ctx, wear_marker, LILOTAFS_STATUS_DELETED))
		return LILOTAFS_EFLASH;

	// this is the number of files we will copy to the tail
	// we set a limit defined by the macro, but also do not move more than the number of files
	// this is to avoid moving the same more than onec
	uint32_t count = u32_min(LILOTAFS_WEAR_LEVEL_MAX_RECORDS, num_files);

	struct lilotafs_rec_header *cur_header = (struct lilotafs_rec_header *) (ctx->flash_mmap + ctx->fs_head);

	// this should be impossible
	// if (cur_header->magic != LILOTAFS_RECORD && cur_header->magic != LILOTAFS_START && cur_header->magic != LILOTAFS_START_CLEAN) {
	// 	PRINTF("??????????\n");
	// 	return LILOTAFS_EUNKNOWN;
	// }

	while (1) {
		struct lilotafs_rec_header *next_header = process_advance_header(ctx, cur_header, partition_size);

		if (!next_header)
			break;

		if (next_header->magic != LILOTAFS_RECORD && next_header->magic != LILOTAFS_START &&
				next_header->magic != LILOTAFS_START_CLEAN && next_header->magic != LILOTAFS_WRAP_MARKER) {

			break;
		}

		uint32_t cur_offset = (uint64_t) cur_header - (uint64_t) ctx->flash_mmap;
		uint32_t next_offset = (uint64_t) next_header - (uint64_t) ctx->flash_mmap;

		// move the current file to the tail
		uint32_t cur_offset_after_move = cur_offset;

		// NOTE: if we mount the filesystem and do not find a LILOTAFS_START record
		// we create a record at offset 0 with magic LILOTAFS_START and length 0
		// we do NOT want to move that record
		if (!(cur_header->data_len == 0 && cur_offset == 0) && cur_header->status == LILOTAFS_STATUS_COMMITTED) {
			count--;

			char *filename = (char *) cur_header + sizeof(struct lilotafs_rec_header);

			uint32_t data_offset = cur_offset + sizeof(struct lilotafs_rec_header) + strlen(filename) + 1;
			data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);
			uint8_t *data_start = (uint8_t *) ctx->flash_mmap + data_offset;

			uint32_t code = append_file(ctx, filename, &cur_offset_after_move, data_start,
										cur_header->data_len, LILOTAFS_STATUS_COMMITTED, false);
			if (code != LILOTAFS_SUCCESS)
				return code;
		}

		// we need to be deliberate about the order to make modifications in case of a crash
		// first, set the current file to LILOTAFS_START_CLEAN, indicating this file is the old LILOTAFS_START
		if (change_file_magic(ctx, next_header, LILOTAFS_START_CLEAN))
			return LILOTAFS_EFLASH;

		// we cannot have any magic numbers in 32 byte boundaries, or they will be detected as files
		// we want to clobber the filename too, so data start is the file name
		uint32_t code = clobber_file_data(ctx, cur_header);
		if (code != LILOTAFS_SUCCESS)
			return code;

		// if the file crosses a flash boundary, or we are wrapping
		if (next_offset / sector_size != cur_offset / sector_size) {
			// if next_offset is 0, then we are processing a wrap marker
			// the wrap marker does not actually cross the flash erase boundar
			// we can simply erase all of the last sector
			code = erase_file(ctx, cur_offset, next_offset == 0 ? flash_get_total_size() : next_offset);
			if (code != LILOTAFS_SUCCESS)
				return code;
		}

		// NOTE: if we crash during wear leveling, there are two failure cases:
		// If here or earlier, then there is an LILOTAFS_START_CLEAN (next)
		// and maybe an LILOTAFS_START, depending on whether the current file's header is erased

		// done with processing the current file
		// now advance LILOTAFS_START and delete the just-moved file
		if (change_file_magic(ctx, next_header, LILOTAFS_START))
			return LILOTAFS_EFLASH;

		// NOTE: if we crash here, there is one or two LILOTAFS_START's
		// next is definitely LILOTAFS_START, and current may have been erased

		// do not set magic to 0 if the sector has been erased
		if (next_offset / sector_size == cur_offset / sector_size) {
			if (change_file_magic(ctx, cur_header, 0))
				return LILOTAFS_EFLASH;
		}

		ctx->fs_head = next_offset;
		cur_header = next_header;

		if (count == 0)
			break;
	}

	return LILOTAFS_SUCCESS;
}

struct lilotafs_rec_header *find_file_name(struct lilotafs_context *ctx, const char *name) {
	uint32_t filename_len = strlen(name);
	uint32_t partition_size = flash_get_total_size();

	struct lilotafs_rec_header *cur_header = (struct lilotafs_rec_header *) (ctx->flash_mmap + ctx->fs_head);
	while (1) {
		if (cur_header->status == LILOTAFS_STATUS_COMMITTED) {
			char *cur_filename = (char *) cur_header + sizeof(struct lilotafs_rec_header);
			if (strlen(cur_filename) == filename_len && strncmp(cur_filename, name, filename_len) == 0)
				return cur_header;
		}

		struct lilotafs_rec_header *next_header = process_advance_header(ctx, cur_header, partition_size);
		if (!next_header)
			break;
		cur_header = next_header;
		if (cur_header->magic != LILOTAFS_RECORD && cur_header->magic != LILOTAFS_WRAP_MARKER)
			break;
	}
	return NULL;
}

// simulate graceful computer shutdown
uint32_t lilotafs_unmount(struct lilotafs_context *ctx) {
	ctx->largest_file_size = 0, ctx->largest_filename_len = 0;
	ctx->fs_head = 0, ctx->fs_tail = 0;
	ctx->has_wear_marker = false;

	// close all open FDs
	if (ctx->fd_list_capacity != 0 && ctx->fd_list != NULL)
		free(ctx->fd_list);
	ctx->fd_list_size = 0, ctx->fd_list_capacity = 0;

	munmap(ctx->flash_mmap, flash_get_total_size());

	return LILOTAFS_SUCCESS;
}


uint32_t lilotafs_mount(struct lilotafs_context *ctx) {
	ctx->largest_file_size = 0, ctx->largest_filename_len = 0;
	ctx->fs_head = 0, ctx->fs_tail = 0;
	ctx->has_wear_marker = false;

	ctx->vfs_open_error = 0;

	uint32_t partition_size = flash_get_total_size();

	struct lilotafs_rec_header *cur_header = (struct lilotafs_rec_header *) ctx->flash_mmap;
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
	if (cur_header->magic != LILOTAFS_START) {
		// follow current file until no more
		// (if no file at 0 this will return cur_header immediately, then offset = 0)
		struct scan_headers_result scan_result = scan_headers(ctx, cur_header, partition_size);
		cur_header = scan_result.last_header;
		if (scan_result.wear_marker)
			wear_marker = scan_result.wear_marker;

		// scan_headers will return either the space directly after the last file,
		// or the header of the last file in case the last file is a reserved file
		// this is because we do not know when we crashed previously when writing it
		// so we cannot accurately determine when the reserved file ends
		//
		// if we crash while or before writing the filename, then either data_len is bad
		// or the filename is not terminated -- so we know no actual data has been written
		// since the max file length is 64, we can assume that is where free space starts
		//
		// if we crash afterwards, then the filename is intact and null terminated
		// this guarantees that data_len was properly written, and can trust it
		//
		// (we deal with this crash again later on)

		uint32_t offset = (uint64_t) cur_header - (uint64_t) ctx->flash_mmap;
		if (cur_header == scan_result.reserved && (cur_header->status == LILOTAFS_STATUS_RESERVED || cur_header->status == 0xFF)) {
			uint32_t reserved_filename_len = 0;
			char *reserved_filename = (char *) scan_result.reserved + sizeof(struct lilotafs_rec_header);
			for (uint32_t i = 0; i < 64; i++) {
				if (reserved_filename[i] == 0)
					break;
				reserved_filename_len++;
			}

			if (reserved_filename_len != 64) {
				offset += sizeof(struct lilotafs_rec_header) + reserved_filename_len + 1;
				offset = lilotafs_align_up_32(offset, LILOTAFS_DATA_ALIGN);
				offset += cur_header->data_len;
				offset = lilotafs_align_up_32(offset, LILOTAFS_HEADER_ALIGN);
			}
			else {
				offset += sizeof(struct lilotafs_rec_header) + 64;
				offset = lilotafs_align_up_32(offset, LILOTAFS_HEADER_ALIGN);
			}
		}

		// scan disk 1 byte at a time until we find LILOTAFS_START magic
		cur_header = scan_for_header(ctx, offset, partition_size);

		// if cur_header is NULL: no LILOTAFS_START
		// if offset 0 is LILOTAFS_START_CLEAN, then this is the result of the crash described earlier
		// if not, we need to write a LILOTAFS_START
		// (merely setting cur_header to 0 is enough, as this case is handled later)
		if (!cur_header && ((struct lilotafs_rec_header *) ctx->flash_mmap)->magic == LILOTAFS_START_CLEAN)
			cur_header = (struct lilotafs_rec_header *) ctx->flash_mmap;
	}

	// if none found, write a LILOTAFS_START header at 0
	if (!cur_header) {
		struct lilotafs_rec_header new_header = {
			.magic = LILOTAFS_START,
			.status = LILOTAFS_STATUS_COMMITTED,
			.data_len = 0
		};

		char empty = 0;
		// write record and empty filename
		if (flash_write(ctx->flash_mmap, &new_header, 0, sizeof(struct lilotafs_rec_header)))
			return LILOTAFS_EFLASH;
		if (flash_write(ctx->flash_mmap, &empty, sizeof(struct lilotafs_rec_header), 1))
			return LILOTAFS_EFLASH;

		ctx->fs_head = 0;
		ctx->fs_tail = sizeof(struct lilotafs_rec_header) + 1;
		ctx->fs_tail = lilotafs_align_up_32(ctx->fs_tail, LILOTAFS_HEADER_ALIGN);

		PRINTF("head = 0x%lx\n", (uint64_t) ctx->fs_head);
		PRINTF("tail = 0x%lx\n", (uint64_t) ctx->fs_tail);

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
	if (cur_header->magic == LILOTAFS_START_CLEAN) {
		if (change_file_magic(ctx, cur_header, LILOTAFS_START))
			return LILOTAFS_EFLASH;
	}
	else {
		struct lilotafs_rec_header *next_header = process_advance_header(ctx, cur_header, partition_size);
		if (next_header) {
			// 2. crash after the file has been cleaned up, simply delete the old file
			if (next_header->magic == LILOTAFS_START) {
				if (change_file_magic(ctx, cur_header, 0))
					return LILOTAFS_EFLASH;
				cur_header = next_header;
			}
			// 3. complicated
			if (next_header->magic == LILOTAFS_START_CLEAN) {
				uint32_t cur_offset = (uint64_t) cur_header - (uint64_t) ctx->flash_mmap;
				uint32_t next_offset = (uint64_t) next_header - (uint64_t) ctx->flash_mmap;

				// we cannot have any magic numbers in 32 byte boundaries, or they will be detected as files
				uint32_t code = clobber_file_data(ctx, cur_header);
				if (code != LILOTAFS_SUCCESS)
					return code;

				// if the file crosses a flash boundary, or we are wrapping
				uint32_t sector_size = flash_get_sector_size();
				if (next_offset / sector_size != cur_offset / sector_size) {
					// if next_offset is 0, then we are processing a wrap marker
					// the wrap marker does not actually cross the flash erase boundar
					// we can simply erase all of the last sector
					code = erase_file(ctx, cur_offset, next_offset == 0 ? flash_get_total_size() : next_offset);
					if (code != LILOTAFS_SUCCESS)
						return code;
				}

				// done with processing the current file
				// now advance LILOTAFS_START and delete the just-moved file
				if (change_file_magic(ctx, next_header, LILOTAFS_START))
					return LILOTAFS_EFLASH;

				// do not set magic to 0 if the sector has been erased
				if (next_offset / sector_size == cur_offset / sector_size) {
					if (change_file_magic(ctx, cur_header, 0))
						return LILOTAFS_EFLASH;
				}

				cur_header = next_header;
			}
		}
	}

	ctx->fs_head = (uint64_t) cur_header - (uint64_t) ctx->flash_mmap;

	// scan until no more files -- set that to tail pointer
	struct scan_headers_result scan_result = scan_headers(ctx, cur_header, partition_size);
	cur_header = scan_result.last_header;
	num_files += scan_result.num_files;
	if (scan_result.wear_marker)
		wear_marker = scan_result.wear_marker;

	// this is not actually the tail! scan_headers ends on a reserved file
	// because we do not know how much of that file was successfully written
	ctx->fs_tail = (uint64_t) cur_header - (uint64_t) ctx->flash_mmap;

	// TODO: consider whether to wear level before recovery
	// in case we need the extra space to append a migrating file

	// found migrating, found reserved
	if (scan_result.migrating && scan_result.reserved) {
		// make the old file the active/committed file
		// we can do this by copying the content of the migrating file to reserved
		// but we don't know if we can (cannot change bits from 0 to 1)

		struct lilotafs_rec_header *migrating = scan_result.migrating;
		struct lilotafs_rec_header *reserved = scan_result.reserved;

		uint32_t reserved_offset = (uint64_t) reserved - (uint64_t) ctx->flash_mmap;
		char *migrating_filename = (char *) migrating + sizeof(struct lilotafs_rec_header);

		uint32_t reserved_filename_len = 0;
		char *reserved_filename = (char *) reserved + sizeof(struct lilotafs_rec_header);
		for (uint32_t i = 0; i < 64; i++) {
			if (reserved_filename[i] == 0)
				break;
			reserved_filename_len++;
		}

		bool can_write;
		uint32_t total_size = 0;
		uint8_t *migrating_ptr = (uint8_t *) migrating_filename;
		uint8_t *reserved_ptr = (uint8_t *) reserved + sizeof(struct lilotafs_rec_header);

		// consider this case:
		//  1. migrating && reserved
		//  2. reserved data_len and filename are written, crash in the middle of writing file data
		//  3. the actual length of reserved is a lot more than migrating
		// observe that we do not want to write directly ("can_write = true") in this case
		// specifically, when reserved.data_len is written and is greater than migrating.data_len
		// even if we COULD write all of migrating in, we do not know about whatever data comes after
		// and we may not be able to put the next file there

		if (reserved_filename_len != 64 && reserved->data_len > migrating->data_len)
			can_write = false;
		else {
			can_write = FLASH_CAN_WRITE(migrating->data_len, reserved->data_len);

			// somewhat complicated, since we need to account for the fact that data
			// must be on xx-byte aligned boundaries (probably 16)
			// takes advantage of the fact that header align is a multiple of 16

			total_size = sizeof(struct lilotafs_rec_header) + strlen(migrating_filename) + 1;
			total_size = lilotafs_align_up_32(total_size, LILOTAFS_DATA_ALIGN);
			total_size = total_size + migrating->data_len - sizeof(struct lilotafs_rec_header);

			for (uint32_t i = 0; i < total_size && can_write; i++) {
				if (!FLASH_CAN_WRITE(migrating_ptr[i], reserved_ptr[i]))
					can_write = false;
			}
		}

		if (can_write) {
			if (change_file_data_len(ctx, reserved, migrating->data_len))
				return LILOTAFS_EFLASH;
			if (flash_write(ctx->flash_mmap, migrating_ptr, reserved_offset + sizeof(struct lilotafs_rec_header), total_size))
				return LILOTAFS_EFLASH;
			if (change_file_status(ctx, reserved, LILOTAFS_STATUS_COMMITTED))
				return LILOTAFS_EFLASH;
			if (change_file_status(ctx, migrating, LILOTAFS_STATUS_DELETED))
				return LILOTAFS_EFLASH;
		}
		else {
			// two possibilities: 1. crash while or before writing the filename, or 2. after
			// if 1, we can terminate filename since we know what length it is from migrating
			// in this case, the data len may also not have been properly written yet
			// which is fine because we will set data_len to 0 anyway (since no data was written at all)
			// if 2, we do not know how much of the content was written and where free space starts
			// so we will leave the data_len as is (the data_len must have been properly written)
			// and move fs_tail forward by the size of the file
			//
			// we can detect which case by checking if there is a null terminator \0 in the first
			// 64 bytes after the header

			if (cur_header == reserved) {
				// if strlen is 64 then there is never broken out of loop <=> no terminator
				if (reserved_filename_len != 64) {
					// case 2
					ctx->fs_tail += sizeof(struct lilotafs_rec_header) + strlen(reserved_filename) + 1;
					ctx->fs_tail = lilotafs_align_up_32(ctx->fs_tail, LILOTAFS_DATA_ALIGN);
					ctx->fs_tail += reserved->data_len;
					ctx->fs_tail = lilotafs_align_up_32(ctx->fs_tail, LILOTAFS_HEADER_ALIGN);
				}
				else {
					// case 1
					uint8_t zero = 0;
					uint32_t terminate_position = ctx->fs_tail + sizeof(struct lilotafs_rec_header);
					terminate_position += strlen(migrating_filename);

					// note we are writing the data len first!
					// so that if we crash between these two writes, we waste less space
					if (change_file_data_len(ctx, cur_header, 0))
						return LILOTAFS_EFLASH;
					if (flash_write(ctx->flash_mmap, &zero, terminate_position, 1))
						return LILOTAFS_EFLASH;

					ctx->fs_tail = lilotafs_align_up_32(terminate_position + 1, LILOTAFS_HEADER_ALIGN);
				}
			}

			if (change_file_status(ctx, reserved, LILOTAFS_STATUS_DELETED))
				return LILOTAFS_EFLASH;

			uint32_t migrating_offset = (uint64_t) migrating - (uint64_t) ctx->flash_mmap;
			uint32_t migrating_data_offset = migrating_offset + sizeof(struct lilotafs_rec_header);
			migrating_data_offset += strlen(migrating_filename) + 1;
			migrating_data_offset = lilotafs_align_up_32(migrating_data_offset, LILOTAFS_DATA_ALIGN);
			
			uint32_t code = append_file(
				ctx, migrating_filename, &migrating_offset, ctx->flash_mmap + migrating_data_offset,
				migrating->data_len, LILOTAFS_STATUS_COMMITTED, false
			);

			if (code != LILOTAFS_SUCCESS)
				return code;
		}

		// check everything again
		struct lilotafs_rec_header *head_record = (struct lilotafs_rec_header *) (ctx->flash_mmap + ctx->fs_head);
		struct scan_headers_result scan_result = scan_headers(ctx, head_record, partition_size);
		ctx->fs_tail = (uint64_t) scan_result.last_header - (uint64_t) ctx->flash_mmap;
		num_files = scan_result.num_files;
		if (scan_result.wear_marker)
			wear_marker = scan_result.wear_marker;
	}

	// found migrating, no reserved: there may or may not be a commited file of the same name
	// possible there is a committed file of the same name since we commit file before delete migrating
	else if (scan_result.migrating && !scan_result.reserved) {
		char *filename = (char *) scan_result.migrating + sizeof(struct lilotafs_rec_header);

		struct lilotafs_rec_header *found_file = find_file_name(ctx, filename);

		// if we find a committed file of the same name, there is no need to do anything
		// append file crashed right before setting the migrating file to deleted, complete this

		if (!found_file) {
			// there is no committed file of the same name
			// append a file with the same contents

			// uint32_t migrating_offset = (uint64_t) scan_result.migrating - (uint64_t) flash_mmap;

			uint32_t migrating_offset = (uint64_t) scan_result.migrating - (uint64_t) ctx->flash_mmap;
			uint32_t migrating_data_offset = migrating_offset + sizeof(struct lilotafs_rec_header);
			migrating_data_offset += strlen(filename) + 1;
			migrating_data_offset = lilotafs_align_up_32(migrating_data_offset, LILOTAFS_DATA_ALIGN);

			uint32_t code = append_file(
				ctx, filename, &migrating_offset, ctx->flash_mmap + migrating_data_offset,
				scan_result.migrating->data_len, LILOTAFS_STATUS_COMMITTED, false
			);

			if (code != LILOTAFS_SUCCESS)
				return code;
		}

		if (change_file_status(ctx, scan_result.migrating, LILOTAFS_STATUS_DELETED))
			return LILOTAFS_EFLASH;
	}

	// crash in the middle adding a new file (not overwriting/modifying existing file)
	// or adding wear marker (happens after the main file is committed)
	// it should be the "tail"/last header, since files are appended there
	else if (!scan_result.migrating && scan_result.reserved && scan_result.reserved == cur_header) {
		// as with some other cases: handling depends on where specifically we crash
		// and we can crash while or before writing the filename, or while writing data
		struct lilotafs_rec_header *reserved = scan_result.reserved;
		uint32_t reserved_filename_len = 0;
		char *reserved_filename = (char *) reserved + sizeof(struct lilotafs_rec_header);
		for (uint32_t i = 0; i < 64; i++) {
			if (reserved_filename[i] == 0)
				break;
			reserved_filename_len++;
		}

		// full filename with null terminator => data_len is completely written
		if (reserved_filename_len != 64) {
			if (change_file_status(ctx, reserved, LILOTAFS_STATUS_DELETED))
				return LILOTAFS_EFLASH;
			ctx->fs_tail += sizeof(struct lilotafs_rec_header) + reserved_filename_len + 1;
			ctx->fs_tail = lilotafs_align_up_32(ctx->fs_tail, LILOTAFS_DATA_ALIGN);
			ctx->fs_tail += reserved->data_len;
			ctx->fs_tail = lilotafs_align_up_32(ctx->fs_tail, LILOTAFS_HEADER_ALIGN);
		}
		else {
			uint8_t zero = 0;
			uint32_t terminate_position = (uint64_t) scan_result.reserved - (uint64_t) ctx->flash_mmap;
			// pretend the file name was supposed to be 63, which is the longest allowed
			terminate_position += sizeof(struct lilotafs_rec_header) + 63;

			if (change_file_data_len(ctx, cur_header, 0))
				return LILOTAFS_EFLASH;
			if (flash_write(ctx->flash_mmap, &zero, terminate_position, 1))
				return LILOTAFS_EFLASH;
			if (change_file_status(ctx, reserved, LILOTAFS_STATUS_DELETED))
				return LILOTAFS_EFLASH;

			ctx->fs_tail = lilotafs_align_up_32(terminate_position + 1, LILOTAFS_HEADER_ALIGN);
		}
	}

	PRINTF("head = 0x%lx\n", (uint64_t) ctx->fs_head);
	PRINTF("tail = 0x%lx\n", (uint64_t) ctx->fs_tail);

	// if we find wear marker, need to perform wear leveling
	if (wear_marker) {
		uint32_t code = wear_level_compact(ctx, wear_marker, num_files);
		if (code != LILOTAFS_SUCCESS)
			return code;

		PRINTF("new head = 0x%lx\n", (uint64_t) ctx->fs_head);
		PRINTF("new tail = 0x%lx\n", (uint64_t) ctx->fs_tail);
	}

	return LILOTAFS_SUCCESS;
}






uint32_t fd_list_append(struct lilotafs_context *ctx, struct lilotafs_file_descriptor fd) {
	if (ctx->fd_list_size < ctx->fd_list_capacity) {
		ctx->fd_list[ctx->fd_list_size++] = fd;
		return 0;
	}

	if (ctx->fd_list_capacity == 0) {
		ctx->fd_list_capacity = 1;
		ctx->fd_list = (struct lilotafs_file_descriptor *) malloc(sizeof(struct lilotafs_file_descriptor));
	}
	else {
		ctx->fd_list_capacity *= 2;
		ctx->fd_list = (struct lilotafs_file_descriptor *) realloc(
			ctx->fd_list,
			ctx->fd_list_capacity * sizeof(struct lilotafs_file_descriptor)
		);
	}
	if (!ctx->fd_list)
		return 1;

	ctx->fd_list[ctx->fd_list_size++] = fd;
	return 0;
}

// returns index in list, or UINT32_MAX on failure
uint32_t fd_list_add(struct lilotafs_context *ctx, struct lilotafs_file_descriptor fd) {
	for (uint32_t i = 0; i < ctx->fd_list_size; i++) {
		if (!ctx->fd_list[i].in_use) {
			ctx->fd_list[i] = fd;
			return i;
		}
	}
	if (fd_list_append(ctx, fd))
		return UINT32_MAX;
	return ctx->fd_list_size - 1;
}

uint32_t lilotafs_open_errno(struct lilotafs_context *ctx) {
	return ctx->vfs_open_error;
}

uint32_t lilotafs_open(struct lilotafs_context *ctx, const char *name, int flags) {
	uint32_t filename_len = strlen(name);
	if (filename_len > LILOTAFS_MAX_FILENAME_LEN) {
		ctx->vfs_open_error = LILOTAFS_EINVAL;
		return -1;
	}

	struct lilotafs_rec_header *file_found = find_file_name(ctx, name);

	struct lilotafs_file_descriptor fd = {
		.in_use = true,
		.offset = UINT32_MAX,
	};

	if (!file_found) {
		if ((flags & LILOTAFS_CREATE) != LILOTAFS_CREATE) {
			ctx->vfs_open_error = LILOTAFS_ENOENT;
			return -1;
		}
		uint32_t code = append_file(ctx, name, &fd.offset, NULL, 0, LILOTAFS_STATUS_COMMITTED, true);
		if (code != LILOTAFS_SUCCESS) {
			ctx->vfs_open_error = code;
			return -1;
		}
		file_found = (struct lilotafs_rec_header *) (ctx->flash_mmap + fd.offset);
	}

	fd.offset = (uint64_t) file_found - (uint64_t) ctx->flash_mmap;

	uint32_t fd_index = fd_list_add(ctx, fd);
	if (fd_index == UINT32_MAX) {
		ctx->vfs_open_error = LILOTAFS_EUNKNOWN;
		return -1;
	}

	return fd_index + 1;
}

uint32_t lilotafs_close(struct lilotafs_context *ctx, uint32_t fd) {
	if (fd - 1 >= ctx->fd_list_size)
		return LILOTAFS_EBADF;

	fd--;
	if (fd >= ctx->fd_list_size)
		return -1;

	ctx->fd_list[fd].in_use = false;
	ctx->fd_list[fd].offset = 0;

	return 0;
}

uint32_t lilotafs_write(struct lilotafs_context *ctx, uint32_t fd, void *buffer, uint32_t len) {
	if (fd - 1 >= ctx->fd_list_size)
		return LILOTAFS_EBADF;

	struct lilotafs_file_descriptor *descriptor = &ctx->fd_list[fd - 1];
	struct lilotafs_rec_header *header = (struct lilotafs_rec_header *) (ctx->flash_mmap + descriptor->offset);
	char *filename = (char *) header + sizeof(struct lilotafs_rec_header);

	uint32_t code = append_file(ctx, filename, &descriptor->offset, buffer, len, LILOTAFS_STATUS_COMMITTED, true);
	if (code != LILOTAFS_SUCCESS)
		return code;

	if (header->data_len == ctx->largest_file_size && len < header->data_len) {
		ctx->largest_file_size = 0, ctx->largest_filename_len = 0;
		struct lilotafs_rec_header *head_header = (struct lilotafs_rec_header *) (ctx->flash_mmap + ctx->fs_head);
		scan_headers(ctx, head_header, flash_get_total_size());
	}
	else if (len >= ctx->largest_file_size) {
		ctx->largest_file_size = len;
		ctx->largest_filename_len = strlen(filename);
	}

	return LILOTAFS_SUCCESS;
}

uint32_t lilotafs_get_size(struct lilotafs_context *ctx, uint32_t fd) {
	if (fd - 1 >= ctx->fd_list_size)
		return LILOTAFS_EBADF;

	struct lilotafs_file_descriptor descriptor = ctx->fd_list[fd - 1];
	struct lilotafs_rec_header *header = (struct lilotafs_rec_header *) (ctx->flash_mmap + descriptor.offset);
	return header->data_len;
}

uint32_t lilotafs_read(struct lilotafs_context *ctx, uint32_t fd, void *buffer, uint32_t addr, uint32_t len) {
	if (fd - 1 >= ctx->fd_list_size)
		return LILOTAFS_EBADF;

	struct lilotafs_file_descriptor descriptor = ctx->fd_list[fd - 1];

	char *filename = (char *) ctx->flash_mmap + descriptor.offset + sizeof(struct lilotafs_rec_header);
	uint32_t data_offset = descriptor.offset + sizeof(struct lilotafs_rec_header) + strlen(filename) + 1;
	data_offset = lilotafs_align_up_32(data_offset, LILOTAFS_DATA_ALIGN);
	uint8_t *data_start = (uint8_t *) ctx->flash_mmap + data_offset;

	memcpy(buffer, data_start + addr, len);

	return LILOTAFS_SUCCESS;
}

uint32_t lilotafs_delete(struct lilotafs_context *ctx, uint32_t fd) {
	if (fd - 1 >= ctx->fd_list_size)
		return LILOTAFS_EBADF;

	struct lilotafs_file_descriptor descriptor = ctx->fd_list[fd - 1];
	struct lilotafs_rec_header *header = (struct lilotafs_rec_header *) (ctx->flash_mmap + descriptor.offset);
	if (change_file_status(ctx, header, LILOTAFS_STATUS_DELETED))
		return LILOTAFS_EFLASH;

	if (header->data_len == ctx->largest_filename_len) {
		ctx->largest_file_size = 0, ctx->largest_filename_len = 0;
		struct lilotafs_rec_header *head_header = (struct lilotafs_rec_header *) (ctx->flash_mmap + ctx->fs_head);
		scan_headers(ctx, head_header, flash_get_total_size());
	}

	if (!ctx->has_wear_marker) {
		char empty = 0;
		uint32_t current_offset = UINT32_MAX;
		ctx->has_wear_marker = true;
		return append_file(ctx, &empty, &current_offset, NULL, 0, LILOTAFS_STATUS_WEAR_MARKER, false);
	}

	return LILOTAFS_SUCCESS;
}


uint32_t lilotafs_count_files(struct lilotafs_context *ctx) {
	uint32_t count = 0;
	uint32_t partition_size = flash_get_total_size();
	struct lilotafs_rec_header *cur_header = (struct lilotafs_rec_header *) (ctx->flash_mmap + ctx->fs_head);

	if (cur_header->magic != LILOTAFS_RECORD && cur_header->magic != LILOTAFS_START &&
			cur_header->magic != LILOTAFS_START_CLEAN && cur_header->magic != LILOTAFS_WRAP_MARKER) {

		return count;
	}

	char *filename = (char *) cur_header + sizeof(struct lilotafs_rec_header);
	if (cur_header->status == LILOTAFS_STATUS_COMMITTED && (cur_header->magic != LILOTAFS_START || strlen(filename) != 0))
		count++;

	while (1) {
		struct lilotafs_rec_header *next_header = process_advance_header(ctx, cur_header, partition_size);
		if (!next_header)
			break;
		cur_header = next_header;

		if (cur_header->magic != LILOTAFS_RECORD && cur_header->magic != LILOTAFS_START &&
			cur_header->magic != LILOTAFS_START_CLEAN && cur_header->magic != LILOTAFS_WRAP_MARKER) {

			return count;
		}

		filename = (char *) cur_header + sizeof(struct lilotafs_rec_header);
		if (cur_header->status == LILOTAFS_STATUS_COMMITTED && (cur_header->magic != LILOTAFS_START || strlen(filename) != 0))
			count++;
	}

	return count;
}

uint32_t lilotafs_get_largest_file_size(struct lilotafs_context *ctx) {
	return ctx->largest_file_size;
}
uint32_t lilotafs_get_largest_filename_len(struct lilotafs_context *ctx) {
	return ctx->largest_filename_len;
}
uint32_t lilotafs_get_head(struct lilotafs_context *ctx) {
	return ctx->fs_head;
}
uint32_t lilotafs_get_tail(struct lilotafs_context *ctx) {
	return ctx->fs_tail;
}

