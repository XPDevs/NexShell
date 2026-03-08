/* Binary Loader for NexShell / DoorsOS
   Loads and runs ELF binaries and raw flat binaries.
   Press Ctrl+E to forcefully stop a running program.

   Self-contained execution:
     - Allocates binary stack
     - Builds a proper x86-32 ELF initial stack frame (argc/argv/envp/auxv)
     - Switches stack and jumps (not calls) to _start
     - Binary exits by calling bin_do_exit() from the syscall handler,
       which longjmps back into bin_run() for clean teardown.

   Wire-up required in your syscall dispatcher:
     syscall exit(int code)  →  bin_do_exit(code);
*/

#include "fs.h"
#include "string.h"
#include "console.h"
#include "process.h"
#include "memorylayout.h"
#include "kmalloc.h"
#include "graphics.h"
#include "interrupt.h"
#include "clock.h"
#include <setjmp.h>
#include <stddef.h>

// ELF Type Definitions
typedef uint32_t uintptr_t;
typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Word;

#define EI_NIDENT   16
#define PT_LOAD     1
#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

#define CTRL_E          0x05
#define BIN_STACK_SIZE  (64 * 1024)

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word    p_type;
    Elf32_Off     p_offset;
    Elf32_Addr    p_vaddr;
    Elf32_Addr    p_paddr;
    Elf32_Word    p_filesz;
    Elf32_Word    p_memsz;
    Elf32_Word    p_flags;
    Elf32_Word    p_align;
} Elf32_Phdr;

/* ── System App Detection ─────────────────────────────────────── */
#define SYS_APP_GUID "NEXSHELL_SYS_RESTART_APP_V1"

/* Must match the structure in restart.c */
struct sys_app_header {
    char guid[32];
    struct graphics *g_root;
    void (*fn_fgcolor)(struct graphics *g, struct graphics_color c);
    void (*fn_rect)(struct graphics *g, int x, int y, int w, int h);
    void (*fn_wait)(void);
    void (*fn_disable)(int i);
    uint32_t (*fn_time)(void);
    int screen_w;
    int screen_h;
};

extern struct graphics graphics_root;

/* ------------------------------------------------------------------ */
/*  setjmp/longjmp implementation for x86-32                          */
/* ------------------------------------------------------------------ */

__attribute__((naked)) int setjmp(jmp_buf env) {
    __asm__ __volatile__ (
        "movl 4(%esp), %edx\n\t"  // Get env pointer (arg1)
        "movl %ebx, 0(%edx)\n\t"  // Save ebx
        "movl %esi, 4(%edx)\n\t"  // Save esi
        "movl %edi, 8(%edx)\n\t"  // Save edi
        "movl %ebp, 12(%edx)\n\t" // Save ebp
        "leal 4(%esp), %eax\n\t"  // Calculate caller's esp (esp + 4)
        "movl %eax, 16(%edx)\n\t" // Save esp
        "movl 0(%esp), %eax\n\t"  // Get return address (eip)
        "movl %eax, 20(%edx)\n\t" // Save eip
        "xorl %eax, %eax\n\t"     // Return 0
        "ret\n\t"
    );
}

__attribute__((naked)) void longjmp(jmp_buf env, int val) {
    __asm__ __volatile__ (
        "movl 4(%esp), %edx\n\t"  // Get env pointer
        "movl 8(%esp), %eax\n\t"  // Get val
        "testl %eax, %eax\n\t"    // If val == 0
        "jnz 1f\n\t"
        "incl %eax\n\t"           //   val = 1
        "1:\n\t"
        "movl 0(%edx), %ebx\n\t"  // Restore ebx
        "movl 4(%edx), %esi\n\t"  // Restore esi
        "movl 8(%edx), %edi\n\t"  // Restore edi
        "movl 12(%edx), %ebp\n\t" // Restore ebp
        "movl 16(%edx), %esp\n\t" // Restore esp
        "jmp *20(%edx)\n\t"       // Jump to saved eip
    );
}

/* ------------------------------------------------------------------ */
/*  Exit / interrupt state                                              */
/* ------------------------------------------------------------------ */

