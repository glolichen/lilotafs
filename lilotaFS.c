#include "lilotaFS.h"

#include <bits/pthreadtypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "flash.h"

uint8_t *flash_mmap;
uint32_t fs_head, fs_tail;
uint32_t largest_file_size, largest_filename_len;

bool has_wear_marker;

#define FLASH_CAN_WRITE(want, current) (!((current) ^ ((want) | (current))))

uint32_t u32_min(uint32_t a, uint32_t b) {
	return a < b ? a : b;
}
uint32_t u32_max(uint32_t a, uint32_t b) {
	return a > b ? a : b;
}

struct fs_rec_header *scan_for_header(uint32_t start, uint32_t partition_size) {
	start = ALIGN_UP(start);
	for (uint32_t i = start; i < partition_size - sizeof(struct fs_rec_header); i += FS_HEADER_ALIGN) {
		struct fs_rec_header *header = (struct fs_rec_header *) (flash_mmap + i);
		if (header->magic == FS_START || header->magic == FS_START_CLEAN)
			return header;
	}
	return NULL;
}

struct fs_rec_header *process_advance_header(struct fs_rec_header *cur_header, uint32_t partition_size) {
	uint64_t current_offset = (uint64_t) cur_header - (uint64_t) flash_mmap;

	if (current_offset + sizeof(struct fs_rec_header) >= partition_size)
		return NULL;

	// file name immediately after header
	char *filename = (char *) cur_header + sizeof(struct fs_rec_header);
	// add 1 for null terminator
	uint32_t filename_len_padded = strlen(filename) + 1;

	// Malformed record - treat as end of valid data
	if (cur_header->data_len > partition_size - current_offset
			- sizeof(struct fs_rec_header) - filename_len_padded) {

		return NULL;
	}

	uint32_t next_offset;
	// if wrap header, back to start of partition
	if (cur_header->status == STATUS_WRAP_MARKER)
		next_offset = 0;
	else {
		// go to next header
		next_offset = current_offset;
		next_offset += sizeof(struct fs_rec_header);
		next_offset += filename_len_padded;
		next_offset += cur_header->data_len;
		next_offset = ALIGN_UP(next_offset);
	}

	if (next_offset >= partition_size)
		return NULL;

	if (cur_header->status == STATUS_COMMITTED) {
		// TODO: save to hash table
		if (cur_header->data_len > largest_file_size) {
			largest_file_size = cur_header->data_len;
			largest_filename_len =  filename_len_padded - 1;
		}
	}

	return (struct fs_rec_header *) (flash_mmap + next_offset);
}

struct scan_headers_result {
	struct fs_rec_header *last_header, *wear_marker, *wrap_marker, *migrating, *reserved;
	uint32_t num_files;
} scan_headers(struct fs_rec_header *start, uint32_t partition_size) {
	struct fs_rec_header *cur_header = start;
	struct scan_headers_result ret = {
		.last_header = cur_header,
		.wear_marker = NULL,
		.wrap_marker = NULL,
		.migrating = NULL,
		.reserved = NULL,
		.num_files = 0
	};

	if (cur_header->magic != FS_RECORD && cur_header->magic != FS_START && cur_header->magic != FS_START_CLEAN)
		return ret;

	while (1) {
		// if mount creates a dummy header, do not count it as a file (it is empty)
		char *filename = (char *) cur_header + sizeof(struct fs_rec_header);
		if (cur_header->status == STATUS_COMMITTED && (cur_header->magic != FS_START || strlen(filename) != 0))
			ret.num_files++;

		if (cur_header->status == STATUS_WEAR_MARKER)
			ret.wear_marker = cur_header;
		if (cur_header->status == STATUS_WRAP_MARKER)
			ret.wrap_marker = cur_header;
		if (cur_header->status == STATUS_MIGRATING)
			ret.migrating = cur_header;
		if (cur_header->status == STATUS_RESERVED)
			ret.reserved = cur_header;

		struct fs_rec_header *next_header = process_advance_header(cur_header, partition_size);
		if (!next_header)
			break;

		cur_header = next_header;
		if (cur_header->magic != FS_RECORD && cur_header->magic != FS_START && cur_header->magic != FS_START_CLEAN)
			break;
	}
	ret.last_header = cur_header;
	return ret;
}

