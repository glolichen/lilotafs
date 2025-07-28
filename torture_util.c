#include <asm-generic/errno-base.h>
#include <unistd.h>
#ifdef LILOTAFS_LOCAL
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
#include "lilotafs.h"

extern jmp_buf lfs_mount_jmp_buf;

uint32_t alternate_content_file = UINT32_MAX;
int alternate_content_size = 0; 
uint8_t *alternate_content = NULL;

// returns the number of files that are wrong
uint32_t inspect_fs(struct lilotafs_context *ctx, struct table_entry *files, uint32_t op) {
	PRINTF("INSPECT OP #%u\n", op);

	uint32_t total_wrong = 0;
	for (uint32_t i = 0; i < FILE_COUNT; i++) {
		if (!files[i].opened)
			continue;

		char *filename = files[i].filename;
		PRINTF("inspect: opening file %u: %.63s\n", i, filename);

		int fd = lilotafs_open(ctx, filename, O_RDONLY, 0);
		if (fd == -1) {
			PRINTF("inspect: file %s: can't open, error %d\n", filename, lilotafs_errno(ctx));
			total_wrong++;
			continue;
		}

		int actual_size = lilotafs_lseek(ctx, fd, 0, SEEK_END);
		lilotafs_lseek(ctx, fd, 0, SEEK_SET);

		uint8_t *file_content = (uint8_t *) calloc(actual_size, sizeof(uint8_t));
		if (!file_content) {
			PRINTF("inspect: file %s: malloc fail\n", filename);
			total_wrong++;
			lilotafs_close(ctx, fd);
			continue;
		}

		int read_bytes = lilotafs_read(ctx, fd, file_content, actual_size);
		if (read_bytes == -1) {
			PRINTF("inspect: file %s: read fail\n", filename);
			total_wrong++;
			free(file_content);
			lilotafs_close(ctx, fd);
			continue;
		}


		if (alternate_content_file == i) {
			bool correct = false;
			if (actual_size == files[i].content_size) {
				if (actual_size != 0) {
					uint32_t cmp = memcmp(file_content, files[i].content, actual_size);
					if (cmp == 0)
						correct = true;
				}
			}
			if (actual_size == alternate_content_size) {
				if (actual_size != 0) {
					uint32_t cmp = memcmp(file_content, alternate_content, actual_size);
					if (cmp == 0) {
						correct = true;
						files[i].content_size = actual_size;
						files[i].content = (uint8_t *) realloc(files[i].content, actual_size);
						memcpy(files[i].content, alternate_content, actual_size);
					}
				}
			}

			if (!correct) {
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
				lilotafs_close(ctx, fd);
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

		lilotafs_close(ctx, fd);
		free(file_content);
	}

	return total_wrong;
}

uint32_t torture(const char *disk_name, uint64_t random_seed) {
	int disk = open(disk_name, O_RDWR);
	uint64_t seed = random_seed == 0 ? time(NULL) : random_seed;

	fprintf(stdout, "random seed: %ld\n", seed);
	srand(seed);

	struct lilotafs_context ctx;
	memset(&ctx, 0, sizeof(struct lilotafs_context));

	PRINTF("mount: %d\n", lilotafs_mount(&ctx, TOTAL_SIZE, disk)); 

	struct table_entry files[FILE_COUNT];
	memset(files, 0, sizeof(files));

	for (uint32_t i = 0; i < FILE_COUNT; i++) {
		uint32_t filename_len = RANDOM_NUMBER(10, 63);
		files[i].filename = (char *) malloc(filename_len + 1);
		for (uint32_t j = 0; j < filename_len; j++) {
			do {
				files[i].filename[j] = RANDOM_NUMBER(33, 126);
			} while (files[i].filename[j] == '/');
		}
		files[i].filename[filename_len] = 0;

		files[i].opened = false;
		files[i].content_size = 0;
		files[i].content = NULL;
	}

	lilotafs_flash_set_crash(CRASH_WRITE_MIN_MOVES, CRASH_WRITE_MAX_MOVES, CRASH_ERASE_MIN_MOVES, CRASH_ERASE_MAX_MOVES);

	int total_wrong = 0, error_code = 0;
	for (uint32_t op = 0; op < OP_COUNT; op++) {
		uint32_t file = RANDOM_NUMBER(0, FILE_COUNT - 1);
		
		int random_size = RANDOM_NUMBER(MIN_SIZE, MAX_SIZE);
		uint8_t *random = (uint8_t *) malloc(random_size);
		if (!random) {
			PRINTF("main loop: file %s: malloc fail\n", files[file].filename);
			error_code = 1;
			goto cleanup;
		}

		for (int i = 0; i < random_size; i++)
			random[i] = RANDOM_NUMBER(0, 255);

		if (op == 101)
			printf("\n");

		int fd = lilotafs_open(&ctx, files[file].filename, O_WRONLY | O_CREAT, 0);
		if (fd == -1) {
			if (lilotafs_errno(&ctx) == LILOTAFS_ENOSPC) {
				PRINTF("\n\n=============== UNMOUNT/REMOUNT ===============\n");

				int remount_code = lilotafs_unmount(&ctx);
				if (remount_code != LILOTAFS_SUCCESS) {
					PRINTF("wear level: file %s: unmount error %d\n", files[file].filename, remount_code);
					error_code = remount_code;
					free(random);
					goto cleanup;
				}

				remount_code = lilotafs_mount(&ctx, TOTAL_SIZE, disk); 
				if (remount_code != LILOTAFS_SUCCESS) {
					PRINTF("wear level: file %s: remount error %d\n", files[file].filename, remount_code);
					error_code = remount_code;
					free(random);
					goto cleanup;
				}

				uint32_t wrong = inspect_fs(&ctx, files, op);
				total_wrong += wrong;
				PRINTF("after mount inspect: %u\n", wrong);

				PRINTF("=============== UNMOUNT/REMOUNT OVER ===============\n\n");

				free(random);
				continue;
			}

			error_code = lilotafs_errno(&ctx);
			PRINTF("main loop: file %s: open error %d\n", files[file].filename, error_code);
			free(random);
			goto cleanup;
		}

		int code;
		PRINTF("main loop: step %u\n", op);

		if (setjmp(lfs_mount_jmp_buf) == 0)
			code = lilotafs_write(&ctx, fd, random, random_size);
		else {
			// simulated crash will long jump back to here
			// simulate computer restart by unmounting and remounting
			PRINTF("\n\n=============== SIMULATED CRASH ===============\n");
			PRINTF("crash inject: random = %p\n", random);

			lilotafs_unmount(&ctx);

			// lilotafs_flash_set_crash(CRASH_WRITE_MIN_MOVES, CRASH_WRITE_MAX_MOVES, CRASH_ERASE_MIN_MOVES, CRASH_ERASE_MAX_MOVES);
			lilotafs_mount(&ctx, TOTAL_SIZE, disk); 

			alternate_content_file = file;
			alternate_content_size = random_size; 
			alternate_content = random;

			uint32_t wrong = inspect_fs(&ctx, files, op);
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

		int error = (code == random_size) ? LILOTAFS_SUCCESS : lilotafs_errno(&ctx);

		int close_error = lilotafs_close(&ctx, fd);
		if (close_error) {
			PRINTF("main loop: file %s: close error %d\n", files[file].filename, close_error);
			error_code = close_error;
			free(random);
			goto cleanup;
		}

		if (error == LILOTAFS_SUCCESS) {
			files[file].opened = true;
			files[file].content_size = random_size;
			files[file].content = (uint8_t *) realloc(files[file].content, random_size);
			if (random_size != 0)
				memcpy(files[file].content, random, random_size);
			PRINTF("main loop: file %s, step %u: write %u bytes\n", files[file].filename, op, random_size);
		}

#ifdef TORTURE_REMOUNT
		if (error == LILOTAFS_ENOSPC || (error == LILOTAFS_SUCCESS && op % OPS_PER_REMOUNT == 0 && op != 0)) {
			PRINTF("\n\n=============== UNMOUNT/REMOUNT ===============\n");

			int remount_code = lilotafs_unmount(&ctx);
			if (remount_code != LILOTAFS_SUCCESS) {
				PRINTF("wear level: file %s: unmount error %d\n", files[file].filename, remount_code);
				error_code = remount_code;
				free(random);
				goto cleanup;
			}

			remount_code = lilotafs_mount(&ctx, TOTAL_SIZE, disk); 
			if (remount_code != LILOTAFS_SUCCESS) {
				PRINTF("wear level: file %s: remount error %d\n", files[file].filename, remount_code);
				error_code = remount_code;
				free(random);
				goto cleanup;
			}

			uint32_t wrong = inspect_fs(&ctx, files, op);
			total_wrong += wrong;
			PRINTF("after mount inspect: %u\n", wrong);

			PRINTF("=============== UNMOUNT/REMOUNT OVER ===============\n\n");
		}

		if (error != LILOTAFS_SUCCESS && error != LILOTAFS_ENOSPC) {
			PRINTF("main loop: file %s: error %d\n", files[file].filename, error);
			error_code = error;
			free(random);
			goto cleanup;
		}
#else
		if (error != LILOTAFS_SUCCESS) {
			PRINTF("main loop: file %s: error %d\n", files[file].filename, error);
			error_code = error;
			free(random);
			goto cleanup;
		}
#endif

		free(random);
	}

	uint32_t wrong = inspect_fs(&ctx, files, UINT32_MAX);
	PRINTF("final inspect: %u\n", wrong);
	total_wrong += wrong;

	PRINTF("ERROR CODE: %d\n", error_code);
	PRINTF("FINAL WRONG TOTAL: %u\n", total_wrong);
	PRINTF("SEED: %lu\n", seed);

cleanup:
	for (uint32_t i = 0; i < FILE_COUNT; i++) {
		free(files[i].filename);
		if (files[i].content)
			free(files[i].content);
	}

	lilotafs_flash_clear_crash();
	lilotafs_unmount(&ctx);

	if (!error_code && !total_wrong)
		return 0;

	fprintf(stderr, "SEED: %lu\n", seed);
	fprintf(stderr, "ERROR CODE: %d\n", error_code);
	fprintf(stderr, "FINAL WRONG TOTAL: %d\n", total_wrong);
	if (error_code)
		return error_code;
	if (total_wrong)
		return total_wrong;

	return 1;
}

#endif