/*
 * bin_jmpbuf    - setjmp context saved just before we jump to _start.
 *                 bin_do_exit() longjmps back here from the exit syscall.
 * bin_exit_code - value passed to bin_do_exit(), read after longjmp.
 * bin_running   - true while a binary is executing; guards bin_do_exit().
 * bin_interrupted - set by keyboard ISR on Ctrl+E.
 */
static jmp_buf         bin_jmpbuf;
static volatile int    bin_exit_code   = 0;
static volatile int    bin_running     = 0;
static volatile int    bin_interrupted = 0;

/*
 * bin_do_exit() - call this from your exit() syscall handler.
 *
 * If a binary is currently running, this longjmps back into bin_run()
 * with the supplied exit code and never returns to the caller.
 * If no binary is running the call is a no-op (safe to call regardless).
 *
 * Example syscall dispatch:
 *   case SYSCALL_EXIT:
 *       bin_do_exit((int)arg0);
 *       break;  // unreachable when binary is running
 */
void bin_do_exit(int code)
{
    if (!bin_running)
        return;

    bin_exit_code = code;
    bin_running   = 0;
    longjmp(bin_jmpbuf, 1);   /* never returns */
}

/* Called from the keyboard ISR when Ctrl+E (0x05) is detected. */
void bin_interrupt_check(char c)
{
    if (c == CTRL_E) {
        bin_interrupted = 1;
        /*
         * Uncomment to immediately kill the running binary on Ctrl+E
         * rather than waiting for it to make a syscall:
         */
        /* bin_do_exit(-1); */
    }
}

static void bin_interrupt_reset(void)
{
    bin_interrupted = 0;
}

static void sys_app_patch(struct process *p, uint32_t size);

