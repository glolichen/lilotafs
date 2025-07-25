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
void flash_set_crash(uint32_t write_min, uint32_t write_max, uint32_t erase_min, uint32_t erase_max);
#endif

int lilotafs_flash_write(struct lilotafs_context *ctx, const void *buffer, uint32_t address, uint32_t length);
int lilotafs_flash_erase_region(struct lilotafs_context *ctx, uint32_t start, uint32_t len);

#endif

