#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "flash.h"

#define FREE_RETURN(mem, val) { free(mem); return val; }

uint32_t flash_get_total_size() {
	return TOTAL_SIZE;
}
uint32_t flash_get_sector_size() {
	return SECTOR_SIZE;
}

// return 1 = trying to write unerased flash (i.e. 0 to 1)
int flash_write(uint8_t *mmap, const void *buffer, uint32_t address, uint32_t length) {
	if (address >= flash_get_total_size()) {
		printf("out of bounds: %u\n", address);
		return FLASH_OUT_OF_BOUNDS;
	}

	// check if operation is legal (no changing 0 to 1)
	uint8_t *write = (uint8_t *) buffer;
	for (uint32_t i = 0; i < length; i++) {
		uint8_t current = mmap[address + i];
		uint8_t byte = write[i] | current;
		if (byte ^ current)
			return 1;
	}

	memcpy(mmap + address, buffer, length);

	return FLASH_OK;
}

int flash_erase_region(uint8_t *mmap, uint32_t start, uint32_t len) {
	uint32_t total_size = flash_get_total_size();
	uint32_t sector_size = flash_get_sector_size();
	if (start % sector_size != 0 || len % sector_size != 0)
		return FLASH_OUT_OF_BOUNDS;
	if (start >= total_size || start + len >= total_size)
		return FLASH_OUT_OF_BOUNDS;

	memset(mmap + start, 0xFF, len);

	return FLASH_OK;
}

