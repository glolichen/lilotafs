#include "torture_util.h"

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

uint32_t alternate_content_file = UINT32_MAX;
uint32_t alternate_content_size = 0; 
uint8_t *alternate_content = NULL;

// returns the number of files that are wrong
uint32_t inspect_fs(struct lilotafs_context *ctx, struct table_entry *files) {
	uint32_t total_wrong = 0;
	for (uint32_t i = 0; i < FILE_COUNT; i++) {
		char *filename = files[i].filename;

		uint32_t fd = lilotafs_open(ctx, filename, LILOTAFS_READABLE | LILOTAFS_WRITABLE);
		if (fd == (uint32_t) -1) {
			PRINTF("inspect: file %s: can't open\n", filename);
			total_wrong++;
			continue;
		}

		uint32_t actual_size = lilotafs_get_size(ctx, fd);
		uint8_t *file_content = (uint8_t *) calloc(actual_size, sizeof(uint8_t));
		if (!file_content) {
			PRINTF("inspect: file %s: malloc fail\n", filename);
			total_wrong++;
			continue;
		}

		uint32_t code = lilotafs_read(ctx, fd, file_content, 0, actual_size);
		if (code != LILOTAFS_SUCCESS) {
			PRINTF("inspect: file %s: read fail\n", filename);
			total_wrong++;
			free(file_content);
			continue;
		}


		if (alternate_content_file == i) {
			if (actual_size == files[i].content_size) {
				if (actual_size != 0) {
					uint32_t cmp = memcmp(file_content, files[i].content, actual_size);
					if (cmp != 0) {
						PRINTF("inspect: file %s: content wrong\n", filename);
						total_wrong++;
					}
				}
			}
			else if (actual_size == alternate_content_size) {
				if (actual_size != 0) {
					uint32_t cmp = memcmp(file_content, alternate_content, actual_size);
					if (cmp != 0) {
						PRINTF("inspect: file %s: content wrong\n", filename);
						total_wrong++;
						free(file_content);
						continue;
					}
					files[i].content_size = actual_size;
					files[i].content = (uint8_t *) realloc(files[i].content, actual_size);
					memcpy(files[i].content, alternate_content, actual_size);
				}
			}
			else {
				PRINTF("inspect: file %s: size wrong: got %u, expected %u or %u\n",
						filename, actual_size, files[i].content_size, alternate_content_size);
				total_wrong++;
			}
		}
		else {
			if (actual_size != files[i].content_size) {
				PRINTF("inspect: file %s: size wrong: got %u, expected %u\n", filename, actual_size, files[i].content_size);
				total_wrong++;
				free(file_content);
				continue;
			}
			if (actual_size != 0) {
				uint32_t cmp = memcmp(file_content, files[i].content, actual_size);
				if (cmp != 0) {
					PRINTF("inspect: file %s: content wrong\n", filename);
					total_wrong++;
				}
			}
		}

		free(file_content);
	}

	return total_wrong;
}

