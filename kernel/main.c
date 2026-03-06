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

/* Simple LCG for kernel-level randomness */
static uint32_t boot_seed = 12345;

/* Direct CMOS read to fix 'rtc_get_seconds' undefined reference */
static inline uint8_t cmos_read(uint8_t addr) {
    asm volatile ("outb %%al, $0x70" : : "a"(addr));
    uint8_t res;
    asm volatile ("inb $0x71, %%al" : "=a"(res));
    return res;
}

void seed_boot_rand() {
    // Read seconds directly from CMOS register 0x00
    boot_seed = (uint32_t)cmos_read(0x00) + 1;
}

uint32_t boot_rand() {
    boot_seed = 1103515245 * boot_seed + 12345;
    return (uint32_t)(boot_seed / 65536) % 32768;
}

#define BOOT_DELAY_LOOP() __asm__ __volatile__ ( \
    "mov $300000000, %%ecx\n"               \
    "1:\n"                                  \
    "dec %%ecx\n"                           \
    "jnz 1b\n"                              \
    :                                       \
    :                                       \
    : "ecx"                                 \
)

/* Logic: 70% chance to wait, 30% to skip for "bursty" output */
#define RANDOM_BOOT_DELAY() do { \
    if ((boot_rand() % 10) < 7) { \
        BOOT_DELAY_LOOP(); \
    } \
} while(0)

void get_screen_dimensions(uint16_t *width, uint16_t *height) {
    asm volatile (
        "movw $0x1, %%ax\n\t"
        "movw $0x1CE, %%dx\n\t"
        "outw %%ax, %%dx\n\t"
        "movw $0x1D0, %%dx\n\t"
        "inw %%dx, %%ax"
        : "=a"(*width)
        :
        : "dx"
    );

    asm volatile (
        "movw $0x2, %%ax\n\t"
        "movw $0x1CE, %%dx\n\t"
        "outw %%ax, %%dx\n\t"
        "movw $0x1D0, %%dx\n\t"
        "inw %%dx, %%ax"
        : "=a"(*height)
        :
        : "dx"
    );
}

void detect_memory() {
    uint32_t mem;
    uint32_t step = 1024 * 1024;
    printf("Detecting memory...\n");
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

    struct graphics_color black = {0, 0, 0, 0};
    struct graphics_color white = {255, 255, 255, 0};

    console_set_color(black, white);
    printf("\f");

    console_set_color(white, black);
    int w, h;
    console_size(console, &w, &h);
    const char *header = "NexShell Booting";
    int len = strlen(header);
    int pad = (w - len) / 2;
    if (pad < 0) pad = 0;

    for (int i = 0; i < w; i++) printf(" ");
    for (int i = 0; i < pad; i++) printf(" ");
    printf("%s", header);
    for (int i = pad + len; i < w; i++) printf(" ");
    for (int i = 0; i < w; i++) printf(" ");

    console_set_color(black, white);
    
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

    interrupt_init();
    rtc_init();
    seed_boot_rand(); // Now using inline cmos_read to avoid linker error

    mouse_init();
    keyboard_init();
    clock_init();
    process_init();
    ata_init();
    cdrom_init();
    diskfs_init();

    uint16_t screen_width, screen_height;
    get_screen_dimensions(&screen_width, &screen_height);
    *(uint16_t*)BOOT_INFO_SCREEN_WIDTH = screen_width;
    *(uint16_t*)BOOT_INFO_SCREEN_HEIGHT = screen_height;

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