#include <linux/limits.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>

#include "lilotaFS.h"

#define FILE_COUNT 1
#define OP_COUNT 198

#define MIN_SIZE 500
#define MAX_SIZE 20000

struct table_entry {
	char *filename;
	uint8_t *content;
};

uint32_t random_number(uint32_t min, uint32_t max) {
	return ((double) rand() / RAND_MAX) * (max - min) + min;
}

uint32_t inspect_fs(struct table_entry *files) {
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
	return wrong_count;
}

int main(int argc, char *argv[]) {
	if (argc < 3) {
		printf("error: must enter arguments\n");
		return 1;
	}

	printf("total size %d, sector size %d\n", TOTAL_SIZE, SECTOR_SIZE);

	char *disk_name = argv[1];
	int disk = open(disk_name, O_RDWR);

	srand(strtoul(argv[2], NULL, 0));

	lfs_set_file(disk);

	printf("mount: %u\n", lfs_mount());

	struct table_entry files[FILE_COUNT];
	uint32_t fds[FILE_COUNT];

	for (uint32_t file = 0; file < FILE_COUNT; file++) {
		uint32_t filename_len = random_number(10, 63);

		struct table_entry entry = {
			.filename = (char *) calloc(filename_len + 1, sizeof(char)),
			.content = NULL
		};
		for (uint32_t i = 0; i < filename_len; i++)
			entry.filename[i] = random_number(33, 126);
		files[file] = entry;

		fds[file] = vfs_open(entry.filename, FS_WRITABLE | FS_READABLE | FS_CREATE);
		
		printf("file %u = %.63s\n", file, entry.filename);
	}

	for (uint32_t op = 0; op < OP_COUNT; op++) {
		uint32_t file = random_number(0, FILE_COUNT - 1);
		uint32_t size = random_number(MIN_SIZE, MAX_SIZE);
		uint8_t *random = (uint8_t *) malloc(size);
		for (uint32_t i = 0; i < size; i++)
			random[i] = random_number(0, 255);

		uint32_t code = vfs_write(fds[file], random, size);
		if (code) {
			printf("write error: %u\n", code);
			goto cleanup;
		}

		struct table_entry *entry = &files[file];
		if (entry->content)
			free(entry->content);
		entry->content = random;
	}

	printf("%u\n", inspect_fs(files));

cleanup:
	for (uint32_t i = 0; i < FILE_COUNT; i++) {
		free(files[i].filename);
		free(files[i].content);
	}

	lfs_unmount();

	return 0;
}

