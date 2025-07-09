#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "flash.h"

#define FREE_RETURN(mem, val) { free(mem); return val; }

uint32_t get_flash_total_size() {
	return TOTAL_SIZE;
}
uint32_t get_flash_sector_size() {
	return SECTOR_SIZE;
}

int flash_read(FILE *fd, void *buffer, uint32_t address, uint32_t length) {
	if (address >= get_flash_total_size())
		return FLASH_OUT_OF_BOUNDS;

	if (fseek(fd, address, SEEK_SET))
		return FLASH_FILE_IO_ERROR;

	if (fread(buffer, 1, length, fd) != length)
		return FLASH_FILE_IO_ERROR;

	return FLASH_OK;
}

// return 1 = trying to write unerased flash (i.e. 0 to 1)
int flash_write(FILE *fd, const void *buffer, uint32_t address, uint32_t length) {
	if (address >= get_flash_total_size())
		return FLASH_OUT_OF_BOUNDS;

	if (fseek(fd, address, SEEK_SET))
		return FLASH_FILE_IO_ERROR;

	uint8_t *current = (uint8_t *) malloc(length);
	if (!current)
		return FLASH_MALLOC_ERROR;
	if (fread(current, 1, length, fd) != length)
		FREE_RETURN(current, FLASH_FILE_IO_ERROR);

	// check if operation is legal (no changing 0 to 1)
	uint8_t *write = (uint8_t *) buffer;
	for (uint32_t i = 0; i < length; i++) {
		uint8_t byte = write[i] | current[i];
		if (byte ^ current[i])
			FREE_RETURN(current, 1);
	}

	free(current);

	if (fseek(fd, address, SEEK_SET))
		return FLASH_FILE_IO_ERROR;
	if (fwrite(write, 1, length, fd) != length)
		return FLASH_FILE_IO_ERROR;

	return FLASH_OK;
}

int flash_erase_region(FILE *fd, uint32_t start, uint32_t len) {
	uint32_t total_size = get_flash_total_size();
	uint32_t sector_size = get_flash_sector_size();
	if (start % sector_size != 0 || len % sector_size != 0)
		return FLASH_OUT_OF_BOUNDS;
	if (start >= total_size || start + len >= total_size)
		return FLASH_OUT_OF_BOUNDS;

	uint8_t *ones = (uint8_t *) malloc(len);
	if (!ones)
		return FLASH_MALLOC_ERROR;
	memset(ones, 0xFF, len);

	if (fseek(fd, start, SEEK_SET))
		FREE_RETURN(ones, FLASH_FILE_IO_ERROR);
	if (fwrite(ones, 1, len, fd) != len)
		FREE_RETURN(ones, FLASH_FILE_IO_ERROR);

	free(ones);
	return FLASH_OK;
}

int flash_erase_chip(FILE *fd) {
	return flash_erase_region(fd, 0, get_flash_total_size());
}