uint32_t change_file_status(struct fs_rec_header *file_header, uint8_t status) {
	uint32_t file_offset = (uint64_t) file_header - (uint64_t) flash_mmap;
	uint32_t status_addr = file_offset + ((uint64_t) &file_header->status - (uint64_t) file_header);

	file_header->status = status;
	int out = flash_write(flash_mmap, &status, status_addr, 1);
	if (out)
		return FS_EFLASH;

	return 0;
}
uint32_t change_file_magic(struct fs_rec_header *file_header, uint16_t magic) {
	uint32_t file_offset = (uint64_t) file_header - (uint64_t) flash_mmap;
	uint32_t status_addr = file_offset + ((uint64_t) &file_header->magic - (uint64_t) file_header);

	file_header->magic = magic;
	int out = flash_write(flash_mmap, &magic, status_addr, 1);
	if (out)
		return FS_EFLASH;

	return 0;
}

// uint32_t calculate_free_space() {
// 	uint32_t partition_size = flash_get_total_size();
// 	uint32_t actual_head = ALIGN_DOWN(fs_head);
// 	if (fs_tail > actual_head)
// 		return partition_size - (fs_tail - actual_head);
// 	return actual_head - fs_tail;
// }

uint32_t check_free_space(uint32_t current_offset, uint32_t write_offset, uint32_t filename_len, uint32_t len) {
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
		struct fs_rec_header *old_file = (struct fs_rec_header *) (flash_mmap + current_offset);

		// the old and new file names should really be the same, but checking to be safe
		uint32_t old_filename_len = strlen((char *) old_file + sizeof(struct fs_rec_header));

		old_file_total = sizeof(struct fs_rec_header) + old_filename_len + 1 + old_file->data_len;
		old_file_total = ALIGN_UP(old_file_total);
	}

	uint32_t new_file_total = sizeof(struct fs_rec_header) + filename_len + 1 + len;
	new_file_total = ALIGN_UP(new_file_total);

	uint32_t wrap_marker_total = sizeof(struct fs_rec_header) + 1;
	wrap_marker_total = ALIGN_UP(wrap_marker_total);

	uint32_t largest_file_total = sizeof(struct fs_rec_header) + largest_filename_len + 1 + largest_file_size;
	largest_file_total = ALIGN_UP(largest_file_total);
	// the current file we're writing might be larger than current largest
	largest_file_total = u32_max(largest_file_total, u32_max(new_file_total, old_file_total));

	// hypothetical tail pointer if we added this file
	// log_wrap indicates whether there is already a wear marker,
	// or if one is needed in case we add the current file, and copy
	// the largest file twiec
	bool log_wrap = fs_head >= fs_tail;
	uint32_t current_position = write_offset;
	if (new_file_total + wrap_marker_total > partition_size - current_position) {
		if (log_wrap)
			return FS_ENOSPC;
		log_wrap = true;
		current_position = 0;
	}
	current_position += new_file_total;
	current_position = ALIGN_UP(current_position);

	// check if we can write the old file
	if (old_file_total + wrap_marker_total > partition_size - current_position) {
		if (log_wrap)
			return FS_ENOSPC;
		log_wrap = true;
		current_position = 0;
	}
	current_position += old_file_total;
	current_position = ALIGN_UP(current_position);

	// now we try to add the largerst file, twice
	for (uint32_t i = 0; i < 2; i++) {
		if (largest_file_total + wrap_marker_total > partition_size - current_position) {
			if (log_wrap)
				return FS_ENOSPC;
			log_wrap = true;
			current_position = 0;
		}
		current_position += largest_file_total;
		current_position = ALIGN_UP(current_position);

		// we cannot end such that the tail is in the same sector as the head,
		uint32_t sector_size = flash_get_sector_size();
		if (log_wrap && current_position / sector_size == fs_head / sector_size)
			return FS_ENOSPC;

		// if we wrapped the log / inserted a wear marker for any of the two files,
		// tail must be in front of head
		if (log_wrap && current_position >= fs_head)
			return FS_ENOSPC;
	}

	// if no wrap, there is no problem since we still have free space
	// after tail and before the end of partition

	return FS_SUCCESS;
}

