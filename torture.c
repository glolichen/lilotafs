#include <linux/limits.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>

#include "util.h"
#include "flash.h"
#include "lilotaFS.h"

struct table_entry {
	char *filename;
	uint8_t *content;
};

uint32_t inspect_fs(struct table_entry *files) {
	printf("========== INSPECT ==========\n");
	uint32_t wrong_count = 0;
	for (uint32_t i = 0; i < FILE_COUNT; i++) {
		struct table_entry entry = files[i];
		if (!entry.content) {
			printf("    NOTE: %.63s (%u) empty\n", entry.filename, i);
			continue;
		}
		uint32_t fd = vfs_open(entry.filename, FS_READABLE | FS_WRITABLE);
		if (fd == UINT32_MAX) {
			printf("WRONG: %.63s (%u) -- fd == -1\n", entry.filename, i);
			wrong_count++;
			continue;
		}
		uint32_t size = vfs_get_size(fd);
		uint8_t *file_content = (uint8_t *) malloc(size);
		if (file_content == NULL) {
			printf("WRONG: %.63s (%u) -- malloc fail\n", entry.filename, i);
			wrong_count++;
			vfs_close(fd);
			continue;
		}
		if (vfs_read(fd, file_content, 0, size)) {
			printf("WRONG: %.63s (%u) -- read fail\n", entry.filename, i);
			wrong_count++;
			free(file_content);
			vfs_close(fd);
			continue;
		}
		bool wrong = false;
		for (uint32_t j = 0; j < size; j++) {
			if (file_content[j] != entry.content[j]) {
				wrong = true;
				break;
			}
		}
		free(file_content);
		vfs_close(fd);
		if (wrong) {
			printf("WRONG: %.63s (%u) -- content wrong\n", entry.filename, i);
			wrong_count++;
			continue;
		}
		printf("CORRECT: %.63s (%u)\n", entry.filename, i);
	}

	uint32_t file_count = lfs_count_files();
	if (file_count != FILE_COUNT)
		printf("# of files wrong: LFS reports %u\n", file_count);

	printf("========== INSPECT OVER ==========\n");

	return wrong_count;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		printf("error: must enter arguments\n");
		return 1;
	}

	printf("total size %d, sector size %d\n", TOTAL_SIZE, SECTOR_SIZE);

	char *disk_name = argv[1];
	int disk = open(disk_name, O_RDWR);

	uint64_t seed;
	if (argc == 3)
		seed = strtoul(argv[2], NULL, 0);
	else
		seed = time(NULL);

	printf("random seed: %ld\n", seed);
	srand(seed);

	lfs_set_file(disk);
	flash_set_crash(CRASH_WRITE_MIN_MOVES, CRASH_WRITE_MAX_MOVES, CRASH_ERASE_MIN_MOVES, CRASH_ERASE_MAX_MOVES);

	printf("mount: %u\n", lfs_mount());

	struct table_entry files[FILE_COUNT];
	uint32_t fds[FILE_COUNT];

	for (uint32_t file = 0; file < FILE_COUNT; file++) {
		uint32_t filename_len = RANDOM_NUMBER(10, 63);

		struct table_entry entry = {
			.filename = (char *) calloc(filename_len + 1, sizeof(char)),
			.content = NULL
		};
		for (uint32_t i = 0; i < filename_len; i++)
			entry.filename[i] = RANDOM_NUMBER(33, 126);
		files[file] = entry;

		fds[file] = vfs_open(entry.filename, FS_WRITABLE | FS_READABLE | FS_CREATE);
		
		printf("file %u = %.63s\n", file, entry.filename);
	}

	uint32_t total_wrong = 0, error_code = 0;
	for (uint32_t op = 0; op < OP_COUNT; op++) {
		uint32_t file = RANDOM_NUMBER(0, FILE_COUNT - 1);
		uint32_t size = RANDOM_NUMBER(MIN_SIZE, MAX_SIZE);
		uint8_t *random = (uint8_t *) malloc(size);
		for (uint32_t i = 0; i < size; i++)
			random[i] = RANDOM_NUMBER(0, 255);

		uint32_t code = vfs_write(fds[file], random, size);
		if (code == FS_SUCCESS) {
			struct table_entry *entry = &files[file];
			if (entry->content)
				free(entry->content);
			entry->content = random;
		}

		if (code != FS_SUCCESS && code != FS_ENOSPC) {
			free(random);
			printf("write error: %u\n", code);
			error_code = code;
			goto cleanup;
		}

		if (code == FS_ENOSPC || (op % OPS_PER_REMOUNT == 0 && op != 0)) {
			printf("\n\n=============== UNMOUNT/REMOUNT ===============\n");
			if (code == FS_SUCCESS) {
				uint32_t wrong = inspect_fs(files);
				total_wrong += wrong;
				if (wrong != 0) {
					;
				}
				printf("before unmount inspect: %u\n", wrong);
			}
			else
				printf("out of space!\n");

			printf("\nunmount: %u\n", lfs_unmount());
			lfs_set_file(disk);
			uint32_t mount_code = lfs_mount();
			if (mount_code != FS_SUCCESS) {
				free(random);
				printf("mount error: %u\n", code);
				error_code = code;
				goto cleanup;
			}

			for (uint32_t file = 0; file < FILE_COUNT; file++) {
				struct table_entry entry = files[file];
				fds[file] = vfs_open(entry.filename, FS_WRITABLE | FS_READABLE | FS_CREATE);
			}

			uint32_t wrong = inspect_fs(files);
			total_wrong += wrong;
			if (wrong != 0) {
				;
			}
			printf("after unmount inspect: %u\n", wrong);
			printf("=============== UNMOUNT/REMOUNT OVER ===============\n\n");
		}
	}

	if (error_code == 0) {
		uint32_t wrong = inspect_fs(files);
		total_wrong += wrong;
		printf("final inspect: %u\n", wrong);
	}

cleanup:
	for (uint32_t i = 0; i < FILE_COUNT; i++) {
		free(files[i].filename);
		free(files[i].content);
	}

	lfs_unmount();

	if (total_wrong == 0 && error_code == 0)
		return 0;

	if (total_wrong == 0 && error_code == FS_ENOSPC)
		return 0;

	if (error_code == 0) {
		fprintf(stderr, "\nseed %ld, wrong %u\n", seed, total_wrong);
		return total_wrong == 0 ? 0 : 1;
	}

	fprintf(stderr, "\nseed %ld, error code %u\n", seed, error_code);
	return error_code ? 0 : 1;
}

