# 2 MiB
TOTAL_SIZE = 2097152
# TOTAL_SIZE = 8192
# 4 KiB
SECTOR_SIZE = 4096

CC=gcc
CFLAGS=-I. -D TOTAL_SIZE=$(TOTAL_SIZE) -D SECTOR_SIZE=$(SECTOR_SIZE) \
	   -Wall -Wextra -fsanitize=undefined -static-libasan -g
OBJ = lilotaFS.o flash.o

all: test torture

test: $(OBJ) test.o
	$(CC) -o test $^ $(CFLAGS)

torture: $(OBJ) torture.o
	$(CC) -o torture $^ $(CFLAGS)

# gen_test: $(OBJ) gen_test_disk.o
# 	dd if=/dev/zero bs=$(TOTAL_SIZE) count=1 | tr '\000' '\377' > disk.bin
# 	$(CC) -o gen_test_disk $^ $(CFLAGS)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

new_disk:
	dd if=/dev/zero bs=$(TOTAL_SIZE) count=1 | tr '\000' '\377' > disk.bin

clean:
	# rm test gen_test_disk torture *.o
	rm test torture *.o

