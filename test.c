#include "flash.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "lilotaFS.h"

// NOTE: testing utility commands:
// W [fd] [length]        | write random data of length [length] to file [fd]
// w [fd] [length]        | write only 0xFF of length [length] to file [fd]
// d [fd]                 | delete file [fd]
// c [fd]                 | close file [fd]
// o [create?] [filename] | open file with name [filename], choose whether to create
// q                      | query filesystem for information


int main(int argc, char *argv[]) {
	srand(time(NULL));

	if (argc < 2) {
		printf("error: must enter arguments\n");
		return 1;
	}

	printf("total size %d, sector size %d\n", TOTAL_SIZE, SECTOR_SIZE);

	char *disk_name = argv[1];
	int disk = open(disk_name, O_RDWR);

	struct lilotafs_context ctx;
	memset(&ctx, 0, sizeof(struct lilotafs_context));

	lilotafs_test_set_file(&ctx, disk);
	printf("mount: %u\n", lilotafs_mount(&ctx));

	char input[101];
	char *token;

	char mode;
	uint32_t fd;
	uint32_t flag;
	char filename[64];
	bool success;

	printf("\n");
	while (1) {
		mode = 0, fd = 0, flag = 0, success = true;
		memset(filename, 0, 64);

		printf("> ");
		fgets(input, 100, stdin);

		for (uint32_t i = 0; i < strlen(input); i++) {
			if (input[i] == '\n') {
				input[i] = 0;
				break;
			}
		}

		token = strtok(input, " ");
		for (uint32_t i = 0; token != NULL; i++) {
			if (i == 0) {
				mode = input[0];
				if (mode != 'o' && mode != 'r' && mode != 'w' && mode != 'd' && mode != 'c' && mode != 'q' && mode != 'W') {
					success = false;
					break;
				}
			}

			if (i == 1 && mode != 'q') {
				if (mode == 'o')
					flag = strtoul(token, NULL, 0);
				else
					fd = strtoul(token, NULL, 0);
			}

			if (i == 2 && mode != 'o' && mode != 'c' && mode != 'q')
				flag = strtoul(token, NULL, 0);

			if (i >= 2 && mode == 'o') {
				if (strlen(filename) + strlen(token) > 63) {
					success = false;
					break;
				}
				strcat(filename, token);
			}

			token = strtok(NULL, " ");
		}

		if (!success) {
			printf("error\n");
			continue;
		}

		// printf("mode = %c\n", mode);
		// printf("fd = %u\n", fd);
		// printf("flag = %u\n", flag);
		// printf("filename = %.63s\n", filename);

		if (mode == 'o') {
			uint32_t ret_fd = lilotafs_open(&ctx, filename, LILOTAFS_READABLE | LILOTAFS_WRITABLE | (flag ? LILOTAFS_CREATE : 0));
			printf("open file %.63s fd: %u\n", filename, ret_fd);
		}
		else if (mode == 'c') {
			uint32_t result = lilotafs_close(&ctx, fd);
			printf("close fd %u: %u\n", fd, result);
		}
		else if (mode == 'd') {
			uint32_t result = lilotafs_delete(&ctx, fd);
			printf("delete fd %u: %u\n", fd, result);

		}
		else if (mode == 'w' || mode == 'W') {
			uint32_t size = flag;
			uint8_t *data = (uint8_t *) malloc(size);
			for (uint32_t i = 0; i < size; i++)
				data[i] = mode == 'W' ? RANDOM_NUMBER(0, 255) : 0xFF;
			uint32_t result = lilotafs_write(&ctx, fd, data, size);
			printf("write %u bytes to fd %u: %u\n", size, fd, result);
			free(data);
		}
		else if (mode == 'q') {
			uint32_t count = lilotafs_count_files(&ctx);
			uint32_t largest_file = lilotafs_get_largest_file_size(&ctx);
			uint32_t largest_filename_len = lilotafs_get_largest_filename_len(&ctx);
			uint32_t head = lilotafs_get_head(&ctx);
			uint32_t tail = lilotafs_get_tail(&ctx);
			printf("number of files: %u\n", count);
			printf("largest file: %u\n", largest_file);
			printf("largest filename len: %u\n", largest_filename_len);
			printf("head: 0x%x\n", head);
			printf("tail: 0x%x\n", tail);
		}
		else if (mode == 'r') {
			printf("TODO\n");
		}
	}

	
	// uint32_t fd = vfs_open("lilota", LILOTAFS_WRITABLE | LILOTAFS_READABLE);
	// uint32_t fd = vfs_open("test", LILOTAFS_WRITABLE | LILOTAFS_READABLE | LILOTAFS_CREATE);
	// printf("fd: %d\n", fd);
	// if (fd == UINT32_MAX) {
	// 	close(disk);
	// 	return 1;
	// }
	//
	// printf("largest file size: %d\n", vfs_get_largest_file_size());

	// write test
	// char *content = "lilota FS test";
	// uint32_t result = vfs_write(fd, content, strlen(content));
	// printf("write result: %d\n", result);

	// char *content2 = "Lorem ipsum dolor sit amet, consectetur adipiscing elit";
	// result = vfs_write(fd, content2, strlen(content2));
	// printf("%d\n", result);

	// char *content2 = (char *) malloc(301);
	// result = vfs_write(fd, content2, 301);
	// printf("%d\n", result);

	// read test
	// uint32_t file_size = vfs_get_size(fd);
	// printf("size: %d\n", file_size);
	// char *buffer = (char *) malloc(file_size);
	// vfs_read(fd, buffer, 2, file_size - 5);
	// for (uint32_t i = 0; i < file_size - 5; i++)
	// 	printf("%c", buffer[i]);
	// printf("\n");
	// free(buffer);

	// delete test
	// uint32_t delete_result = vfs_delete(fd);
	// printf("delete: %d\n", delete_result);
	//
	// printf("close: %d\n", vfs_close(fd));
	
	// uint8_t buffer[4];
	// flash_read(buffer, 0x118, 4);

	// for (int i = 0; i < 4; i++)
	// 	printf("%02X\n", buffer[i]);

	close(disk);

	return 0;
}
