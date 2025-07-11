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

struct fs_rec_header *scan_for_header(uint32_t start, uint32_t partition_size) {
	start = ALIGN_UP(start);
	for (uint32_t i = start; i < partition_size - sizeof(struct fs_rec_header); i += FS_HEADER_ALIGN) {
		struct fs_rec_header *header = (struct fs_rec_header *) (flash_mmap + i);
		if (header->magic == FS_START || header->magic == FS_START_CLEAN)
			return header;
	}
	return NULL;
}

struct fs_rec_header *process_header(struct fs_rec_header *cur_header, uint32_t partition_size) {
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

	uint64_t next_offset;
	// if wrap header, back to start of partition
	if (cur_header->status == STATUS_WRAP_MARKER)
		next_offset = (uint64_t) flash_mmap;
	else {
		// go to next header
		next_offset = (uint64_t) cur_header;
		next_offset += sizeof(struct fs_rec_header);
		next_offset += filename_len_padded;
		next_offset += cur_header->data_len;
		next_offset = ALIGN_UP(next_offset);
	}

	if ((uint64_t) next_offset - (uint64_t) flash_mmap > partition_size)
		return NULL;

	if (cur_header->status == STATUS_COMMITTED) {
		// TODO: save to hash table
		if (cur_header->data_len > largest_file_size) {
			largest_file_size = cur_header->data_len;
			largest_filename_len =  filename_len_padded - 1;
		}
	}

	return (struct fs_rec_header *) next_offset;
}

struct fs_rec_header *found_wear_marker;

