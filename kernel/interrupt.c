/*
Copyright (C) 2015-2019 The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file LICENSE for details.
*/

#include "interrupt.h"
#include "console.h"
#include "pic.h"
#include "process.h"
#include "kernelcore.h"
#include "x86.h"
#include "graphics.h"
#include "ioports.h"

static interrupt_handler_t interrupt_handler_table[48];
static uint32_t interrupt_count[48];
static uint8_t interrupt_spurious[48];

static const char *exception_names[] = {
	"division by zero",
	"debug exception",
	"nonmaskable interrupt",
	"breakpoint",
	"overflow",
	"bounds check",
	"invalid instruction",
	"coprocessor error",
	"double fault",
	"copressor overrun",
	"invalid task",
	"segment not present",
	"stack exception",
	"general protection fault",
	"page fault",
	"unknown",
	"coprocessor error"
};

/*
 * Exception Error Codes
 * These codes are displayed on the blue screen of death.
 * 0: 0x4E530000 - Division by zero
 * 1: 0x4E530001 - Debug exception
 * 2: 0x4E530002 - Nonmaskable interrupt
 * 3: 0x4E530003 - Breakpoint
 * 4: 0x4E530004 - Overflow
 * 5: 0x4E530005 - Bounds check
 * 6: 0x4E530006 - Invalid instruction
 * 7: 0x4E530007 - Coprocessor error
 * 8: 0x4E530008 - Double fault
 * 9: 0x4E530009 - Coprocessor overrun
 * 10: 0x4E53000A - Invalid task
 * 11: 0x4E53000B - Segment not present
 * 12: 0x4E53000C - Stack exception
 * 13: 0x4E53000D - General protection fault
 * 14: 0x4E53000E - Page fault
 * 15: 0x4E53000F - Unknown
 * 16: 0x4E530010 - Coprocessor error
 */
static const uint32_t exception_codes[] = {
	0x4E530000, 0x4E530001, 0x4E530002, 0x4E530003, 0x4E530004, 0x4E530005, 0x4E530006, 0x4E530007,
	0x4E530008, 0x4E530009, 0x4E53000A, 0x4E53000B, 0x4E53000C, 0x4E53000D, 0x4E53000E, 0x4E53000F, 0x4E530010
};