/* ------------------------------------------------------------------ */
/*  Internal: load a raw flat binary                                   */
/* ------------------------------------------------------------------ */
static int load_raw(struct process *p, struct fs_dirent *d, addr_t *entry)
{
    uint32_t file_size = fs_dirent_size(d);
    uint32_t actual;

    if (file_size == 0 || file_size > 0x8000000) {
        printf("bin: file empty or too large\n");
        return -1;
    }

    if (process_data_size_set(p, file_size) != 0) {
        printf("bin: out of memory\n");
        return -1;
    }

    actual = fs_dirent_read(d, (char *)PROCESS_ENTRY_POINT, file_size, 0);
    if (actual != file_size) {
        printf("bin: load failed\n");
        return -1;
    }

    /* Check if this is a system app and patch it */
    sys_app_patch(p, file_size);

    *entry = PROCESS_ENTRY_POINT;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sys_app_patch - Scan for GUID and inject kernel pointers           */
/* ------------------------------------------------------------------ */
static uint32_t sys_app_time(void) {
    clock_t t = clock_read();
    return t.seconds * 1000 + t.millis;
}

static void sys_app_patch(struct process *p, uint32_t size)
{
    /* Scan the loaded binary image for the GUID */
    char *base = (char *)PROCESS_ENTRY_POINT;
    char *limit = base + size;

    size_t guid_len = strlen(SYS_APP_GUID);
    for (char *ptr = base; ptr <= limit - sizeof(struct sys_app_header); ptr++) {
        if (strncmp(ptr, SYS_APP_GUID, guid_len) == 0) {
            struct sys_app_header *hdr = (struct sys_app_header *)ptr;
            printf("bin: detected system app, granting kernel access...\n");
            
            hdr->g_root = &graphics_root;
            hdr->fn_fgcolor = graphics_fgcolor;
            hdr->fn_rect = graphics_rect;
            hdr->fn_wait = interrupt_wait;
            hdr->fn_disable = interrupt_disable;
            hdr->fn_time = sys_app_time;
            hdr->screen_w = graphics_width(&graphics_root);
            hdr->screen_h = graphics_height(&graphics_root);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Internal: load an ELF binary                                       */
/* ------------------------------------------------------------------ */
static int load_elf(struct process *p, struct fs_dirent *d, addr_t *entry)
{
    Elf32_Ehdr ehdr;
    Elf32_Phdr *phdrs;
    uint32_t actual;

    actual = fs_dirent_read(d, (char *)&ehdr, sizeof(ehdr), 0);
    if (actual != sizeof(ehdr)) {
        printf("bin: read error or empty\n");
        return -1;
    }

    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        printf("bin: not a valid ELF file\n");
        return -1;
    }

    if (ehdr.e_phnum == 0) {
        printf("bin: ELF has no program headers\n");
        return -1;
    }

    phdrs = kmalloc(sizeof(Elf32_Phdr) * ehdr.e_phnum);
    if (!phdrs) {
        printf("bin: out of memory\n");
        return -1;
    }

    actual = fs_dirent_read(d, (char *)phdrs,
                            sizeof(Elf32_Phdr) * ehdr.e_phnum,
                            ehdr.e_phoff);
    if (actual != sizeof(Elf32_Phdr) * ehdr.e_phnum) {
        kfree(phdrs);
        printf("bin: load failed (phdrs)\n");
        return -1;
    }

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD)
            continue;

        if (bin_interrupted) {
            kfree(phdrs);
            printf("\nbin: interrupted during load\n");
            return -1;
        }

        uint32_t max_addr = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (max_addr > p->vm_data_size) {
            if (process_data_size_set(p, max_addr) != 0) {
                kfree(phdrs);
                printf("bin: out of memory\n");
                return -1;
            }
        }

        if (phdrs[i].p_filesz > 0) {
            actual = fs_dirent_read(d,
                                    (char *)phdrs[i].p_vaddr,
                                    phdrs[i].p_filesz,
                                    phdrs[i].p_offset);
            if (actual != phdrs[i].p_filesz) {
                kfree(phdrs);
                printf("bin: segment read failed\n");
                return -1;
            }
        }

        /* Zero BSS region (memsz > filesz) */
        if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
            memset((char *)(phdrs[i].p_vaddr + phdrs[i].p_filesz),
                   0,
                   phdrs[i].p_memsz - phdrs[i].p_filesz);
        }
    }

    kfree(phdrs);
    *entry = ehdr.e_entry;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  bin_load - auto-detect ELF vs raw and load into process            */
/* ------------------------------------------------------------------ */
int bin_load(struct process *p, struct fs_dirent *d, addr_t *entry)
{
    unsigned char magic[4];
    uint32_t actual;

    actual = fs_dirent_read(d, (char *)magic, 4, 0);
    if (actual < 4) {
        printf("bin: file too small\n");
        return -1;
    }

    if (magic[0] == ELFMAG0 && magic[1] == ELFMAG1 &&
        magic[2] == ELFMAG2 && magic[3] == ELFMAG3) {
        printf("bin: detected ELF binary\n");
        return load_elf(p, d, entry);
    } else {
        printf("bin: detected raw binary\n");
        return load_raw(p, d, entry);
    }
}

/* ------------------------------------------------------------------ */
/*  bin_exec - build ELF initial stack and jump to entry point         */
/*                                                                     */
/*  Implements the x86-32 System V ELF initial stack layout:           */
/*                                                                     */
/*    [esp+0]   argc  = 0                                              */
/*    [esp+4]   argv  NULL terminator  (no arguments)                  */
/*    [esp+8]   envp  NULL terminator  (no environment)                */
/*    [esp+12]  auxv  type=0, val=0    (AT_NULL sentinel)              */
/*                                                                     */
/*  We JMP (not CALL) to _start because ELF entry points are declared  */
/*  noreturn — they exit via syscall (int 0x80 / sysenter) and never   */
/*  ret back.  Control returns to bin_run() via bin_do_exit()/longjmp. */
/* ------------------------------------------------------------------ */
static void __attribute__((noreturn))
bin_exec(void *stack_top, addr_t entry)
{
    /*
     * Build the initial stack frame, growing downward from stack_top.
     * Align to 16 bytes first (System V ABI requires this before _start).
     */
    uint32_t *sp = (uint32_t *)((uintptr_t)stack_top & ~(uintptr_t)15);

    /* auxv AT_NULL sentinel: two zero words */
    *--sp = 0;   /* auxv[0].a_val  = 0 */
    *--sp = 0;   /* auxv[0].a_type = AT_NULL */

    /* envp: NULL-terminated list of env pointers (empty) */
    *--sp = 0;

    /* argv: NULL-terminated list of arg pointers (empty, argc=0) */
    *--sp = 0;

    /* argc */
    *--sp = 0;

    /*
     * Switch esp to the binary's stack and jump to _start.
     *
     * ebp = 0  marks the bottom of the call-frame chain for debuggers.
     * General-purpose registers are zeroed — the ABI leaves them
     * undefined at process start, but zeroing them avoids leaking
     * kernel state into the binary.
     *
     * There is no return address on the stack (this is a JMP, not CALL).
     * If _start ever executes a bare ret it will fault — that is correct
     * because _start must not return; it must call exit().
     */
    __asm__ __volatile__ (
        "movl  %[bsp], %%esp\n\t"   /* switch to binary stack         */
        "xorl  %%ebp,  %%ebp\n\t"   /* ebp = 0: bottom of frame chain */
        "xorl  %%eax,  %%eax\n\t"
        "xorl  %%ebx,  %%ebx\n\t"
        "xorl  %%ecx,  %%ecx\n\t"
        "xorl  %%edx,  %%edx\n\t"
        "xorl  %%esi,  %%esi\n\t"
        "xorl  %%edi,  %%edi\n\t"
        "jmp   *%[fn]\n\t"           /* jump to _start — no return addr */
        :
        : [bsp] "r" ((uint32_t)sp),
          [fn]  "r" ((uint32_t)entry)
        : "memory"
    );

    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/*  bin_run - load and execute a binary, honouring Ctrl+E              */
/*                                                                     */
/*  Top-level entry point called by NexShell.                          */
/*  Returns the binary's exit code on success, -1 on error/interrupt.  */
/* ------------------------------------------------------------------ */
int bin_run(struct process *p, struct fs_dirent *d)
{
    addr_t  entry = 0;
    void   *stack = NULL;
    void   *stack_top;

    bin_interrupt_reset();
    bin_exit_code = 0;

    printf("bin: loading...\n");

    if (bin_load(p, d, &entry) != 0)
        return -1;

    if (bin_interrupted) {
        printf("\nbin: interrupted before launch\n");
        return -1;
    }

    /* Allocate a private stack for the binary */
    stack = kmalloc(BIN_STACK_SIZE);
    if (!stack) {
        printf("bin: out of memory (stack)\n");
        return -1;
    }
    stack_top = (char *)stack + BIN_STACK_SIZE;

    printf("bin: stack  0x%x – 0x%x\n", (unsigned)stack, (unsigned)stack_top);
    printf("bin: entry  0x%x  (Ctrl+E to stop)\n", (unsigned)entry);

    /*
     * setjmp checkpoint.
     *
     * First pass  (setjmp == 0): mark binary as running, jump into it.
     *                             bin_exec() never returns normally.
     *
     * Second pass (setjmp == 1): arrived here from bin_do_exit()/longjmp.
     *                             Fall through to cleanup below.
     */
    if (setjmp(bin_jmpbuf) == 0) {
        bin_running = 1;
        bin_exec(stack_top, entry);   /* [[noreturn]] */
    }

    /* --- landed here via bin_do_exit() --- */
    bin_running = 0;

    /* Free the binary's stack */
    kfree(stack);

    /* Release the binary's loaded pages */
    process_data_size_set(p, 0);

    if (bin_interrupted) {
        printf("\nbin: stopped by user (Ctrl+E)\n");
        return -1;
    }

    printf("bin: exited with code %d\n", bin_exit_code);
    return bin_exit_code;
}

/* ------------------------------------------------------------------ */
/*  Legacy compatibility shim                                           */
/* ------------------------------------------------------------------ */
int elf_load(struct process *p, struct fs_dirent *d, addr_t *entry)
{
    return bin_load(p, d, entry);
}