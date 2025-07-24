#ifndef TORTURE_UTIL_H
#define TORTURE_UTIL_H

#include <stdint.h>
#include <stdbool.h>

struct table_entry {
	char *filename;
	bool opened;
	int content_size;
	uint8_t *content;
};

uint32_t torture(const char *disk_name, uint64_t random_seed);

#endif