// current_offset is updated with the new offset, provide pointer to UINT32_MAX if only creating
// updates fs_tail
uint32_t append_file(const char *filename, uint32_t *current_offset, void *buffer, uint32_t len,
					uint8_t want_status, bool add_wear_marker) {

	uint32_t filename_len = strlen(filename);

	uint32_t partition_size = flash_get_total_size();

	if (check_free_space(*current_offset, fs_tail, filename_len, len))
		return FS_ENOSPC;

	uint32_t new_file_total = sizeof(struct fs_rec_header) + filename_len + 1 + len;
	new_file_total = ALIGN_UP(new_file_total);

	uint32_t wrap_marker_total = sizeof(struct fs_rec_header) + 1;
	wrap_marker_total = ALIGN_UP(wrap_marker_total);

	// mark old file as migrating
	if (*current_offset != UINT32_MAX) {
		struct fs_rec_header *old_header = (struct fs_rec_header *) (flash_mmap + *current_offset);
		change_file_status(old_header, STATUS_MIGRATING);
	}

	uint32_t new_file_offset = fs_tail;
	// ensure we have space for a wrap marker afterwards
	if (new_file_total + wrap_marker_total > partition_size - fs_tail) {
		struct fs_rec_header wrap_marker = {
			.magic = FS_RECORD,
			.status = STATUS_WRAP_MARKER,
			.data_len = 0
		};
		char empty = 0;
		if (flash_write(flash_mmap, &wrap_marker, fs_tail, sizeof(struct fs_rec_header)))
			return FS_EFLASH;
		if (flash_write(flash_mmap, &empty, fs_tail + sizeof(struct fs_rec_header), 1))
			return FS_EFLASH;
		new_file_offset = 0;
	}

	// phase 1: reserve
	struct fs_rec_header file_header = {
		.magic = FS_RECORD,
		.status = STATUS_RESERVED,
		.data_len = len
	};
	if (flash_write(flash_mmap, &file_header, new_file_offset, sizeof(struct fs_rec_header)))
		return FS_EFLASH;

	// phase 2: write data
	if (flash_write(flash_mmap, filename, new_file_offset + sizeof(struct fs_rec_header), filename_len + 1))
		return FS_EFLASH;
	if (len) {
		if (flash_write(flash_mmap, buffer, new_file_offset + sizeof(struct fs_rec_header) + filename_len + 1, len))
			return FS_EFLASH;
	}

	// phase 3: commit
	if (change_file_status((struct fs_rec_header *) (flash_mmap + new_file_offset), want_status))
		return FS_EFLASH;

	// invalidate old
	if (*current_offset != UINT32_MAX) {
		struct fs_rec_header *old_header = (struct fs_rec_header *) (flash_mmap + *current_offset);
		if (change_file_status(old_header, STATUS_DELETED))
			return FS_EFLASH;
	}
	
	fs_tail = new_file_offset + sizeof(struct fs_rec_header) + filename_len + 1 + len;
	fs_tail = ALIGN_UP(fs_tail);

	if (!has_wear_marker && *current_offset != UINT32_MAX && add_wear_marker) {
		char empty = 0;
		uint32_t current_offset = UINT32_MAX;
		has_wear_marker = true;
		uint32_t code = append_file(&empty, &current_offset, NULL, 0, STATUS_WEAR_MARKER, false);
		if (code != FS_SUCCESS)
			return code;
	}

	*current_offset = new_file_offset;

	return FS_SUCCESS;
}

