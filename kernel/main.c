/*
Copyright (C) 2015-2019 The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file LICENSE for details.
*/

#include "console.h"
#include "page.h"
#include "process.h"
#include "keyboard.h"
#include "mouse.h"
#include "interrupt.h"
#include "clock.h"
#include "ata.h"
#include "device.h"
#include "cdromfs.h"
#include "string.h"
#include "graphics.h"
#include "kernel/ascii.h"
#include "kernel/syscall.h"
#include "rtc.h"
#include "kernelcore.h"
#include "kmalloc.h"
#include "memorylayout.h"
#include "kshell.h"
#include "diskfs.h"
#include "serial.h"
#include <stddef.h>

void detect_memory() {
    uint32_t mem;
    uint32_t step = 1024 * 1024; // 1MB

    printf("Detecting memory...\n");

    // Start probing from MAIN_MEMORY_START (2MB)
    // We assume the first 2MB are valid as the kernel is running there.
    for(mem = MAIN_MEMORY_START; mem < 0xFFF00000; mem += step) {
        volatile uint32_t *p = (uint32_t*)mem;
        uint32_t old = *p;
        *p = 0x55AA55AA;
        if(*p != 0x55AA55AA) break;
        *p = 0xAA55AA55;
        if(*p != 0xAA55AA55) break;
        *p = old;
    }

    total_memory = mem / (1024 * 1024);
    printf("Detected RAM: %d MB\n", total_memory);
}

int kernel_main()
{
    struct console *console = console_create_root();
    console_addref(console);

    // Display boot logo
    printf("      __________    __________\n");
    printf("     |  __  __  |  |  __  __  |\n");
    printf("     | |  ||  | |  | |  ||  | |\n");
    printf("     | |  ||  | |  | |  ||  | |    ___   ____\n");
    printf("     | |__||__| |  | |__||__| |   / _ \\ / ___|\n");
    printf("     |  __  __()|  |()__  __  |  | | | |\\___ \\\n");
    printf("     | |  ||  | |  | |  ||  | |  | |_| | ___) |\n");
    printf("     | |  ||  | |  | |  ||  | |   \\___/ |____/\n");
    printf("     | |__||__| |  | |__||__| |\n");
    printf("     |__________|  |__________|\n");
    printf("\n");

    detect_memory();
    page_init();
    kmalloc_init((char *) KMALLOC_START, KMALLOC_LENGTH);

    // Start the setup immediately
    interrupt_init();
    mouse_init();
    keyboard_init();
    rtc_init();
    clock_init();
    process_init();
    ata_init();
    cdrom_init();
    diskfs_init();

    current->ktable[KNO_STDIN]   = kobject_create_console(console);
    current->ktable[KNO_STDOUT]  = kobject_copy(current->ktable[0]);
    current->ktable[KNO_STDERR]  = kobject_copy(current->ktable[1]);
    current->ktable[KNO_STDWIN]  = kobject_create_window(&window_root);
    current->ktable[KNO_STDDIR]  = 0; 
    
	kshell_launch();

    while(1) {
        console_putchar(console, console_getchar(console));
    }

    return 0;
}