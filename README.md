# Lilota Filesystem Documentatation

Jayden Li, Dr. Michael Ferdman

---

We design a file system with the following requirements:

 - Contiguous files: Lilota dynamic module loading requires files to be stored contiguously in flash.
 - Crash-recoverable: Embedded systems like esp32 may lose power at any time, and we must be able to recover from a crash event.
 - Support SPI flash: individual bits can only be set from 1 to 0 ("programming"). Only entire sectors, 4096 bytes on esp, can be set from 0 to 1 ("erasing").

## 0. Overview

Files are stored immediately next to each other. There is a head file, and a tail file. For any file, we can easily locate the next file by adding the file's data size, so we can think of the file system as a linked list.

To create a new file or modify an existing file, a new file will be appended at the tail, and in case of a modification, the old file will be tombstoned. The file writing process consists of progressively removing bits from the file's "status" byte, so that we can detect and recover from a crash that occurs while writing files.

In order to compact the file system and eliminate tombstoned files, we also have a wear leveling process. On mounting the file system, the first 5 files are moved to the tail and the head pointer is advanced. If we are able to, all sectors before the head pointer are erased, and every bit is set to 1.

There is no specialized data structure for directories. A file's directory is noted by storing its full absolute path, from which the disk's directory structure can then be constructed. The directory-related API functions work by performing string operations on a path.

We implement basic file API functions: `open`, `close`, `write`, `read`, `lseek`, `mkdir`, `opendir`, `readdir`, `closedir`.

## 1. Image Layout

Each file consists of a header, a null-terminated file name, and the file data.

```c
struct lilotafs_rec_header {
	uint16_t magic;
	uint8_t status;
	uint8_t __reserved;
	uint32_t data_len;
} __attribute__((packed));
```

Headers are 8 bytes long, storing a "magic number" indicating the file type, a "status" byte, an unused reserved (struct padding) byte to ensure alignment, and a 32 byte data size field indicating the size of file data. Headers are stored on eight byte aligned boundaries.

Headers are immediately followed by a null-terminated array of characters, which is the file name. The maximum file name length is 64 bytes, which is 63 characters plus a null terminator.

The next address that is a multiple of eight immediately following the end of the file name string null terminator is the start of file data.

It is important that each of header, file name and file data are stored on eight byte aligned boundaries. This is a requirement for executing a file: the file will be ``mmap``ed into instruction memory, and esp32 can only read instruction memory.

```
|------------|
|header 1    |
|------------|
|file name 1 |
|------------|
|file data 1 |
|            |
|            |
|------------|
|UNUSED      |
|------------|
|header 2    |
|------------|
|file name 2 |
|------------|
|UNUSED      |
|------------|
|file data 2 |
|            |
|------------|
```

### 1.1. Magic Number and Status

```c
enum lilotafs_record_status {
	LILOTAFS_STATUS_ERASED = 0xFF, // 11111111
	LILOTAFS_STATUS_RESERVED = 0xFE, // 11111110
	LILOTAFS_STATUS_COMMITTED = 0xFC, // 11111100
	LILOTAFS_STATUS_MIGRATING = 0xF8, // 11111000
	LILOTAFS_STATUS_WRAP_MARKER = 0xF0, // 11110000
	LILOTAFS_STATUS_WEAR_MARKER = 0xE0, // 11100000
	LILOTAFS_STATUS_DELETED = 0x00, // 00000000
};

enum lilotafs_magic {
	LILOTAFS_RECORD = 0x5AA5,
	LILOTAFS_WRAP_MARKER = 0x5AFA,
	LILOTAFS_START = 0x5A00,
	LILOTAFS_START_CLEAN = 0x5AA0
};
```

Regular files have magic number `LILOTAFS_RECORD = 0x5AA5`. The first file in the file system, the "head" of the "file linked list," is `LILOTAFS_START = 0x5A00`. `LILOTAFS_START_CLEAN` is used to facilitate recovery from a crash during wear leveling and `LILOTAFS_WRAP_MARKER` are for special wrap marker files.

