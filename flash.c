#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "flash.h"
#include "lilotafs.h"

#ifdef LILOTAFS_LOCAL
#include <sys/mman.h>

jmp_buf lfs_mount_jmp_buf;

uint32_t write_moves_remaining = 0;
uint32_t erase_moves_remaining = 0;


void lilotafs_flash_clear_crash(void) {
	write_moves_remaining = 0;
	erase_moves_remaining = 0;
}
void lilotafs_flash_set_crash(uint32_t write_min, uint32_t write_max, uint32_t erase_min, uint32_t erase_max) {
	write_moves_remaining = RANDOM_NUMBER(write_min, write_max) + 1;
	erase_moves_remaining = RANDOM_NUMBER(erase_min, erase_max) + 1;
}
#else
#include "esp_flash.h"
#endif

uint32_t lilotafs_flash_get_partition_size(struct lilotafs_context *ctx) {
#ifdef LILOTAFS_LOCAL
	return ctx->partition_size;
#else
	return ctx->partition->size;
#endif
}

uint32_t lilotafs_flash_flush(struct lilotafs_context *ctx, uint32_t address, uint32_t size) {
#ifdef LILOTAFS_LOCAL
	(void) ctx;
	(void) address;
	(void) size;
	return 0;
#else
	spi_flash_host_inst_t *host = ctx->partition->flash_chip->host;
	esp_err_t (*flush_cache)(spi_flash_host_inst_t *host, uint32_t addr, uint32_t size) = host->driver->flush_cache;
	if (!flush_cache)
		return LILOTAFS_SUCCESS;
	esp_err_t err = (*flush_cache)(host, address, size);
	if (err != ESP_OK)
		return err;
	return LILOTAFS_SUCCESS;
#endif
}

int lilotafs_flash_write(struct lilotafs_context *ctx, const void *buffer, uint32_t address, uint32_t length) {
	if (address + length > lilotafs_flash_get_partition_size(ctx))
		return 1;
	
	// check if operation is legal (no changing 0 to 1)
	uint8_t *write = (uint8_t *) buffer;
	for (uint32_t i = 0; i < length; i++) {
		uint8_t current = ctx->flash_mmap[address + i];
		uint8_t byte = write[i] | current;
		if (byte ^ current) {
#ifdef LILOTAFS_LOCAL
			fprintf(stderr, "flash write fail: write 0x%x to 0x%x\n", write[i], address + i);
			exit(1);
#endif
			return 1;
		}
	}

#ifdef LILOTAFS_LOCAL
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
#else
	int err = esp_partition_write(ctx->partition, address, buffer, length);
	if (err != ESP_OK)
		return 1;

	if (lilotafs_flash_flush(ctx, 0, lilotafs_flash_get_partition_size(ctx)))
		return LILOTAFS_EFLASH;
#endif

	return 0;
}

int lilotafs_flash_erase_region(struct lilotafs_context *ctx, uint32_t start, uint32_t len) {
	uint32_t total_size = lilotafs_flash_get_partition_size(ctx);
	uint32_t sector_size = ctx->block_size;
	if (start % sector_size != 0 || len % sector_size != 0)
		return 1;
	if (start > total_size || start + len > total_size)
		return 1;
	
#ifdef LILOTAFS_LOCAL
	for (uint32_t i = 0; i < len; i++) {
#ifdef CRASH_INJECT
		if (erase_moves_remaining != 0 && erase_moves_remaining-- == 1)
			longjmp(lfs_mount_jmp_buf, 1);
#endif
		*(ctx->flash_mmap + start + i) = 0xFF;
	}
#else
	int err = esp_partition_erase_range(ctx->partition, start, len);
	if (err != ESP_OK)
		return 1;

	if (lilotafs_flash_flush(ctx, 0, lilotafs_flash_get_partition_size(ctx)))
		return LILOTAFS_EFLASH;
#endif

	return 0;
}
