#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "util.h"
#include "flash.h"
#include "lilotaFS.h"

jmp_buf lfs_mount_jmp_buf;

uint32_t write_moves_remaining = 0;
uint32_t erase_moves_remaining = 0;

void flash_set_crash(uint32_t write_min, uint32_t write_max, uint32_t erase_min, uint32_t erase_max) {
	write_moves_remaining = RANDOM_NUMBER(write_min, write_max) + 1;
	erase_moves_remaining = RANDOM_NUMBER(erase_min, erase_max) + 1;
}

uint32_t flash_get_total_size() {
	return TOTAL_SIZE;
}
uint32_t flash_get_sector_size() {
	return SECTOR_SIZE;
}

// return 1 = trying to write unerased flash (i.e. 0 to 1)
int flash_write(uint8_t *mmap, const void *buffer, uint32_t address, uint32_t length) {
	if (address >= flash_get_total_size()) {
		PRINTF("out of bounds: %u\n", address);
		return FLASH_OUT_OF_BOUNDS;
	}

	// PRINTF("flash: writing %u bytes to 0x%x\n", length, address);

	// check if operation is legal (no changing 0 to 1)
	uint8_t *write = (uint8_t *) buffer;
	for (uint32_t i = 0; i < length; i++) {
		uint8_t current = mmap[address + i];
		uint8_t byte = write[i] | current;
		if (byte ^ current) {
			PRINTF("FLASH WRITE FAILED: address 0x%x\n", address);
			return 1;
		}
	}

#ifdef CRASH_INJECT
	for (uint32_t i = 0; i < length; i++) {
		if (write_moves_remaining != 0 && write_moves_remaining-- == 1) {
			PRINTF("WRITE CRASH\n");
			PRINTF("call: write len %u to address 0x%x\n", length, address);
			PRINTF("crash before writing 0x%x to 0x%x\n", write[i], address + i);
			longjmp(lfs_mount_jmp_buf, 1);
		}
		*(mmap + address + i) = write[i];
	}
#else
	memcpy(mmap + address, buffer, length);
#endif

	return FLASH_OK;
}

int flash_erase_region(uint8_t *mmap, uint32_t start, uint32_t len) {
	uint32_t total_size = flash_get_total_size();
	uint32_t sector_size = flash_get_sector_size();
	if (start % sector_size != 0 || len % sector_size != 0)
		return FLASH_OUT_OF_BOUNDS;
	if (start > total_size || start + len > total_size)
		return FLASH_OUT_OF_BOUNDS;

#ifdef CRASH_INJECT
	for (uint32_t i = 0; i < len; i++) {
		if (erase_moves_remaining != 0 && erase_moves_remaining-- == 1)
			longjmp(lfs_mount_jmp_buf, 1);
		*(mmap + start + i) = 0xFF;
	}
#else
	memset(mmap + start, 0xFF, len);
#endif

	return FLASH_OK;
}