### 1.2. Special Files

#### Wrap Marker

The wrap marker indicates there are no more files between the wrap marker and the end of flash. A wrap marker is used if there is no space at the end of flash storage to store a given file, and therefore, the next file is stored at the start of flash at offset `0`.

Wrap marker has status `LILOTAFS_STATUS_WRAP_MARKER = 0xF0` and magic number `LILOTAFS_WRAP_MARKER = 0x5AFA`. Notice that the lowest byte of the magic number in flash is `0xFA` (little endian), which is different from that of all other magic numbers. This is so that if we crash while writing the wrap marker, we can distinguish it from other records even if only one byte is written.

Unlike other files, wrap markers are only 8 bytes and consist of only a header. The file name is not written (no null terminator).

#### Wear Marker

A wear marker is a normal file. If a file is migrated/moved and copied to the tail, a wear marker is written to the file system, if there is not already a wear marker.

Wear marker has status `LILOTAFS_STATUS_WEAR_MARKER = 0xE0` and there is no special magic number (i.e., it will be `LILOTAFS_RECORD` or `LILOTAFS_START`). The file name is empty, so only a null terminator is written. As with normal files, if a wear marker is "active," it will have status `LILOTAFS_STATUS_COMMITTED = 0xFC`.

Due to the 8-byte alignment requirement for files, the total size of a wear marker is 16 bytes.

On mount, if an active wear marker (status `LILOTAFS_STATUS_COMMITTED`) is found, we will perform wear leveling, and the wear marker is deleted by setting its status to `LILOTAFS_STATUS_DELETED = 0`. Wear leveling is described in detail later.

## 2. Appending Files

The behavior of the file system, failure modes and crash recovery stem from the file writing process. This process is used whenever a file is appended to the tail, though there are additional steps in case we are copying an existing file ("updating"), which are shown in *italics*. This is implemented in the `append_file` function.

The file system tail pointer points to the 8-byte aligned address immediately following the last file. If possible, the new file will be written there.

 1. Check for free space ("Free Space Guarantee") and return `ENOSPC` if not met.
 2. *If updating, set the status of the existing file to `MIGRATING = 0xF8`*
 3. Check if there is sufficient space between the current file system tail and the end of partition to write the file. If not, add a wrap marker to the current position, and set the tail pointer to the start of the partition at address `0`.
 4. Write the file header at the tail pointer, with status `RESERVED = 0XFE` and data size is unchanged at `0xFFFFFFFF = -1`. This is because we do not yet know the final size of the file, as we should be able to process multiple `write` API calls.
 5. Write the file name and content.
 6. Change the status from the new file to `COMMITTED = 0XFC`.
 7. *If updating, "delete" the old file by setting its status to `DELETED = 0`*.

Below is what will happen if a crash were to occur after each of these seven steps.

 - If updating, crash after step 3 and before step 4: `MIGRATING` file present, no `RESERVED` file.
 - Crash after step 4 and before step 6: presence of `MIGRATING` depends on whether we are updating, `RESERVED` file is present. The file name may or may not be null terminated (depends on whether the crash happens after writing the null terminator), and some file data might be written.
 - If updating, crash after step 6 and before step 7: `MIGRATING` file present, no `RESERVED` file, `COMMITTED` file of the same name.

Crash recovery then occurs when mounting the file system, and the full process for recovery is explained in section 5.

## 3. Free Space Guarantee

If we are in the process of writing any file, we are to check whether we are able to add the **largest file in the file system** can be stored twice more (i.e. not counting the copy already stored), either directly or with a wrap marker.

If our file system looks like this, where `A` and `B` are files:
```
|=AAAAABBBBBBBBBB|B---------------|
```

and then file `A` were to be moved to the tail (i.e., after file `B`) due to wear leveling or user API calls, the free space before and after the file will become fragmented:
```
|======BBBBBBBBBB|BAAAAA----------|
```

