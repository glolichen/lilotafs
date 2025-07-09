#include "flash.h"
#include <stdio.h>

int main(int argc, char *argv[]) {
	if (argc < 2) {
		printf("error: must enter arguments\n");
		return 1;
	}

	printf("total size %d, sector size %d\n", TOTAL_SIZE, SECTOR_SIZE);

	char *disk_name = argv[1];
	FILE *disk = fopen(disk_name, "rb+");

	// flash_erase_region(disk, 4096, 4096);
	
	uint8_t test[] = {0x9a, 0x21, 0xaa, 0xc7};
	printf("%d\n", flash_write(disk, test, 0x118, 4));

	fclose(disk);

	return 0;
}