uint32_t lfs_set_file(int fd) {
	flash_mmap = mmap(NULL, flash_get_total_size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (flash_mmap == MAP_FAILED)
		return -1;
	return 0;
}

uint32_t remove_false_magic(uint8_t *start, uint32_t size) {
	for (uint8_t *p = (uint8_t *) ALIGN_UP((uint64_t) start); p < start + size; p += FS_HEADER_ALIGN) {
		uint16_t magic = *((uint16_t *) p);
		// if data is any magic number, set it to 0 to avoid picking it up by mistake
		if (magic == FS_RECORD || magic == FS_START || magic == FS_START_CLEAN) {
			uint32_t offset = (uint64_t) p - (uint64_t) flash_mmap;
			uint16_t zero = 0;
			if (flash_write(flash_mmap, &zero, offset, 2))
				return FS_EFLASH;
		}
	}
	return FS_SUCCESS;
}

uint32_t clobber_file_data(struct fs_rec_header *file) {
	char *filename = (char *) file + sizeof(struct fs_rec_header);
	return remove_false_magic((uint8_t *) filename, file->data_len + strlen(filename) + 1);
}

uint32_t erase_file(uint32_t cur_offset, uint32_t next_offset) {
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
		uint32_t sector_start = (next_offset / sector_size) * sector_size;
		if (remove_false_magic(flash_mmap + sector_start, next_offset % sector_size))
			return FS_EFLASH;
	}

	// previous offset cannot be in the same sector as tail
	if (fs_tail / sector_size == cur_offset / sector_size)
		return FS_ENOSPC;

	// erase old blocks, in reverse order (large address -> small address)
	for (uint32_t sector = next_offset / sector_size; sector >= cur_offset / sector_size + 1; sector--) {
		if (flash_erase_region(flash_mmap, (sector - 1) * sector_size, sector_size))
			return FS_EFLASH;
	}

	return FS_SUCCESS;
}

// this function has caused sooo many problems
uint32_t wear_level_compact(struct fs_rec_header *wear_marker, uint32_t num_files) {
	uint32_t partition_size = flash_get_total_size();
	uint32_t sector_size = flash_get_sector_size();

	// delete wear marker
	if (change_file_status(wear_marker, STATUS_DELETED))
		return FS_EFLASH;

	// this is the number of files we will copy to the tail
	// we set a limit defined by the macro, but also do not move more than the number of files
	// this is to avoid moving the same more than onec
	uint32_t count = u32_min(WEAR_LEVEL_MAX_RECORDS, num_files);

	struct fs_rec_header *cur_header = (struct fs_rec_header *) (flash_mmap + fs_head);

	// this should be impossible
	if (cur_header->magic != FS_RECORD && cur_header->magic != FS_START && cur_header->magic != FS_START_CLEAN) {
		printf("??????????\n");
		return FS_EUNKNOWN;
	}

	while (1) {
		struct fs_rec_header *next_header = process_advance_header(cur_header, partition_size);

		if (!next_header)
			break;
		if (next_header->magic != FS_RECORD && next_header->magic != FS_START && next_header->magic != FS_START_CLEAN)
			break;

		uint32_t cur_offset = (uint64_t) cur_header - (uint64_t) flash_mmap;
		uint32_t next_offset = (uint64_t) next_header - (uint64_t) flash_mmap;

		// move the current file to the tail
		uint32_t cur_offset_after_move = cur_offset;

		// NOTE: if we mount the filesystem and do not find a FS_START record
		// we create a record at offset 0 with magic FS_START and length 0
		// we do NOT want to move that record
		if (!(cur_header->data_len == 0 && cur_offset == 0) && cur_header->status == STATUS_COMMITTED) {
			count--;

			char *filename = (char *) cur_header + sizeof(struct fs_rec_header);
			uint8_t *data_start = (uint8_t *) (filename + strlen(filename) + 1);

			uint32_t code = append_file(filename, &cur_offset_after_move, data_start,
										cur_header->data_len, STATUS_COMMITTED, false);
			if (code != FS_SUCCESS)
				return code;
		}

		// we need to be deliberate about the order to make modifications in case of a crash
		// first, set the current file to FS_START_CLEAN, indicating this file is the old FS_START
		if (change_file_magic(next_header, FS_START_CLEAN))
			return FS_EFLASH;

		// we cannot have any magic numbers in 32 byte boundaries, or they will be detected as files
		// we want to clobber the filename too, so data start is the file name
		uint32_t code = clobber_file_data(cur_header);
		if (code != FS_SUCCESS)
			return code;

		// if the file crosses a flash boundary, or we are wrapping
		if (next_offset / sector_size != cur_offset / sector_size) {
			// if next_offset is 0, then we are processing a wrap marker
			// the wrap marker does not actually cross the flash erase boundar
			// we can simply erase all of the last sector
			code = erase_file(cur_offset, next_offset == 0 ? flash_get_total_size() : next_offset);
			if (code != FS_SUCCESS)
				return code;
		}

		// NOTE: if we crash during wear leveling, there are two failure cases:
		// If here or earlier, then there is an FS_START_CLEAN (next)
		// and maybe an FS_START, depending on whether the current file's header is erased


		// if (count == 1) {
		// 	printf("CRASH\n");
		// 	printf("0x%lx\n", (uint64_t) cur_header - (uint64_t) flash_mmap);
		// 	printf("0x%lx\n", (uint64_t) next_header - (uint64_t) flash_mmap);
		// 	exit(1);
		// }

		// done with processing the current file
		// now advance FS_START and delete the just-moved file
		if (change_file_magic(next_header, FS_START))
			return FS_EFLASH;

		// NOTE: if we crash here, there is one or two FS_START's
		// next is definitely FS_START, and current may have been erased

		// do not set magic to 0 if the sector has been erased
		if (next_offset / sector_size == cur_offset / sector_size) {
			if (change_file_magic(cur_header, 0))
				return FS_EFLASH;
		}

		fs_head = next_offset;
		cur_header = next_header;

		if (count == 0)
			break;
	}

	return FS_SUCCESS;
}

