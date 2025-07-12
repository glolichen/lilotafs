// #include <stdint.h>
// #include <stdio.h>
// #include <sys/mman.h>
// #include <fcntl.h>
// #include <unistd.h>
// #include <time.h>
// #include <stdlib.h>
// #include <string.h>
//
// #define FILE_SIZE_MIN 20
// #define FILE_SIZE_MAX 200
// #define LOW_FILE_COUNT 0
// #define HIGH_FILE_COUNT 8
// #define GAP_LENGTH_MIN 100
// #define GAP_LENGTH_MAX 400
//
// #include "flash.h"
// #include "lilotaFS.h"
//
// // inclusive of both endpoints
// uint32_t random_number(uint32_t min, uint32_t max) {
// 	return ((double) rand() / RAND_MAX) * (max - min) + min;
// }
//
// uint32_t place_file(uint8_t *flash_mmap, uint32_t cur_offset, uint16_t magic) {
// 	uint32_t data_len = random_number(FILE_SIZE_MIN, FILE_SIZE_MAX);
// 	printf("%d\n", data_len);
//
// 	struct fs_rec_header header = {
// 		.magic = magic,
// 		.status = STATUS_COMMITTED,
// 		.data_len = data_len,
// 	};
//
// 	uint32_t filename_len = random_number(8, 16);
// 	char *filename = (char *) malloc((filename_len + 1) * sizeof(char));
// 	for (uint32_t i = 0; i < filename_len; i++)
// 		filename[i] = random_number(33, 126);
// 	filename[filename_len] = 0;
//
// 	memcpy(flash_mmap + cur_offset, &header, sizeof(struct fs_rec_header));
// 	cur_offset += sizeof(struct fs_rec_header);
//
// 	memcpy(flash_mmap + cur_offset, filename, (filename_len + 1) * sizeof(char));
// 	cur_offset += (filename_len + 1) * sizeof(char);
//
// 	cur_offset += data_len;
// 	cur_offset = ALIGN_UP(cur_offset);
//
// 	return cur_offset;
// }
//
// int main(int argc, char *argv[]) {
// 	srand(time(NULL));
//
// 	if (argc < 2) {
// 		printf("error: must enter arguments\n");
// 		return 1;
// 	}
//
// 	printf("total size %d, sector size %d\n", TOTAL_SIZE, SECTOR_SIZE);
//
// 	char *disk_name = argv[1];
// 	int disk = open(disk_name, O_RDWR);
//
// 	uint32_t partition_size = flash_get_total_size();
// 	uint8_t *flash_mmap = mmap(NULL, partition_size, PROT_READ | PROT_WRITE, MAP_SHARED, disk, 0);
// 	if (flash_mmap == MAP_FAILED)
// 		return FLASH_FILE_IO_ERROR;
//
// 	uint32_t cur_offset = 0;
// 	for (uint i = 0; i < LOW_FILE_COUNT; i++)
// 		cur_offset = place_file(flash_mmap, cur_offset, FS_RECORD);
//
// 	uint32_t gap_length = random_number(GAP_LENGTH_MIN, GAP_LENGTH_MAX);
// 	memset(flash_mmap + cur_offset, 0xFF, gap_length);
// 	cur_offset += gap_length;
// 	cur_offset = ALIGN_UP(cur_offset);
//
// 	cur_offset = place_file(flash_mmap, cur_offset, FS_START);
// 	for (uint i = 0; i < HIGH_FILE_COUNT - 1; i++)
// 		cur_offset = place_file(flash_mmap, cur_offset, FS_RECORD);
//
// 	close(disk);
//
// 	return 0;
// }
