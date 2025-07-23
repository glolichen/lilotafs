#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>

// #define PRINT_STUFF
// #define PRINT_FREE

#define FILE_COUNT 60
#define OP_COUNT 4000

#define MIN_SIZE 5
#define MAX_SIZE 9000

// #define MIN_SIZE 0
// #define MAX_SIZE 0

#define OPS_PER_REMOUNT 250

#define CRASH_INJECT

#define CRASH_WRITE_MIN_MOVES 100
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
#define PRINTF(format, ...) printf(format, ##__VA_ARGS__)
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
