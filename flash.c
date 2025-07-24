#ifdef LILOTAFS_LOCAL

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

// return 1 = trying to write unerased flash (i.e. 0 to 1)
int lilotafs_flash_write(struct lilotafs_context *ctx, const void *buffer, uint32_t address, uint32_t length) {
	if (address + length > ctx->partition_size) {
		PRINTF("out of bounds: %u\n", address);
		return FLASH_OUT_OF_BOUNDS;
	}

	// PRINTF("flash: writing %u bytes to 0x%x\n", length, address);

	// check if operation is legal (no changing 0 to 1)
	uint8_t *write = (uint8_t *) buffer;
	for (uint32_t i = 0; i < length; i++) {
		uint8_t current = ctx->flash_mmap[address + i];
		uint8_t byte = write[i] | current;
		if (byte ^ current) {
			PRINTF("FLASH WRITE FAILED: address 0x%x\n", address);
			return 1;
		}
	}

	for (uint32_t i = 0; i < length; i++) {
#ifdef CRASH_INJECT
		if (write_moves_remaining != 0 && write_moves_remaining-- == 1) {
			PRINTF("WRITE CRASH\n");
			PRINTF("call: write len %u to address 0x%x\n", length, address);
			PRINTF("crash before writing 0x%x to 0x%x\n", write[i], address + i);
			longjmp(lfs_mount_jmp_buf, 1);
		}
#endif
		*(ctx->flash_mmap + address + i) = write[i];
	}

	return FLASH_OK;
}

int lilotafs_flash_erase_region(struct lilotafs_context *ctx, uint32_t start, uint32_t len) {
	uint32_t total_size = ctx->partition_size;
	uint32_t sector_size = ctx->flash_sector_size;
	if (start % sector_size != 0 || len % sector_size != 0)
		return FLASH_OUT_OF_BOUNDS;
	if (start > total_size || start + len > total_size)
		return FLASH_OUT_OF_BOUNDS;

	for (uint32_t i = 0; i < len; i++) {
#ifdef CRASH_INJECT
		if (erase_moves_remaining != 0 && erase_moves_remaining-- == 1)
			longjmp(lfs_mount_jmp_buf, 1);
#endif
		*(ctx->flash_mmap + start + i) = 0xFF;
	}

	return FLASH_OK;
}

#else

#include <stdint.h>
#include "lilotaFS.h"

int lilotafs_flash_write(struct lilotafs_context *ctx, const void *buffer, uint32_t address, uint32_t length) {
	if (address + length > ctx->partition->size)
		return 1;

	// check if operation is legal (no changing 0 to 1)
	uint8_t *write = (uint8_t *) buffer;
	for (uint32_t i = 0; i < length; i++) {
		uint8_t current = ctx->flash_mmap[address + i];
		uint8_t byte = write[i] | current;
		if (byte ^ current)
			return 1;
	}
	
	assert(0);

	int err = esp_partition_write(ctx->partition, address, buffer, length);
	if (err != ESP_OK)
		return 1;
	
	return 0;
}

int lilotafs_flash_erase_region(struct lilotafs_context *ctx, uint32_t start, uint32_t len) {
	uint32_t total_size = ctx->partition->size;
	uint32_t sector_size = ctx->flash_sector_size;
	if (start % sector_size != 0 || len % sector_size != 0)
		return 1;
	if (start > total_size || start + len > total_size)
		return 1;

	int err = esp_partition_erase_range(ctx->partition, start, len);
	if (err != ESP_OK)
		return 1;

	return 0;
}

#endif