struct fs_rec_header *find_file_name(const char *name) {
	uint32_t filename_len = strlen(name);
	uint32_t partition_size = flash_get_total_size();

	struct fs_rec_header *cur_header = (struct fs_rec_header *) (flash_mmap + fs_head);
	while (1) {
		if (cur_header->status == STATUS_COMMITTED) {
			char *cur_filename = (char *) cur_header + sizeof(struct fs_rec_header);
			if (strlen(cur_filename) == filename_len && strncmp(cur_filename, name, filename_len) == 0)
				return cur_header;
		}

		struct fs_rec_header *next_header = process_advance_header(cur_header, partition_size);
		if (!next_header)
			break;
		cur_header = next_header;
		if (cur_header->magic != FS_RECORD)
			break;
	}
	return NULL;
}

uint32_t lfs_mount() {
	largest_file_size = 0, largest_filename_len = 0;
	fs_head = 0, fs_tail = 0;
	has_wear_marker = false;

	uint32_t partition_size = flash_get_total_size();

	struct fs_rec_header *cur_header = (struct fs_rec_header *) flash_mmap;
	struct fs_rec_header *wear_marker = NULL;
	uint32_t num_files = 0;

	// if position 0 is an normal file, not the head, or there is nothing there
	if (cur_header->magic != FS_START_CLEAN && cur_header->magic != FS_START) {
		// follow current file until no more
		// (if no file at 0 this will return cur_header immediately, then offset = 0)
		struct scan_headers_result scan_result = scan_headers(cur_header, partition_size);
		cur_header = scan_result.last_header;
		if (scan_result.wear_marker)
			wear_marker = scan_result.wear_marker;

		uint32_t offset = (uint64_t) cur_header - (uint64_t) flash_mmap;
		// scan disk 1 byte at a time until we find FS_START magic
		cur_header = scan_for_header(offset, partition_size);
	}

	// if none found, write a FS_START header at 0
	if (!cur_header) {
		struct fs_rec_header new_header = {
			.magic = FS_START,
			.status = STATUS_COMMITTED,
			.data_len = 0
		};

		char empty = 0;
		// write record and empty filename
		if (flash_write(flash_mmap, &new_header, 0, sizeof(struct fs_rec_header)))
			return FS_EFLASH;
		if (flash_write(flash_mmap, &empty, sizeof(struct fs_rec_header), 1))
			return FS_EFLASH;

		fs_head = 0;
		fs_tail = sizeof(struct fs_rec_header) + 1;
		fs_tail = ALIGN_UP(fs_tail);

		printf("head = 0x%lx\n", (uint64_t) fs_head);
		printf("tail = 0x%lx\n", (uint64_t) fs_tail);

		return FS_SUCCESS;
	}

	// first, check for crash

	// there are multiple cases of crash during wear leveling
	// if the file processed during the crash is across multiple erase sectors,
	// and erase_file erased the header of that file:
	//   1. first record is FS_START_CLEAN, no FS_START
	// else (if the file is in only one erase sector, or crash before the record is erased)
	//   2. first record is FS_START, second record is FS_START
	//      in this case, the first file has already been cleaned up
	//      we can simply set the first record magic to 00, and done
	//   3. first record is FS_START, second record is FS_START_CLEAN
	//      do not know that we are done cleaning up the first file
	//      we clean up the first file, set the second record to FS_START
	//     a nd set the first record to 00

	// 1. if the first is FS_START_CLEAN
	if (cur_header->magic == FS_START_CLEAN) {
		if (change_file_magic(cur_header, FS_START))
			return FS_EFLASH;
	}
	else {
		struct fs_rec_header *next_header = process_advance_header(cur_header, partition_size);
		if (next_header) {
			// 2. crash after the file has been cleaned up, simply delete the old file
			if (next_header->magic == FS_START) {
				if (change_file_magic(cur_header, 0))
					return FS_EFLASH;
				cur_header = next_header;
			}
			// 3. complicated
			if (next_header->magic == FS_START_CLEAN) {
				uint32_t cur_offset = (uint64_t) cur_header - (uint64_t) flash_mmap;
				uint32_t next_offset = (uint64_t) next_header - (uint64_t) flash_mmap;

				// we cannot have any magic numbers in 32 byte boundaries, or they will be detected as files
				uint32_t code = clobber_file_data(cur_header);
				if (code != FS_SUCCESS)
					return code;

				// if the file crosses a flash boundary, or we are wrapping
				uint32_t sector_size = flash_get_sector_size();
				if (next_offset / sector_size != cur_offset / sector_size) {
					// if next_offset is 0, then we are processing a wrap marker
					// the wrap marker does not actually cross the flash erase boundar
					// we can simply erase all of the last sector
					code = erase_file(cur_offset, next_offset == 0 ? flash_get_total_size() : next_offset);
					if (code != FS_SUCCESS)
						return code;
				}

				// done with processing the current file
				// now advance FS_START and delete the just-moved file
				if (change_file_magic(next_header, FS_START))
					return FS_EFLASH;

				// do not set magic to 0 if the sector has been erased
				if (next_offset / sector_size == cur_offset / sector_size) {
					if (change_file_magic(cur_header, 0))
						return FS_EFLASH;
				}

				cur_header = next_header;
			}
		}
	}

	fs_head = (uint64_t) cur_header - (uint64_t) flash_mmap;

	// scan until no more files -- set that to tail pointer
	struct scan_headers_result scan_result = scan_headers(cur_header, partition_size);
	cur_header = scan_result.last_header;
	num_files += scan_result.num_files;
	if (scan_result.wear_marker)
		wear_marker = scan_result.wear_marker;

	fs_tail = (uint64_t) cur_header - (uint64_t) flash_mmap;
	

	// TODO: consider whether to wear level before recovery
	// in case we need the extra space to append a migrating file

	// found migrating, found reserved
	if (scan_result.migrating && scan_result.reserved) {
		// make the old file the active/committed file
		// we can do this by copying the content of the migrating file to reserved
		// but we don't know if we can (cannot change bits from 0 to 1)

		struct fs_rec_header *migrating = scan_result.migrating;
		struct fs_rec_header *reserved = scan_result.reserved;

		uint32_t reserved_offset = (uint64_t) reserved - (uint64_t) flash_mmap;
		char *filename = (char *) migrating + sizeof(struct fs_rec_header);

		bool can_write = FLASH_CAN_WRITE(migrating->data_len, reserved->data_len);

		uint32_t total_size = strlen(filename) + 1 + migrating->data_len;
		uint8_t *migrating_ptr = (uint8_t *) filename;
		uint8_t *reserved_ptr = (uint8_t *) reserved + sizeof(struct fs_rec_header);

		for (uint32_t i = 0; i < total_size && can_write; i++) {
			if (!FLASH_CAN_WRITE(migrating_ptr[i], reserved_ptr[i]))
				can_write = false;
		}

		if (can_write) {
			if (flash_write(flash_mmap, migrating_ptr, reserved_offset + sizeof(struct fs_rec_header), total_size))
				return FS_EFLASH;
		}
		else {
			if (change_file_status(reserved, STATUS_DELETED))
				return FS_EFLASH;

			uint32_t migrating_offset = (uint64_t) migrating - (uint64_t) flash_mmap;
			uint32_t code = append_file(filename, &migrating_offset, filename + strlen(filename) + 1,
							   migrating->data_len, STATUS_COMMITTED, false);

			if (code != FS_SUCCESS)
				return code;
		}
	}

	// found migrating, no reserved: there may or may not be a commited file of the same name
	// possible there is a committed file of the same name since we commit file before delete migrating
	else if (scan_result.migrating && !scan_result.reserved) {
		char *filename = (char *) scan_result.migrating + sizeof(struct fs_rec_header);
		struct fs_rec_header *found_file = find_file_name(filename);

		// if we find a committed file of the same name, there is no need to do anything
		// append file crashed right before setting the migrating file to deleted, complete this

		if (!found_file) {
			// there is no committed file of the same name
			// append a file with the same contents

			uint32_t migrating_offset = (uint64_t) scan_result.migrating - (uint64_t) flash_mmap;
			uint32_t code = append_file(filename, &migrating_offset, filename + strlen(filename) + 1,
							   scan_result.migrating->data_len, STATUS_COMMITTED, false);

			if (code != FS_SUCCESS)
				return code;
		}

		if (change_file_status(scan_result.migrating, STATUS_DELETED))
			return FS_EFLASH;
	}

	printf("head = 0x%lx\n", (uint64_t) fs_head);
	printf("tail = 0x%lx\n", (uint64_t) fs_tail);

	// if we find wear marker, need to perform wear leveling
	if (wear_marker) {
		uint32_t code = wear_level_compact(wear_marker, num_files);
		if (code != FS_SUCCESS)
			return code;

		printf("new head = 0x%lx\n", (uint64_t) fs_head);
		printf("new tail = 0x%lx\n", (uint64_t) fs_tail);
	}

	return FS_SUCCESS;
}






