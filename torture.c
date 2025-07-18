#include <linux/limits.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>

#include "util.h"
#include "flash.h"
#include "lilotaFS.h"

extern jmp_buf lfs_mount_jmp_buf;

struct table_entry {
	char *filename;
	uint8_t *content;
	uint8_t *alternate_content;
};

uint32_t inspect_fs(struct table_entry *files) {
	PRINTF("========== INSPECT ==========\n");
	uint32_t wrong_count = 0;
	for (uint32_t i = 0; i < FILE_COUNT; i++) {
		struct table_entry *entry = &files[i];
		if (!entry->content) {
			PRINTF("NOTE: %.63s (%u) empty\n", entry->filename, i);
			continue;
		}
		uint32_t fd = vfs_open(entry->filename, FS_READABLE | FS_WRITABLE);
		if (fd == UINT32_MAX) {
			PRINTF("WRONG: %.63s (%u) -- fd == -1, error %u\n", entry->filename, i, vfs_open_errno());
			wrong_count++;
			continue;
		}
		uint32_t size = vfs_get_size(fd);
		uint8_t *file_content = (uint8_t *) malloc(size);
		if (file_content == NULL) {
			PRINTF("WRONG: %.63s (%u) -- malloc fail\n", entry->filename, i);
			wrong_count++;
			vfs_close(fd);
			continue;
		}
		if (vfs_read(fd, file_content, 0, size) != FS_SUCCESS) {
			PRINTF("WRONG: %.63s (%u) -- read fail\n", entry->filename, i);
			wrong_count++;
			free(file_content);
			vfs_close(fd);
			continue;
		}
		bool wrong = false;
		uint32_t normal = memcmp(file_content, entry->content, size);
		if (entry->alternate_content != NULL) {
			uint32_t alternate = memcmp(file_content, entry->alternate_content, size);
			if (normal != 0 && alternate == 0) {
				free(entry->content);
				entry->content = entry->alternate_content;
				entry->alternate_content = 0;
			}
		}
		free(file_content);
		vfs_close(fd);
		if (wrong) {
			PRINTF("WRONG: %.63s (%u) -- content wrong\n", entry->filename, i);
			wrong_count++;
			continue;
		}
		PRINTF("CORRECT: %.63s (%u)\n", entry->filename, i);
	}

	uint32_t file_count = lfs_count_files();
	if (file_count != FILE_COUNT)
		PRINTF("# of files wrong: LFS reports %u\n", file_count);

	PRINTF("========== INSPECT OVER ==========\n");

	return wrong_count;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		PRINTF("error: must enter arguments\n");
		return 1;
	}

	PRINTF("total size %d, sector size %d\n", TOTAL_SIZE, SECTOR_SIZE);

	char *disk_name = argv[1];
	int disk = open(disk_name, O_RDWR);

	uint64_t seed;
	if (argc == 3)
		seed = strtoul(argv[2], NULL, 0);
	else
		seed = time(NULL);

	PRINTF("random seed: %ld\n", seed);
	srand(seed);

	lfs_set_file(disk);

	PRINTF("mount: %u\n", lfs_mount());

	struct table_entry files[FILE_COUNT];
	memset(files, 0, sizeof(files));
	uint32_t fds[FILE_COUNT];

	for (uint32_t file = 0; file < FILE_COUNT; file++) {
		uint32_t filename_len = RANDOM_NUMBER(10, 63);

		struct table_entry entry = {
			.filename = (char *) calloc(filename_len + 1, sizeof(char)),
			.content = NULL,
			.alternate_content = NULL
		};
		for (uint32_t i = 0; i < filename_len; i++)
			entry.filename[i] = RANDOM_NUMBER(33, 126);
		files[file] = entry;

		fds[file] = vfs_open(entry.filename, FS_WRITABLE | FS_READABLE | FS_CREATE);
		if (fds[file] == (uint32_t) -1) {
			PRINTF("open failed: %u\n", vfs_open_errno());
			goto cleanup;
		}
		
		PRINTF("file %u = %.63s\n", file, entry.filename);
	}

	static bool has_set_jmp = false;
	static uint32_t total_wrong = 0, error_code = 0;
	for (static uint32_t op = 0; op < OP_COUNT; op++) {
		uint32_t file = RANDOM_NUMBER(0, FILE_COUNT - 1);
		struct table_entry *entry = &files[file];

		PRINTF("op #%u\n", op);

		uint32_t size = RANDOM_NUMBER(MIN_SIZE, MAX_SIZE);
		uint8_t *random = (uint8_t *) malloc(size);
		if (!random) {
			fprintf(stderr, "malloc failed\n");
			goto cleanup;
		}

		for (uint32_t i = 0; i < size; i++)
			random[i] = RANDOM_NUMBER(0, 255);

		uint32_t code = vfs_write(fds[file], random, size);
		if (setjmp(lfs_mount_jmp_buf)) {
			// simulated crash will long jump back to here
			// simulate computer restart by unmounting and remounting
			PRINTF("\n\n=============== SIMULATED CRASH ===============\n");
			flash_set_crash(CRASH_WRITE_MIN_MOVES, CRASH_WRITE_MAX_MOVES, CRASH_ERASE_MIN_MOVES, CRASH_ERASE_MAX_MOVES);

			lfs_unmount();
			lfs_set_file(disk);
			lfs_mount();

			for (uint32_t file = 0; file < FILE_COUNT; file++) {
				struct table_entry entry2 = files[file];
				fds[file] = vfs_open(entry2.filename, FS_WRITABLE | FS_READABLE | FS_CREATE);
				if (fds[file] == (uint32_t) (-1)) {
					error_code = vfs_open_errno();
					PRINTF("error opening %s\n", entry2.filename);
					free(random);
					goto cleanup;
				}
				PRINTF("file %u = %.63s\n", file, entry2.filename);
			};

			entry->alternate_content = random;
			PRINTF("inspection: %u\n", inspect_fs(files));
			entry->alternate_content = NULL;
			if (entry->content != random)
				free(random);

			PRINTF("=============== RECOVERED FROM CRASH ===============\n\n");

			continue;
			// exit(1);
		}
		if (!has_set_jmp) {
			has_set_jmp = true;
			flash_set_crash(CRASH_WRITE_MIN_MOVES, CRASH_WRITE_MAX_MOVES, CRASH_ERASE_MIN_MOVES, CRASH_ERASE_MAX_MOVES);
		}
		if (code == FS_SUCCESS) {
			if (entry->content)
				FREE(entry->content);
			entry->content = random;
		}

		if (code != FS_SUCCESS && code != FS_ENOSPC) {
			FREE(random);
			PRINTF("write error: %u\n", code);
			error_code = code;
			goto cleanup;
		}

#ifdef OPS_PER_REMOUNT
		if (code == FS_ENOSPC || (op % OPS_PER_REMOUNT == 0 && op != 0)) {
#else
		if (code == FS_ENOSPC) {
			PRINTF("no space, inspect: %u\n", inspect_fs(files));
			exit(1);
#endif
			PRINTF("\n\n=============== UNMOUNT/REMOUNT ===============\n");
			if (code == FS_SUCCESS) {
				uint32_t wrong = inspect_fs(files);
				total_wrong += wrong;
				if (wrong != 0) {
					;
				}
				PRINTF("before unmount inspect: %u\n", wrong);
			}
			else
				PRINTF("out of space!\n");

			PRINTF("\nunmount: %u\n", lfs_unmount());
			lfs_set_file(disk);
			uint32_t mount_code = lfs_mount();
			if (mount_code != FS_SUCCESS) {
				PRINTF("mount error: %u\n", code);
				error_code = code;
				goto cleanup;
			}

			for (uint32_t file = 0; file < FILE_COUNT; file++) {
				struct table_entry entry = files[file];
				fds[file] = vfs_open(entry.filename, FS_WRITABLE | FS_READABLE | FS_CREATE);
				if (fds[file] == (uint32_t) (-1)) {
					error_code = vfs_open_errno();
					PRINTF("error opening %s\n", entry.filename);
					free(random);
					goto cleanup;
				}
			}

			uint32_t wrong = inspect_fs(files);
			total_wrong += wrong;
			if (wrong != 0) {
				;
			}
			PRINTF("after unmount inspect: %u\n", wrong);
			PRINTF("=============== UNMOUNT/REMOUNT OVER ===============\n\n");
		}
	}

	if (error_code == 0) {
		uint32_t wrong = inspect_fs(files);
		total_wrong += wrong;
		printf("final inspect: %u\n", wrong);
	}

cleanup:
	for (uint32_t i = 0; i < FILE_COUNT; i++) {
		if (files[i].filename)
			free(files[i].filename);
		if (files[i].content)
			free(files[i].content);
	}

	lfs_unmount();

	if (total_wrong == 0 && error_code == 0)
		return 0;

	// if (total_wrong == 0 && error_code == FS_ENOSPC)
	// 	return 0;

	if (error_code == 0) {
		fprintf(stderr, "\nseed %ld, wrong %u\n", seed, total_wrong);
		return total_wrong == 0 ? 0 : 1;
	}

	fprintf(stderr, "\nseed %ld, error code %u\n", seed, error_code);
	return error_code ? 0 : 1;
}