We need to make sure both free variables, before (indicated with `=`) and after (indicated with `-`) the "main file list" are large enough to store the largest file. The easiest way to ensure this is to require that the largest file can be stored twice.

The largest file size for free space checking purposes is: `8 (file header size) + file name length with terminator + data size + 8 (space used by worst case align up)`.

Free space guarantee is implemented in `check_free_space`. This function works by finding the tail after appending the current file, and then attempts to append the largest file twice using the worst-case file size defined above. If doing so is not possible, we return `ENOSPC`.

## 4. API

A file can be opened by calling `open`. Valid file permissions are read-only (`O_RDONLY`), write-only (`O_WDONLY`) and write-only and create (`O_WDONLY | O_CREAT`). We do not support read-write files or appending.

A file descriptor is assigned on each `open` call and stored into the FD table, similar to POSIX. An offset into the file is stored in each FD. `open` with `O_RDONLY` will create a blank file, and the old file will be marked as `MIGRATING` and then deleted on `close`. To append to a file, read the content of the file by opening it `O_RDONLY`, close it, open it again with `O_WDONLY` and `write` the old contents first, before writing new content.

In read-only files, the offset may be set with `lseek`. Subsequent `read` calls will read starting at the offset and advance the offset based on the length of the read call.

In write-only files, the offset cannot be used as we can only append to the end of a file (we are not able to overwrite existing data as we cannot change 0 bits into 1). We can `write` multiple times to an open file descriptor; each `write` call will add data to the end of the file.

In read-only files, `close` does not need to do anything other than remove the corresponding entry from the FD table. In write-only files, `close` will commit the file, delete the old file (if we are updating), and then remove the FD entry.

API functions are designed to be similar to POSIX (as required by ESP-IDF), but is not POSIX compliant. The limitations of lilotafs design and of the flash interface mean that many file operations are not possible.

The error number `errno` can be retrieved by `lilotafs_errno` for the latest error.

### 4.1. `open`

```C
lilotafs_open(void *ctx, const char *name, int flags, int mode);
```

If opening for read, create a new FD with offset at the file start.

If opening for write, reserve a new file and, if modifying a file, store the positions of both the new and old file in flash to the FD table. The new file will have status `RESERVED` and the status of the old file is changed to `MIGRATING`

Returns the file descriptor on success. Returns `-1` on failure and `errno` is set to:

 - `EINVAL` if `name` is too long (greater than 63 characters, plus a null terminator).
 - `EINVAL` if `name` ends with a slash `'/'`, which is reserved for directories.
 - `EPERM` if `flags` is anything other than `O_RDONLY`, `O_WDONLY` or `O_WDONLY | O_CREAT`.
 - `EPERM` if attempting to open a file for write that is already open for write.
 - `ENOENT` if the directory the file is in does not exist.
 - `ENOENT` if the file does not exist and is not being opened in `O_WDONLY | O_CREAT`.
 - `ENOSPC` if no space on flash to reserve a new file (for write only).
 - `ENXIO` on failure to add to FD list.

### 4.2. `read`

```C
ssize_t lilotafs_read(void *ctx, int fd, void *buffer, size_t len);
```

Starting from the offset into file stored in the FD table, the first `len` bytes are copied into `buffer`. **The user must ensure `buffer` is larger than `len` bytes.** `len` is then added to the offset, so that the next time `read` is called, we will read immediately after what being read now.

Returns the number of bits read, or `-1` on failure with `errno` is set to:

 - `EPERM` if file was not opened for read.
 - `EBADF` if invalid file descriptor.

### 4.3. `lseek`

```C
off_t lilotafs_lseek(void *ctx, int fd, off_t offset, int whence);
```

Move the offset in file forward.

Returns the new offset, or `-1` on failure with `errno` is set to:

 - `EINVAL` if `whence` is invalid (not `SEEK_CUR`, `SEEK_SET` or `SEEK_END`).
 - `EPERM` if file was not opened for read.
 - `EBADF` if invalid file descriptor.

### 4.4. `write`