uint32_t torture(const char *disk_name, uint64_t random_seed) {
	int disk = open(disk_name, O_RDWR);
	uint64_t seed = random_seed == 0 ? time(NULL) : random_seed;

	PRINTF("random seed: %ld\n", seed);
	srand(seed);

	struct lilotafs_context ctx;
	memset(&ctx, 0, sizeof(struct lilotafs_context));

	lilotafs_test_set_file(&ctx, disk);

	PRINTF("mount: %u\n", lilotafs_mount(&ctx));

	struct table_entry files[FILE_COUNT];
	uint32_t fds[FILE_COUNT];
	memset(files, 0, sizeof(files));
	memset(fds, 0, sizeof(fds));

	for (uint32_t i = 0; i < FILE_COUNT; i++) {
		uint32_t filename_len = RANDOM_NUMBER(10, 63);
		files[i].filename = (char *) malloc(filename_len + 1);
		for (uint32_t j = 0; j < filename_len; j++)
			files[i].filename[j] = RANDOM_NUMBER(33, 126);
		files[i].filename[filename_len] = 0;

		files[i].content_size = 0;
		files[i].content = NULL;

		uint32_t fd = lilotafs_open(&ctx, files[i].filename, LILOTAFS_WRITABLE | LILOTAFS_READABLE | LILOTAFS_CREATE);
		if (fd == (uint32_t) -1) {
			PRINTF("populate: file %s: can't open\n", files[i].filename);
			return 1;
		}
		fds[i] = fd;
	}

	flash_set_crash(CRASH_WRITE_MIN_MOVES, CRASH_WRITE_MAX_MOVES, CRASH_ERASE_MIN_MOVES, CRASH_ERASE_MAX_MOVES);

	uint32_t total_wrong = 0, error_code = 0;
	for (uint32_t op = 0; op < OP_COUNT; op++) {
		uint32_t file = RANDOM_NUMBER(0, FILE_COUNT - 1);
		uint32_t fd = fds[file];

		uint32_t random_size = RANDOM_NUMBER(MIN_SIZE, MAX_SIZE);
		uint8_t *random = (uint8_t *) malloc(random_size);
		if (!random) {
			PRINTF("main loop: file %s: malloc fail\n", files[file].filename);
			error_code = 1;
			goto cleanup;
		}

		for (uint32_t i = 0; i < random_size; i++)
			random[i] = RANDOM_NUMBER(0, 255);

		uint32_t code;
		PRINTF("main loop: step %u\n", op);

		if (op == 1526)
			printf("\n");

		if (setjmp(lfs_mount_jmp_buf) == 0)
			code = lilotafs_write(&ctx, fd, random, random_size);
		else {
			// simulated crash will long jump back to here
			// simulate computer restart by unmounting and remounting
			PRINTF("\n\n=============== SIMULATED CRASH ===============\n");
			PRINTF("crash inject: random = %p\n", random);

			flash_set_crash(CRASH_WRITE_MIN_MOVES, CRASH_WRITE_MAX_MOVES, CRASH_ERASE_MIN_MOVES, CRASH_ERASE_MAX_MOVES);

			lilotafs_unmount(&ctx);
			lilotafs_test_set_file(&ctx, disk);
			lilotafs_mount(&ctx);

			for (uint32_t i = 0; i < FILE_COUNT; i++) {
				uint32_t fd = lilotafs_open(&ctx, files[i].filename, LILOTAFS_WRITABLE | LILOTAFS_READABLE | LILOTAFS_CREATE);
				if (fd == (uint32_t) -1) {
					PRINTF("crash inject: file %s: can't open\n", files[i].filename);
					error_code = lilotafs_open_errno(&ctx);
					goto cleanup;
				}
				fds[i] = fd;
			}

			alternate_content_file = file;
			alternate_content_size = random_size; 
			alternate_content = random;

			uint32_t wrong = inspect_fs(&ctx, files);
			total_wrong += wrong;
			PRINTF("post crash inspect: %u\n", wrong);

			alternate_content_file = UINT32_MAX;
			alternate_content_size = 0; 
			alternate_content = NULL;

			free(random);

			PRINTF("=============== RECOVERED FROM CRASH ===============\n\n");

			continue;
			// exit(1);
		}



		if (code == LILOTAFS_SUCCESS) {
			files[file].content_size = random_size;
			files[file].content = (uint8_t *) realloc(files[file].content, random_size);
			if (random_size != 0)
				memcpy(files[file].content, random, random_size);
			PRINTF("main loop: file %s, step %u: write %u bytes\n", files[file].filename, op, random_size);
		}

		if (code == LILOTAFS_ENOSPC || (code == LILOTAFS_SUCCESS && op % OPS_PER_REMOUNT == 0 && op != 0)) {
			PRINTF("\n\n=============== UNMOUNT/REMOUNT ===============\n");

			uint32_t remount_code = lilotafs_unmount(&ctx);
			if (remount_code != LILOTAFS_SUCCESS) {
				PRINTF("wear level: file %s: unmount error %u\n", files[file].filename, remount_code);
				error_code = remount_code;
				free(random);
				goto cleanup;
			}

			lilotafs_test_set_file(&ctx, disk);

			remount_code = lilotafs_mount(&ctx);
			if (remount_code != LILOTAFS_SUCCESS) {
				PRINTF("wear level: file %s: unmount error %u\n", files[file].filename, remount_code);
				error_code = remount_code;
				free(random);
				goto cleanup;
			}

			for (uint32_t i = 0; i < FILE_COUNT; i++) {
				uint32_t fd = lilotafs_open(&ctx, files[i].filename, LILOTAFS_WRITABLE | LILOTAFS_READABLE | LILOTAFS_CREATE);
				if (fd == (uint32_t) -1) {
					PRINTF("wear level: file %s: can't open\n", files[i].filename);
					error_code = lilotafs_open_errno(&ctx);
					free(random);
					goto cleanup;
				}
				fds[i] = fd;
			}

			uint32_t wrong = inspect_fs(&ctx, files);
			total_wrong += wrong;
			PRINTF("after mount inspect: %u\n", wrong);

			PRINTF("=============== UNMOUNT/REMOUNT OVER ===============\n\n");
		}
		
		if (code != LILOTAFS_SUCCESS && code != LILOTAFS_ENOSPC) {
			PRINTF("main loop: file %s: error %u\n", files[file].filename, code);
			error_code = code;
			free(random);
			goto cleanup;
		}

		free(random);
	}

	uint32_t wrong = inspect_fs(&ctx, files);
	PRINTF("final inspect: %u\n", wrong);
	total_wrong += wrong;

	PRINTF("FINAL WRONG TOTAL: %u\n", total_wrong);
	PRINTF("SEED: %lu\n", seed);

cleanup:
	for (uint32_t i = 0; i < FILE_COUNT; i++) {
		free(files[i].filename);
		if (files[i].content)
			free(files[i].content);
	}

	lilotafs_unmount(&ctx);

	if (!error_code && !total_wrong)
		return 0;

	fprintf(stderr, "SEED: %lu\n", seed);
	if (error_code) {
		fprintf(stderr, "ERROR CODE: %u\n", error_code);
		return error_code;
	}
	if (total_wrong) {
		fprintf(stderr, "FINAL WRONG TOTAL: %u\n", total_wrong);
		return total_wrong;
	}

	return 1;
}

