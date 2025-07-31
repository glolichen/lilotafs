# 2 MiB
TOTAL_SIZE = 2097152
# TOTAL_SIZE = 8192
# 4 KiB
SECTOR_SIZE = 4096

# CC=gcc
CC=clang
CFLAGS=-I. -D TOTAL_SIZE=$(TOTAL_SIZE) -D SECTOR_SIZE=$(SECTOR_SIZE) -D LILOTAFS_LOCAL \
		-Wall -Wextra -g -O0 \
		-fsanitize=address,undefined -static-libasan

OBJ = lilotafs.o flash.o torture_util.o

all: clean test torture

test: $(OBJ) test.o
	$(CC) -o test $^ $(CFLAGS)

torture: $(OBJ) torture.o
	$(CC) -o torture $^ $(CFLAGS)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

new_disk:
	@dd if=/dev/zero bs=$(TOTAL_SIZE) count=1 | tr '\000' '\377' > disk.bin

clean:
	# rm test gen_test_disk torture *.o
	rm -f test torture *.o