uint32_t fd_list_size = 0, fd_list_capacity = 0;
struct fs_file_descriptor *fd_list = NULL;

uint32_t fd_list_append(struct fs_file_descriptor fd) {
	if (fd_list_size < fd_list_capacity) {
		fd_list[fd_list_size++] = fd;
		return 0;
	}

	if (fd_list_capacity == 0) {
		fd_list_capacity = 1;
		fd_list = (struct fs_file_descriptor *) malloc(sizeof(struct fs_file_descriptor));
	}
	else {
		fd_list_capacity *= 2;
		fd_list = (struct fs_file_descriptor *) realloc(fd_list, fd_list_capacity * sizeof(struct fs_file_descriptor));
	}
	if (!fd_list)
		return 1;

	fd_list[fd_list_size++] = fd;
	return 0;
}

// returns index in list, or UINT32_MAX on failure
uint32_t fd_list_add(struct fs_file_descriptor fd) {
	for (uint32_t i = 0; i < fd_list_size; i++) {
		if (!fd_list[i].in_use) {
			fd_list[i] = fd;
			return i;
		}
	}
	if (fd_list_append(fd))
		return UINT32_MAX;
	return fd_list_size - 1;
}

uint32_t vfs_open(const char *name, int flags) {
	uint32_t filename_len = strlen(name);
	if (filename_len > FS_MAX_FILENAME_LEN)
		return -1;

	struct fs_rec_header *file_found = find_file_name(name);

	struct fs_file_descriptor fd = {
		.in_use = true,
		.offset = UINT32_MAX,
	};

	if (!file_found) {
		if ((flags & FS_CREATE) != FS_CREATE)
			return -1;
		uint32_t code = append_file(name, &fd.offset, NULL, 0, STATUS_COMMITTED, true);
		if (code != FS_SUCCESS)
			return -1;
		file_found = (struct fs_rec_header *) (flash_mmap + fd.offset);
	}

	fd.offset = (uint64_t) file_found - (uint64_t) flash_mmap;

	uint32_t fd_index = fd_list_add(fd);
	return fd_index == UINT32_MAX ? 0 : fd_index + 1;
}

