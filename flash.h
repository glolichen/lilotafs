#ifndef FLASH_H
#define FLASH_H

#include <stdio.h>
#include <stdint.h>

#define FLASH_OK 0
#define FLASH_FILE_IO_ERROR 101
#define FLASH_MALLOC_ERROR 102
#define FLASH_OUT_OF_BOUNDS 103

uint32_t flash_get_total_size();
uint32_t flash_get_sector_size();

int flash_write(uint8_t *mmap, const void *buffer, uint32_t address, uint32_t length);
int flash_erase_region(uint8_t *mmap, uint32_t start, uint32_t len);

#endif