```C
ssize_t lilotafs_write(void *ctx, int fd, const void *buffer, unsigned int len);
```

Writes `len` bytes from `buffer` to the end of file. Add `len` to offset in file, since in files opened for write, the offset always points to the end of written data.

If there is no space left between the file and the end of flash, insert a wear marker, reserve a file at the start of flash (which is where the file system continues in case of a wrap marker) and copy the contents of the file being written into that reserved file.

In any case, the address of the file being written to is retrieved from the FD table. That file has status `RESERVED`. If a wrap marker is needed, the newly reserved file will also have status `RESERVED`.

Returns the number of bits written, or `-1` on failure with `errno` is set to:

 - `EIO` on any failure to write to flash.
 - `ENOSPC` if no space remaining to write data.
 - `EPERM` if file was not opened for write.
 - `EBADF` if invalid file descriptor.

### 4.5. `close`

```C
int lilotafs_close(void *ctx, int fd);
```

For all files, the FD associated with the file is removed from the FD table. For files opened for read, no further action is taken.

For files opened for write:

 - `data_len` of the new file is set (the size of data is equal to the offset in file, as every `write` call will advance the offset).
 - If an error occurred in writing data to the new file (determined by reading `write_errno` field in FD table), the old file is restored by copying it to the tail. Only if we are not creating a new file and updating an existing one.
 - If no error, commit the new file by setting its status to `COMMITTED` and delete the old file by setting its status to `DELETED`. Must be done in this order for crash recovery purposes (if delete and then commit, and there is a crash between those actions, the old file is lost).
 - If there is no wear marker, add one.

Returns `0` on success. Returns `-1` on failure with `errno` is set to:

 - `EIO` on any failure to write to flash.
 - `ENOSPC` if no space to add wear marker or restore old file.
 - `EBADF` if invalid file descriptor.

### 4.6. Directories

`mkdir`, `opendir`, `readdir`, `closedir` are also implemented. These are very straightforward.

## 5. Mount

Mounting the file system can be loosely organized into these steps:

 1. Locate the file system head, which is the file with magic number `LILOTAFS_START`. If we detect an anomaly as a result of a crash while or after scanning the disk for the head, we will perform recovery to address this. If a head is not found, create one.
 2. Parse the entire file system to find the tail.
 3. Recover from crash if we detect unwritten files.
 4. Perform wear leveling.

All crash recovery occurs on mounting the file system. The full mount process is described below in detail.

### 5.1. Locate or Create Head

Depending on the number of files and location of the head (file with magic `LILOTAFS_START`), there are a number of possible scenarios:

 1. `|--------------------------XXXXXAAAAAAAABBBCCCCCCCCCCCCCCC-------|`
 2. `|BBBBBBBBBBBBBBBBCC*-------------------------------XXXAAAAAAAW---|`
 3. `|XXXXXXXXXAAAABBBBBBBBBBBBB--------------------------------------|`

where:

 - `A`, `B`, `C`, `D` are regular files
 - `X` is the start/head file with magic number `LILOTAFS_START`
 - `W` is a wrap marker
 - `-`, free space

The basic algorithm to locate the head file (`X`) is:

 1. Check if there is a file at address `0`. If there is a file and it has magic `LILOTAFS_START`, we are done.
 2. If there is a file at address `0`, continuously advance the address until there are no more files. Because we know the file name length and data size and files are stored next to each other, if we have the address of the header of one file, we can locate the address of the header of the next file. Thus we can find the last valid file in the file chain (the address of `*` in case 2 above). This is performed by the function `scan_headers`.
 3. Scan the file system until we find the head. Since record headers are placed on 8-byte boundaries, we need to read two bytes (size of the magic number) on every 8-byte boundary and check if those two bytes are `LILOTAFS_START`. This is performed by `scan_for_header`.

(*Implementation note: `lilotafs_mount` will perform step 2 above even if there is no file at address `0`, but it will simply return address `0`, which is a no-op.*)

We also need to consider crash recovery in this process. The most difficult situation to address is this:

```
|BBBBBBBBBBBBB???|??????????======|================|==XXXAAAAAAAW---|
```

where `?` is a reserved file, `|` are flash erase boundaries and `=` is also free space.

`scan_headers` will return either the space directly after the last file, **or, the header of the last file in case the last file is a reserved file**. This happens if there is a crash before a file is committed, or if a file is `open`ed for read but not closed.

So this case, `scan_headers` will return the address of the first `?`. 

We cannot accurately determine the size of the reserved file (`?` file) because the `data_len` field may not be written yet, but can deduce when in the file writing process the crash occurred and recover accordingly.

 - If the file name is not null-terminated, a crash occurred while or before writing the file name, so we know no actual data has been written. Since the max file length is 64, we have an upper bound on the space taken up by this file.
 - If the file name is terminated, then we know the crash occurred while writing data. This is more challenging because we do not know how much data was written (the `data_len` field could be unmodified and equals `-1 = 0xFFFFFFFF`, but it may also not be if the crash occurred while writing the `data_len`). We have no upper bound on the file size (other than `UINT32_MAX`). We will erase **every** sector after the reserved file and before the head, which is done by this procedure:

     1. Calculate the new `data_len` of this file, which is whatever value places the end of the current file immediately before the next flash erase boundary, but do not write it to flash in case we crash before finishing the crash recovery process.
     2. Scan for a magic number `0x5a00` on an alignment boundary. **It is entirely possible that the `0x5a00` we find is not the actual head file, but rather the user attempted to write the bytes `0x00` and then `0x5a` to the file.** We will perform a sanity check to verify the magic number we found is an actual file. If one of these are not true, then we continue scanning.
         1. The status byte is valid.
         2. The file name is terminated with a zero (if the file is not a wrap marker).
         3. "Pretend" that file is the head file, and use the `scan_headers` function to verify if the file returned by `scan_header` is the reserved file. If it is not, the user attempted to write `0x5a00` to the disk but it is not actually the head.
     4. As we scan the file, if a sector does not contain the head with magic number `LILOTAFS_START`, erase it (set every bit in the sector to `1`).
     5. When we `LILOTAFS_START`, we are done. Write the reserved file's `data_len` field to the value calculated in step 1 (hereafter the "calculated file size"). Note that if the crash occurred while writing `data_len` to flash, we may not be able to write the calculated file size, if writing it into `data_len` requires changing a bit from `0` to `1`. We will find the smallest value greater than the calculated file size, such that writing it to `data_len` does not require changing any bits from `0` to `1`. The algorithm to calculate this value is:

```C
calculated_file_size--;
while (((++calculated_file_size) | current_data_len) ^ current_data_len);
```

Note it is possible to locate the wrong `LILOTAFS_START`, if the user is careful/unlucky enough to craft a file that defeats our sanity checks in step 2.

You may recall the `LILOTAFS_START_CLEAN` magic number, which is used for crash recovery. These are written during wear leveling so we will explain the process of recovering from `LILOTAFS_START_CLEAN` in that section.

If we were not able to find `LILOTAFS_START`, create one at address `0` in flash.

### 5.2. Parse Disk and Crash Recovery

We now know the address of the file system head. We can use the `scan_headers` function to find the tail (`scan_headers` can handle wrap markers correctly). It will return not only the address of the tail, which was used to locate the head, but also:

```C
struct scan_headers_result {
	uint32_t last_header, wear_marker, wrap_marker, migrating, reserved;
	uint32_t num_files;
};
```

There is no crash if and only if `scan_result.migrating = UINT32_MAX` and `scan_result.reserved = UINT32_MAX`. Otherwise, we must perform crash recovery.

If there is a reserved file (`scan_result.reservef != UINT32_MAX`), we clean up that up first. Similar to before, presence of a reserved file indicates a crash either while writing the file data (or the writing the `data_len` field to the record header during the commit process), or while writing the file name. Additionally, there may or may not be a `MIGRATING` file: if we were making creating a new file, no file is migrated, but there would be one otherwise (copying an existing file).