uint32_t vfs_close(uint32_t fd) {
	if (fd - 1 >= fd_list_size)
		return FS_EBADF;

	fd--;
	if (fd >= fd_list_size)
		return -1;

	fd_list[fd].in_use = false;
	fd_list[fd].offset = 0;

	return 0;
}

uint32_t vfs_write(uint32_t fd, void *buffer, uint32_t len) {
	if (fd - 1 >= fd_list_size)
		return FS_EBADF;

	struct fs_file_descriptor *descriptor = &fd_list[fd - 1];
	struct fs_rec_header *header = (struct fs_rec_header *) (flash_mmap + descriptor->offset);
	char *filename = (char *) header + sizeof(struct fs_rec_header);

	uint32_t code = append_file(filename, &descriptor->offset, buffer, len, STATUS_COMMITTED, true);
	if (code != FS_SUCCESS)
		return code;

	if (header->data_len == largest_file_size && len < header->data_len) {
		largest_file_size = 0, largest_filename_len = 0;
		struct fs_rec_header *head_header = (struct fs_rec_header *) (flash_mmap + fs_head);
		scan_headers(head_header, flash_get_total_size());
	}
	else if (len >= largest_file_size) {
		largest_file_size = len;
		largest_filename_len = strlen(filename);
	}

	return FS_SUCCESS;
}

