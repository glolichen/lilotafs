#ifndef FLASH_H
#define FLASH_H

#include <stdio.h>
#include <stdint.h>

#include "lilotafs.h"

#define FLASH_OK 0
#define FLASH_FILE_IO_ERROR 101
#define FLASH_MALLOC_ERROR 102
#define FLASH_OUT_OF_BOUNDS 103

#ifdef LILOTAFS_LOCAL
void lilotafs_flash_clear_crash(void);
void lilotafs_flash_set_crash(uint32_t write_min, uint32_t write_max, uint32_t erase_min, uint32_t erase_max);
#else
#endif

uint32_t lilotafs_flash_get_partition_size(struct lilotafs_context *ctx);
uint32_t lilotafs_flash_flush(struct lilotafs_context *ctx, uint32_t address, uint32_t size);

int lilotafs_flash_write(struct lilotafs_context *ctx, const void *buffer, uint32_t address, uint32_t length);
int lilotafs_flash_erase_region(struct lilotafs_context *ctx, uint32_t start, uint32_t len);
int lilotafs_flash_read(struct lilotafs_context *ctx, void *buffer, uint32_t address, uint32_t length);

#endif

