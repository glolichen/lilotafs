#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>

#define PRINT_STUFF
#define PRINT_FREE

#define FILE_COUNT 5
#define OP_COUNT 10000

// #define STOP_ON_OP 3468

#define MIN_SIZE 40
#define MAX_SIZE 5000

// #define MIN_SIZE 0
// #define MAX_SIZE 0

#define OPS_PER_REMOUNT 300

#define CRASH_INJECT
#define TORTURE_REMOUNT

#define CRASH_WRITE_MIN_MOVES 50
#define CRASH_WRITE_MAX_MOVES 100000
// #define CRASH_WRITE_MIN_MOVES UINT32_MAX
// #define CRASH_WRITE_MAX_MOVES UINT32_MAX

// #define CRASH_ERASE_MIN_MOVES 10000
// #define CRASH_ERASE_MAX_MOVES 1000000
#define CRASH_ERASE_MIN_MOVES UINT32_MAX
#define CRASH_ERASE_MAX_MOVES UINT32_MAX


// -------------------------------------------------------------------


#define RANDOM_NUMBER(min, max) (rand() % ((max) - (min) + 1) + (min))
#ifdef PRINT_STUFF
#ifdef LILOTAFS_LOCAL
#define PRINTF(format, ...) fprintf(stdout, format, ##__VA_ARGS__)
// #define PRINTF(format, ...) fprintf(stderr, format, ##__VA_ARGS__)
#else
#define PRINTF(format, ...) printf("lilotafs: " format, ##__VA_ARGS__)
#endif
#else
inline static uint32_t evaluate_all(const char *fmt, ...) {
	(void) fmt;
	return 0;
}
#define PRINTF(format, ...) evaluate_all(format, ##__VA_ARGS__)
#endif

#ifdef PRINT_FREE
#define FREE(mem) { \
	printf("free memory %p (variable %s), line %d\n", mem, #mem, __LINE__); \
	free(mem); \
}
#else
#define FREE(mem) { \
	if (mem) { \
		free(mem); \
		mem = NULL; \
	} \
}
#endif

#endif