uint32_t vfs_get_size(uint32_t fd) {
	if (fd - 1 >= fd_list_size)
		return FS_EBADF;

	struct fs_file_descriptor descriptor = fd_list[fd - 1];
	struct fs_rec_header *header = (struct fs_rec_header *) (flash_mmap + descriptor.offset);
	return header->data_len;
}

uint32_t vfs_read(uint32_t fd, void *buffer, uint32_t addr, uint32_t len) {
	if (fd - 1 >= fd_list_size)
		return FS_EBADF;

	struct fs_file_descriptor descriptor = fd_list[fd - 1];
	struct fs_rec_header *header = (struct fs_rec_header *) (flash_mmap + descriptor.offset);

	char *filename = (char *) header + sizeof(struct fs_rec_header);
	uint8_t *data_start = (uint8_t *) filename + strlen(filename) + 1;
	memcpy(buffer, data_start + addr, len);

	return FS_SUCCESS;
}

uint32_t vfs_delete(uint32_t fd) {
	if (fd - 1 >= fd_list_size)
		return FS_EBADF;

	struct fs_file_descriptor descriptor = fd_list[fd - 1];
	struct fs_rec_header *header = (struct fs_rec_header *) (flash_mmap + descriptor.offset);
	if (change_file_status(header, STATUS_DELETED))
		return FS_EFLASH;

	if (header->data_len == largest_filename_len) {
		largest_file_size = 0, largest_filename_len = 0;
		struct fs_rec_header *head_header = (struct fs_rec_header *) (flash_mmap + fs_head);
		scan_headers(head_header, flash_get_total_size());
	}

	if (!has_wear_marker) {
		char empty = 0;
		uint32_t current_offset = UINT32_MAX;
		has_wear_marker = true;
		return append_file(&empty, &current_offset, NULL, 0, STATUS_WEAR_MARKER, false);
	}

	return FS_SUCCESS;
}


// simulate graceful computer shutdown
uint32_t lfs_unmount() {
	largest_file_size = 0, largest_filename_len = 0;
	fs_head = 0, fs_tail = 0;
	has_wear_marker = false;

	// close all open FDs
	fd_list_size = 0, fd_list_capacity = 0;
	free(fd_list);

	munmap(flash_mmap, flash_get_total_size());

	return FS_SUCCESS;
}

uint32_t lfs_count_files() {
	uint32_t count = 0;
	uint32_t partition_size = flash_get_total_size();
	struct fs_rec_header *cur_header = (struct fs_rec_header *) (flash_mmap + fs_head);

	if (cur_header->magic != FS_RECORD && cur_header->magic != FS_START && cur_header->magic != FS_START_CLEAN)
		return count;

	char *filename = (char *) cur_header + sizeof(struct fs_rec_header);
	if (cur_header->status == STATUS_COMMITTED && (cur_header->magic != FS_START || strlen(filename) != 0))
		count++;

	while (1) {
		struct fs_rec_header *next_header = process_advance_header(cur_header, partition_size);
		if (!next_header)
			break;
		cur_header = next_header;
		if (cur_header->magic != FS_RECORD && cur_header->magic != FS_START && cur_header->magic != FS_START_CLEAN)
			break;

		filename = (char *) cur_header + sizeof(struct fs_rec_header);
		if (cur_header->status == STATUS_COMMITTED && (cur_header->magic != FS_START || strlen(filename) != 0))
			count++;
	}

	return count;
}

uint32_t lfs_get_largest_file_size() {
	return largest_file_size;
}
uint32_t lfs_get_largest_filename_len() {
	return largest_filename_len;
}
uint32_t lfs_get_head() {
	return fs_head;
}
uint32_t lfs_get_tail() {
	return fs_tail;
}

