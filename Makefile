# 2 MiB
TOTAL_SIZE = 2097152
# 4 KiB
SECTOR_SIZE = 4096

CC=gcc
CFLAGS=-I. -D TOTAL_SIZE=$(TOTAL_SIZE) -D SECTOR_SIZE=$(SECTOR_SIZE) \
	   -Wall -Wextra
OBJ = main.o flash.o

build: $(OBJ)
	$(CC) -o lilotaFS $^ $(CFLAGS)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

new_disk:
	dd if=/dev/zero bs=$(TOTAL_SIZE) count=1 | tr '\000' '\377' > disk.bin

clean:
	rm lilotaFS *.o