static void unknown_exception(int i, int code)
{
	unsigned vaddr; // virtual address trying to be accessed
	unsigned paddr; // physical address
	unsigned esp; // stack pointer

	if(i==14) {
		asm("mov %%cr2, %0" : "=r" (vaddr) ); // virtual address trying to be accessed		
		esp  = ((struct x86_stack *)(current->kstack_top - sizeof(struct x86_stack)))->esp; // stack pointer of the process that raised the exception
		// Check if the requested memory is in the stack or data
		int data_access = vaddr < current->vm_data_size;

		// Subtract 128 from esp because of the red-zone 
		// According to https:gcc.gnu.org, the red zone is a 128-byte area beyond 
		// the stack pointer that will not be modified by signal or interrupt handlers 
		// and therefore can be used for temporary data without adjusting the stack pointer.
		int stack_access = vaddr >= esp - 128; 

		// Check if the requested memory is already in use
		int page_already_present = pagetable_getmap(current->pagetable,vaddr,&paddr,0);
		
		// Check if page is already mapped (which will result from violating the permissions on page) or that
		// we are accessing neither the stack nor the heap, or we are accessing both. If so, error
		if (page_already_present || !(data_access ^ stack_access)) {
			printf("interrupt: illegal page access at vaddr %x\n",vaddr);
			process_dump(current);
			process_exit(0);
		} else {
			// XXX update process->vm_stack_size when growing the stack.
			pagetable_alloc(current->pagetable, vaddr, PAGE_SIZE, PAGE_FLAG_USER | PAGE_FLAG_READWRITE | PAGE_FLAG_CLEAR);
			return;
		}
	} else {
		if (current) {
			printf("interrupt: exception %d: %s (code %x)\n", i, exception_names[i], code);
			process_dump(current);
		}
	}

	if(current) {
		process_exit(0);
	} else {
		
		// Establish the DoorsOS Panic Palette
// Establish the DoorsOS Panic Palette (Urgent Red)
struct graphics_color red = {255, 0, 0, 0};   // Full Red, 0 Green, 0 Blue
struct graphics_color white = {255, 255, 255, 0};

// Set the console: White text on a Red background
console_set_color(white, red);
        
        printf("\f"); // Clear screen to blue

        // Header - Using the specific XP phrasing
        printf("A problem has been detected and DoorsOS has been shut down to prevent damage\n");
        printf("to your computer.\n\n");
        
        printf("The problem seems to be caused by the following execution fault:\n\n");

        // Error Diagnostics (Using your exact logic but XP formatting)
        if (i < 17) {
            // XP often uppercase errors, and doesn't always use padding
            printf("Error: %s\n", exception_names[i]);
            printf("\nSTOP: 0x%08x (0x%08x, 0x%08x, 0x%08x, 0x%08x)\n\n", exception_codes[i], code, 0, 0, 0);
        } else {
            printf("Error: Unknown Exception %d\n", i);
            printf("\nSTOP: 0x%08x (0x%08x, 0x%08x, 0x%08x, 0x%08x)\n\n", i, code, 0, 0, 0);
        }

        // XP Specific Instructions
        printf("If this is the first time you've seen this Stop error screen,\n");
        printf("restart your computer. If this screen appears again, follow\n");
        printf("these steps:\n\n");

        printf("Check to make sure any new hardware or software is properly installed.\n");
        printf("If this is a new installation, ask your hardware or software manufacturer\n");
        printf("for any DoorsOS updates you might need.\n\n");

        // Reference link (Replacing memory dumps for NexShell)
        printf("Technical information:\n");
        printf("Visit https://xpdevs.github.io/ErrorCodes for details.\n\n");

        printf("Collecting data for crash dump...\n");
        printf("Initializing disk for crash dump...\n");
        
        printf("\nPress [ENTER] to reboot.");

        // Input Polling Loop
        while(1) {
            // Check keyboard status and scan code for Enter (0x1C)
            if ((inb(0x64) & 1) && inb(0x60) == 0x1C) {
                printf("\f\n  REBOOTING...");
                reboot();
            }
        }
	}
}

static void unknown_hardware(int i, int code)
{
	if(!interrupt_spurious[i]) {
		printf("interrupt: spurious interrupt %d\n", i);
	}
	interrupt_spurious[i]++;
}

void interrupt_register(int i, interrupt_handler_t handler)
{
	interrupt_handler_table[i] = handler;
}

static void interrupt_acknowledge(int i)
{
	if(i < 32) {
		/* do nothing */
	} else {
		pic_acknowledge(i - 32);
	}
}

void interrupt_init()
{
	int i;
	pic_init(32, 40);
	for(i = 32; i < 48; i++) {
		interrupt_disable(i);
		interrupt_acknowledge(i);
	}
	for(i = 0; i < 32; i++) {
		interrupt_handler_table[i] = unknown_exception;
		interrupt_spurious[i] = 0;
		interrupt_count[i] = 0;
	}
	for(i = 32; i < 48; i++) {
		interrupt_handler_table[i] = unknown_hardware;
		interrupt_spurious[i] = 0;
		interrupt_count[i] = 0;
	}

	interrupt_unblock();

}

void interrupt_handler(int i, int code)
{
	(interrupt_handler_table[i]) (i, code);
	interrupt_acknowledge(i);
	interrupt_count[i]++;
}

void interrupt_enable(int i)
{
	if(i < 32) {
		/* do nothing */
	} else {
		pic_enable(i - 32);
	}
}

void interrupt_disable(int i)
{
	if(i < 32) {
		/* do nothing */
	} else {
		pic_disable(i - 32);
	}
}

void interrupt_block()
{
	asm("cli");
}

void interrupt_unblock()
{
	asm("sti");
}

void interrupt_wait()
{
	asm("sti");
	asm("hlt");
}
