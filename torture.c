#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "torture_util.h"
#include "util.h"

int main(int argc, char *argv[]) {
	if (argc < 2) {
		PRINTF("error: must enter arguments\n");
		return 1;
	}

	char *disk_name = argv[1];

	uint64_t seed;
	if (argc == 3)
		seed = strtoul(argv[2], NULL, 0);
	else
		seed = time(NULL);

	uint32_t code = torture(disk_name, seed);
	return code;
}

