#ifdef LILOTAFS_LOCAL
// #ifndef LILOTAFS_LOCAL

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#include "lilotafs.h"

char *remove_prefix(const char *filename, int prefix_length) {
	int abs_strlen = strlen(filename);
	int rel_strlen = abs_strlen - prefix_length;
	char *rel_filename = (char *) malloc((rel_strlen + 1) * sizeof(char));
	for (int i = 0; i < rel_strlen; i++)
		rel_filename[i] = filename[prefix_length + i];
	rel_filename[rel_strlen] = 0;
	return rel_filename;
}

int main(int argc, char *argv[]) {
	if (argc < 3)
		return 1;

	const char *disk_name = argv[1];
	int disk = open(disk_name, O_RDWR);

	struct lilotafs_context ctx;
	memset(&ctx, 0, sizeof(struct lilotafs_context));

	int err = lilotafs_mount(&ctx, DISK_SIZE, disk);
	ctx.has_wear_marker = true;

	if (err != LILOTAFS_SUCCESS)
		return err;
	
	printf("formatting disk %s, size %d\n", disk_name, DISK_SIZE);
	
	int prefix_length = strlen(argv[2]);
	struct stat info;
	for (int i = 3; i < argc; i++) {
		const char *filename = argv[i];
		char *rel_filename = remove_prefix(filename, prefix_length);

		stat(filename, &info);
		// check for directory
		if (!S_ISREG(info.st_mode)) {
			// skip "." directory
			if (strlen(rel_filename) == 0) {
				free(rel_filename);
				continue;
			}

			err = lilotafs_mkdir(&ctx, rel_filename, 0);
			if (err != 0) {
				free(rel_filename);
				printf("2 ERROR %d\n", lilotafs_errno(&ctx));
				return 2;
			}

			printf("    adding directory %s %lu\n", rel_filename, strlen(rel_filename));
			free(rel_filename);
			continue;
		}

		printf("    adding file %s, size %ld\n", rel_filename, info.st_size);

		// printf("    head 0x%x, tail 0x%x, largest file %u\n",
		// 	lilotafs_get_head(&ctx),
		// 	lilotafs_get_tail(&ctx),
		// 	lilotafs_get_largest_file_size(&ctx)
		// );

		uint8_t *file_data = (uint8_t *) calloc(info.st_size, sizeof(uint8_t));

		FILE *fp = fopen(filename, "rb");
		fread(file_data, info.st_size, 1, fp);

		int lilotafs_fd = lilotafs_open(&ctx, rel_filename, O_WRONLY | O_CREAT, 0);
		if (lilotafs_fd == -1) {
			fclose(fp);
			free(rel_filename);
			free(file_data);
			printf("3 ERROR %d\n", lilotafs_errno(&ctx));
			return 3;
		}
		
		ssize_t written = lilotafs_write(&ctx, lilotafs_fd, file_data, info.st_size);
		if (written != info.st_size) {
			fclose(fp);
			free(rel_filename);
			free(file_data);
			printf("4 ERROR %d\n", lilotafs_errno(&ctx));
			return 4;
		}
		
		lilotafs_close(&ctx, lilotafs_fd);
		fclose(fp);
		free(rel_filename);
		free(file_data);
	}
	
	err = lilotafs_unmount(&ctx);
	if (err != LILOTAFS_SUCCESS)
		return err;

	printf("generating image complete!\n");

	return 0;
}
#endif