There are three cases for cleaning up a reserved file:

 - (a) Crash while writing file data, or while writing `data_len`:

     1. Locate the flash sector immediately above data start of the reserved file, which is achieved by rounding up that address to 4096 KiB.
     2. Erase sectors until we reach the end of flash, or we reach the head file.
     3. Within the same sector as the data start, find the last byte that is not FF. This is last byte of data that was written before the crash.
     4. Calculate the file size, if that byte were the last byte of the file.
     5. If the existing `data_len` in the record header is NOT `-1 = 0xFFFFFFFF`, in which case the crash occurred while writing `data_len`, increment file size until we can write it into `data_len`. The algorithm to do so is described earlier.
     6. Write `data_len` to the record header and delete the reserved file by setting its status to `LILOTAFS_STATUS_DELETED = 0`.
     7. *If there is migrating (`scan_result.migrating != UINT32_MAX`), the migrating file will be copied to the tail. This is done whenever there is a migrating file. We will describe how this is done later.*

 - (b) In case of a crash while writing the file name (the file name is not terminated with `\0`), recovery procedure differs depending on whether there is a file with status `MIGRATING` is present.

     1. If there is a migrating file, we know we were writing the file name of the migrating file into the reserved file, but a crash occurred during this process.

         1. Fill in the file name from the migrating file.
         2. Copy the content of the migrating file directly.
         3. Set the `data_len`.
         4. Commit the reserved file, and delete the migrating file.

     2.  There is no migrating file, we do not know the file name we were attempting to write when the crash occurred, so we must discard the reserved file.

         1. Locate the 64 byte space where file name should be (recalling that the maximum file name length is 63 characters, and the total space used would be 64 by including a null terminator).
         2. In the 64 byte space, find the first character that is not `0xFF`, which is the last character that was written before the crash.
         3. Set that byte to `0`, terminating the string.
         4. Calculate and write a `data_len` that is exactly the number of bytes between the null terminator and the start of the next file on a 8 byte aligned boundary. (*Note: I am not sure if this step makes sense and there might be a simple way to set the `data_len`. Please see `lilotafs.c` lines 1362-1383.*).
         5. Delete this file, since its file name is not what the user intended to write.

We are done processing the reserved file. The purpose of a `MIGRATING` file is to serve as a backup in case copying or modifying it (to the reserved file) fails. We cannot restore the migrating file by setting its status, as that requires changing a `0` byte to `1`, so we will copy it to the tail. We will perform this restoration in these cases

 - If there is no reserved file.
 - In case (a) for processing a reserved file: we crashed while writing the file data, so we have to restore what the contents of the file before the crash stored in the migrating file.

Case (b)(1) also copies the migrating file into the reserved file, but since the procedure outlined there will delete the migrating file, we will not consider that case here.

It is also possible that a crash occurs AFTER committing a reserved file, but BEFORE the migrating file is deleted. There will be both a `COMMITTED` and `MIGRATING` file of the same name. In this case, we only need to delete the `MIGRATING` file.

In other cases, the `MIGRATING` file is copied by copying its header, file name and data to the tail.

### 5.3. Wear Leveling

Whenever a file is deleted, and there is not already a committed wear marker, a wear marker is appended to the file system.

On boot, if a committed wear marker is found, wear leveling is triggered. Starting from the head, the first `LILOTAFS_WEAR_LEVEL_MAX_RECORDS` files (currently set as 5) are copied to the tail and then deleted. If possible, the flash erase sectors will also be erased. This is to ensure the file system does not become fragmented, since the space occupied by deleted files are reclaimed by erasing flash sectors.

