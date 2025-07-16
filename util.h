#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>

#define RANDOM_NUMBER(min, max) (rand() % ((max) - (min) + 1) + (min))

#define FILE_COUNT 1
#define OP_COUNT 20000

#define MIN_SIZE 5
#define MAX_SIZE 20

#define OPS_PER_REMOUNT 200

#define CRASH_INJECT

// #define CRASH_WRITE_MIN_MOVES 100
// #define CRASH_WRITE_MAX_MOVES 1000000
#define CRASH_WRITE_MIN_MOVES UINT32_MAX
#define CRASH_WRITE_MAX_MOVES UINT32_MAX

// #define CRASH_ERASE_MIN_MOVES 10000
// #define CRASH_ERASE_MAX_MOVES 1000000
#define CRASH_ERASE_MIN_MOVES UINT32_MAX
#define CRASH_ERASE_MAX_MOVES UINT32_MAX

#endif