// will also find the largest file
struct fs_rec_header *scan_headers(struct fs_rec_header *start, uint32_t partition_size) {
	struct fs_rec_header *cur_header = start;
	if (cur_header->magic != FS_RECORD && cur_header->magic != FS_START && cur_header->magic != FS_START_CLEAN)
		return cur_header;
	while (1) {
		if (cur_header->status == STATUS_WEAR_MARKER)
			found_wear_marker = cur_header;
		struct fs_rec_header *next_header = process_header(cur_header, partition_size);
		if (!next_header)
			break;
		cur_header = next_header;
		if (cur_header->magic != FS_RECORD && cur_header->magic != FS_START && cur_header->magic != FS_START_CLEAN)
			break;
	}
	return cur_header;
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

uint32_t calculate_free_space() {
	uint32_t partition_size = flash_get_total_size();
	if (fs_tail > fs_head)
		return partition_size - (fs_tail - fs_head);
	return fs_head - fs_tail;
}

// current_offset is updated with the new offset, provide pointer to UINT32_MAX if only creating
uint32_t append_file(const char *filename, uint32_t *current_offset, void *buffer, uint32_t len,
					uint8_t want_status, bool add_wear_marker) {

	uint32_t filename_len = strlen(filename);

	uint32_t partition_size = flash_get_total_size();
	uint32_t tail_offset = fs_tail;

	uint32_t new_file_total = sizeof(struct fs_rec_header) + filename_len + 1 + len;
	new_file_total = ALIGN_UP(new_file_total);

	uint32_t largest_file_total = sizeof(struct fs_rec_header) + largest_filename_len + 1 + largest_file_size;
	largest_file_total = ALIGN_UP(largest_file_total);

	uint32_t wrap_marker_total = sizeof(struct fs_rec_header) + 1;
	wrap_marker_total = ALIGN_UP(wrap_marker_total);

	if (calculate_free_space() < (new_file_total + largest_file_total + wrap_marker_total))
		return FS_ENOSPC;

	// mark old version as migrating
	if (*current_offset != UINT32_MAX) {
		struct fs_rec_header *old_header = (struct fs_rec_header *) (flash_mmap + *current_offset);
		change_file_status(old_header, STATUS_MIGRATING);
	}

	uint32_t new_file_offset = tail_offset;
	if (new_file_total > partition_size - tail_offset) {
		struct fs_rec_header wrap_marker = {
			.magic = FS_RECORD,
			.status = STATUS_WRAP_MARKER,
			.data_len = 0
		};
		char empty = 0;
		if (flash_write(flash_mmap, &wrap_marker, tail_offset, sizeof(struct fs_rec_header)))
			return FS_EFLASH;
		if (flash_write(flash_mmap, &empty, tail_offset + sizeof(struct fs_rec_header), 1))
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
		return append_file(&empty, &current_offset, NULL, 0, STATUS_WEAR_MARKER, false);
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

uint32_t lfs_mount() {
	largest_file_size = 0, largest_filename_len = 0;
	fs_head = 0, fs_tail = 0;
	has_wear_marker = false;

	found_wear_marker = NULL;

	uint32_t partition_size = flash_get_total_size();

	struct fs_rec_header *cur_header = (struct fs_rec_header *) flash_mmap;
	if (cur_header->magic != FS_START_CLEAN && cur_header->magic != FS_START) {
		cur_header = scan_headers(cur_header, partition_size);
		uint32_t offset = (uint64_t) cur_header - (uint64_t) flash_mmap;
		cur_header = scan_for_header(offset, partition_size);
	}

	// if none found,  write a FS_START header at 0
	if (!cur_header) {
		struct fs_rec_header new_header = {
			.magic = FS_START,
			.status = STATUS_COMMITTED,
			.data_len = 0
		};
		char empty = 0;
		if (flash_write(flash_mmap, &new_header, 0, sizeof(struct fs_rec_header)))
			return FS_EFLASH;
		if (flash_write(flash_mmap, &empty, sizeof(struct fs_rec_header), 1))
			return FS_EFLASH;

		fs_head = 0;
		fs_tail = sizeof(struct fs_rec_header) + 1;
		fs_tail = ALIGN_UP(fs_tail);
	}
	else {
		fs_head = (uint64_t) cur_header - (uint64_t) flash_mmap;

		// if crash in middle of advancing FS_START for wear leveling, where advancing FS_START to same flash sector
		// then next header is set to FS_START, but previous has not been set to 0
		char *filename = (char *) cur_header + sizeof(struct fs_rec_header);
		struct fs_rec_header *next_header = (struct fs_rec_header *) (filename + strlen(filename) + 1 + cur_header->data_len);
		next_header = (struct fs_rec_header *) ALIGN_UP((uint64_t) next_header);
		if (next_header->magic == FS_START) {
			printf("crash detected, wear level, same sector\n");
			change_file_magic(cur_header, 0x0000);
			cur_header = next_header;
			fs_head = (uint64_t) cur_header - (uint64_t) flash_mmap;
		}

		cur_header = scan_headers(cur_header, partition_size);
		fs_tail = (uint64_t) cur_header - (uint64_t) flash_mmap;
	}

	printf("head = 0x%lx\n", (uint64_t) fs_head);
	printf("tail = 0x%lx\n", (uint64_t) fs_tail);

	if (found_wear_marker) {
		// 1. delete wear marker
		if (change_file_status(found_wear_marker, STATUS_DELETED))
			return FS_EFLASH;

		// 2. copy each of next WEAR_LEVEL_MAX_RECORDS files to tail
		uint32_t count = 0;
		cur_header = (struct fs_rec_header *) (flash_mmap + fs_head);

		// how would this even happen...
		if (cur_header->magic != FS_RECORD && cur_header->magic != FS_START && cur_header->magic != FS_START_CLEAN) {
			printf("??????????\n");
			return -1;
		}

		while (1) {
			// scan for the next non-deleted file
			bool has_next_header = false;
			struct fs_rec_header *next_header = cur_header;
			while (1) {
				next_header = process_header(next_header, partition_size);
				if (!next_header)
					break;
				if (next_header->magic != FS_RECORD && next_header->magic != FS_START && next_header->magic != FS_START_CLEAN)
					break;
				if (next_header->status == STATUS_COMMITTED) {
					has_next_header = true;
					break;
				}
				if (change_file_magic(next_header, 0x0000))
					return FS_EFLASH;
			}
			
			// NOTE: there should be no case where there is no next header
			// (even if there was one file, it was marked deleted, there is also the wear marker)
			if (!has_next_header)
				break;

			uint32_t next_offset = (uint64_t) next_header - (uint64_t) flash_mmap;

			// move the current file to the tail
			uint32_t cur_previous_offset = (uint64_t) cur_header - (uint64_t) flash_mmap;
			uint32_t cur_new_offset = cur_previous_offset;

			char *filename = (char *) cur_header + sizeof(struct fs_rec_header);
			uint8_t *data_start = (uint8_t *) (filename + strlen(filename) + 1);

			// NOTE: if we mount the filesystem and do not find a FS_START record
			// we create a record at offset 0 with magic FS_START and length 0
			// we do NOT want to move that record
			if (!(cur_header->data_len == 0 && cur_previous_offset == 0)) {
				uint32_t code = append_file(filename, &cur_new_offset,
								   data_start, cur_header->data_len, STATUS_COMMITTED, false);
				count++;
				if (code)
					return code;
			}

			// make the next file the new FS_START
			uint32_t sector_size = flash_get_sector_size();
			// within same flash block/sector
			if (next_offset / sector_size == cur_previous_offset / sector_size) {
				if (change_file_magic((struct fs_rec_header *) (flash_mmap + next_offset), FS_START))
					return FS_EFLASH;
				if (change_file_magic((struct fs_rec_header *) (flash_mmap + cur_previous_offset), 0x0000))
					return FS_EFLASH;
			}
			// across blocks
			else {
				// write the next header as FS_START_CLEAN
				if (change_file_magic((struct fs_rec_header *) (flash_mmap + next_offset), FS_START_CLEAN))
					return FS_EFLASH;

				// clobber part of block the new file is in, that is before the file
				if (next_offset % sector_size != 0) {
					uint8_t *zero = (uint8_t *) calloc(next_offset % sector_size, sizeof(uint8_t));
					if (flash_write(flash_mmap, zero, (next_offset / sector_size) * sector_size, next_offset % sector_size))
						return FS_EFLASH;
				}

				// previous offset cannot be in the same sector as tail
				if (fs_tail / sector_size == cur_previous_offset / sector_size)
					return FS_ENOSPC;

				// erase old blocks, in reverse order (large address -> small address)
				for (uint32_t sector = cur_previous_offset / sector_size;
						sector < next_offset / sector_size; sector--) {

					if (flash_erase_region(flash_mmap, sector * sector_size, sector_size))
						return FS_EFLASH;
				}

				if (change_file_magic((struct fs_rec_header *) (flash_mmap + next_offset), FS_START))
					return FS_EFLASH;
			}

			cur_header = next_header;
			if (count == WEAR_LEVEL_MAX_RECORDS) {
				fs_head = next_offset;
				cur_header = scan_headers(cur_header, partition_size);
				fs_tail = (uint64_t) cur_header - (uint64_t) flash_mmap;
				break;
			}
		}

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
		if (fd_list[i].filename == NULL) {
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

	uint32_t partition_size = flash_get_total_size();

	bool file_found = false;
	struct fs_rec_header *cur_header = (struct fs_rec_header *) (flash_mmap + fs_head);
	while (1) {
		if (cur_header->status == STATUS_COMMITTED) {
			char *cur_filename = (char *) cur_header + sizeof(struct fs_rec_header);
			if (strlen(cur_filename) == filename_len && strncmp(cur_filename, name, filename_len) == 0) {
				file_found = true;
				break;
			}
		}

		struct fs_rec_header *next_header = process_header(cur_header, partition_size);
		if (!next_header)
			break;
		cur_header = next_header;
		if (cur_header->magic != FS_RECORD)
			break;
	}

	struct fs_file_descriptor fd = {
		.offset = UINT32_MAX,
		.filename = (char *) malloc((filename_len + 1) * sizeof(char)),
	};
	memcpy(fd.filename, name, filename_len);

	if (!file_found) {
		if ((flags & FS_CREATE) != FS_CREATE) {
			free(fd.filename);
			return -1;
		}

		uint32_t code = append_file(fd.filename, &fd.offset, NULL, 0, STATUS_COMMITTED, true);

		if (code != FS_SUCCESS) {
			free(fd.filename);
			return -1;
		}
		cur_header = (struct fs_rec_header *) (flash_mmap + fd.offset);
	}

	fd.offset = (uint64_t) cur_header - (uint64_t) flash_mmap;

	uint32_t fd_index = fd_list_add(fd);
	return fd_index == UINT32_MAX ? 0 : fd_index + 1;
}

uint32_t vfs_close(uint32_t fd) {
	fd--;
	if (fd >= fd_list_size)
		return -1;

	struct fs_file_descriptor descriptor = fd_list[fd];

	if (!descriptor.filename)
		return -1;
	free(descriptor.filename);
	descriptor.filename = NULL;

	descriptor.offset = 0;

	return 0;
}

uint32_t vfs_write(uint32_t fd, void *buffer, uint32_t len) {
	struct fs_file_descriptor *descriptor = &fd_list[fd - 1];
	struct fs_rec_header *header = (struct fs_rec_header *) (flash_mmap + descriptor->offset);

	uint32_t code = append_file(descriptor->filename, &descriptor->offset, buffer, len, STATUS_COMMITTED, true);
	if (code != FS_SUCCESS)
		return code;

	if (header->data_len == largest_file_size && len < header->data_len) {
		largest_file_size = 0, largest_filename_len = 0;
		struct fs_rec_header *head_header = (struct fs_rec_header *) (flash_mmap + fs_head);
		scan_headers(head_header, flash_get_total_size());
	}
	else if (len >= largest_file_size) {
		largest_file_size = len;
		largest_filename_len = strlen(descriptor->filename);
	}

	return FS_SUCCESS;
}

uint32_t vfs_get_size(uint32_t fd) {
	struct fs_file_descriptor descriptor = fd_list[fd - 1];
	struct fs_rec_header *header = (struct fs_rec_header *) (flash_mmap + descriptor.offset);
	return header->data_len;
}

uint32_t vfs_read(uint32_t fd, void *buffer, uint32_t addr, uint32_t len) {
	struct fs_file_descriptor descriptor = fd_list[fd - 1];
	struct fs_rec_header *header = (struct fs_rec_header *) (flash_mmap + descriptor.offset);

	char *filename = (char *) header + sizeof(struct fs_rec_header);
	uint8_t *data_start = (uint8_t *) filename + strlen(filename) + 1;
	memcpy(buffer, data_start + addr, len);

	return FS_SUCCESS;
}

uint32_t vfs_delete(uint32_t fd) {
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