The full wear leveling process is:

 1. Start from the head file (with magic `LILOTAFS_START`).
 2. The current file has magic number `LILOTAFS_START`. If the current file has status `COMMITTED`, copy it to the tail.
 3. Set the magic number of the next file to `LILOTAFS_START_CLEAN`.
 4. Clobber the contents of the current file by removing all `LILOTAFS_START = 0x5a00` and `LILOTAFS_START_CLEAN = 0x5aa0` bytes on 8 byte boundaries. This is to ensure if we crash, they are not falsely detected as the head file.
 5. If the next file is in a different flash erase sector from the current file, erase all sectors not before the current file but before the next file. In the diagram below (`X` is the current file, `Y` is the next file, `A` is another files, `|` marks a sector boundary and `-` is free space), sectors `0` and `1` are erased:
```
| SECTOR 0       | SECTOR 1       | SECTOR 2       | SECTOR 4       |
|--------------XX|XXXXXXXXXXXXXXXX|XXYYYYYYYYAAAAA-|----------------|
```
 6. Set the magic number of the next file to `LILOTAFS_START`.
 7. If the sector the current file is in has not been erased in step 5, delete the current file by setting its magic number to `LILOTAFS_DELETED = 0`. The magic number is changed in addition to status to completely remove it from the file chain (as we are advancing the head file forward).
 8. Set the current file to the next file, and go back to step 2. This process is repeated `LILOTAFS_WEAR_LEVEL_MAX_RECORDS` times, unless the number of files is less than `LILOTAFS_WEAR_LEVEL_MAX_RECORDS`, in which the number of files is used.

#### Crash Recovery from Wear Leveling

If a crash occurs during wear leveling, it will be detected and processed when finding the file system head (in section 5.1).

 - If `LILOTAFS_START_CLEAN` is found (and is the first header) and `LILOTAFS_START` is not found, change `LILOTAFS_START_CLEAN` to `LILOTAFS_START.`
 - `LILOTAFS_START` is found and is immediately followed by `LILOTAFS_START_CLEAN`, perform steps 4 to 8 above.
 - `LILOTAFS_START` is found and is immediately followed by `LILOTAFS_START` (crash in the final step), the first header with `LILOTAFS_START` is deleted by setting its magic number to `0` and the second is made the head.

## 6. Torture Testing

This repository also includes a torture testing suite `torture_util.c`, `torture_util.h`, `torture.c`, `megatorture.py` to subject the file system to extreme requests to test its functionality, including crash recovery.

A hardware abstraction layer can be found in `flash.h` and `flash.c`. Flash read, write and erase functions are different depending on if lilotafs is being compiled for the esp32, or on a computer for testing. lilotafs can be run on the host computer by writing to a binary file filled with `0xFF` bytes, generated with:
```bash
dd if=/dev/zero bs=$(TOTAL_SIZE) count=1 | tr '\000' '\377' > disk.bin
```
Some settings may need to be changed with the shell locale to run this command.

### 6.1. Torture Basics

The file system is subjected to a series of random write: random data of random length is written to a random file. At the start, we choose a number of random file names, and subsequent `write` calls will choose a file from this name to write to. At the same time, the correct value of each file is maintained in memory. The file system is inspected periodically with `read` calls, and the contents of the file system is compared with the correct value stored in memory. If the file system runs out of spaced, a reboot is simulated by unmounting and remounting the file system, during which wear leveling will take place, hopefully reclaiming enough space to proceed with the torture test.

### 6.2. Crash Injection

To test crash recovery, we first need to be able to inject crashes. This is done through long jumps. After a random number of bytes written to the file system, a "crash" is simulated by jumping to the main torture loop with `setjmp` and `lompjmp`. Back in the main torture loop, the file system is remounted, and the file system is checked against the model of the file system stored in memory for crash recovery correctness. Note that the file written to when the crash occurred can either hold the new contents or the old contents, both of which are counted as correct.

### 6.3. Automation

At the start of each torture test, the random number generator is either seeded with the time or with a user-provided argument. This is to ensure each torture test instance can be recreated for debugging.

The `megatorture.py` file automatically runs a large number of torture tests. If a torture test does not pass by containing files with different contents as the correct contents stored in memory, or by returning an error, the seed for the trial is printed for debugging.

