/* Binary Loader for NexShell / DoorsOS 
   Loads raw machine code directly into memory.
*/

#include "fs.h"
#include "string.h"
#include "console.h"
#include "process.h"
#include "memorylayout.h"

int elf_load(struct process *p, struct fs_dirent *d, addr_t *entry)
{
	uint32_t file_size;
	uint32_t actual;

	// 1. Get the size of the file
	file_size = fs_dirent_size(d);
	
	if(file_size == 0 || file_size > 0x8000000) {
		goto noload;
	}

	// 2. Prepare the memory space (starts at your defined entry point)
	// We ensure the process has enough room for the whole file
	if(process_data_size_set(p, file_size) != 0) {
		goto nomem;
	}

	// 3. Copy the raw binary directly into the entry point address
	// In your ecosystem, this is usually 0x1000
	actual = fs_dirent_read(d, (char *) PROCESS_ENTRY_POINT, file_size, 0);
	
	if(actual != file_size) {
		goto mustdie;
	}

	// 4. Set the entry point to exactly where we loaded the code
	*entry = PROCESS_ENTRY_POINT;

	return 0;

noload:
	printf("bin: file empty or too large\n");
	return -1;
nomem:
	printf("bin: out of memory\n");
	return -1;
mustdie:
	printf("bin: load failed\n");
	return -1;
}
