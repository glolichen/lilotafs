#include "flash.h"

#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "lilotaFS.h"

int main(int argc, char *argv[]) {
	if (argc < 2) {
		printf("error: must enter arguments\n");
		return 1;
	}

	printf("total size %d, sector size %d\n", TOTAL_SIZE, SECTOR_SIZE);

	char *disk_name = argv[1];
	int disk = open(disk_name, O_RDWR);

	lfs_set_file(disk);
	lfs_mount();
	
	// uint32_t fd = vfs_open("lilota", FS_WRITABLE | FS_READABLE);
	uint32_t fd = vfs_open("test", FS_WRITABLE | FS_READABLE | FS_CREATE);
	printf("fd: %d\n", fd);
	if (!fd) {
		close(disk);
		return 1;
	}

	printf("largest file size: %d\n", vfs_get_largest_file_size());

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

	printf("largest file size: %d\n", vfs_get_largest_file_size());

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
	// vfs_delete(fd);

	printf("close: %d\n", vfs_close(fd));
	
	// uint8_t buffer[4];
	// flash_read(buffer, 0x118, 4);

	// for (int i = 0; i < 4; i++)
	// 	printf("%02X\n", buffer[i]);

	close(disk);

	return 0;
}
