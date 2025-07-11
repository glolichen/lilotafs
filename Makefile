# 2 MiB
TOTAL_SIZE = 2097152
# 4 KiB
SECTOR_SIZE = 4096

CC=gcc
CFLAGS=-I. -D TOTAL_SIZE=$(TOTAL_SIZE) -D SECTOR_SIZE=$(SECTOR_SIZE) \
	   -Wall -Wextra -fsanitize=undefined -static-libasan -g
OBJ = lilotaFS.o flash.o

build: $(OBJ) test.o
	$(CC) -o lilotaFS $^ $(CFLAGS)

gen_test: $(OBJ) gen_test_disk.o
	dd if=/dev/zero bs=$(TOTAL_SIZE) count=1 | tr '\000' '\377' > disk.bin
	$(CC) -o gen_test_disk $^ $(CFLAGS)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

new_disk:
	dd if=/dev/zero bs=$(TOTAL_SIZE) count=1 | tr '\000' '\377' > disk.bin

clean:
	rm lilotaFS gen_test_disk *.o

