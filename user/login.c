/*
 * Copyright (C) 2016-2019 The University of Notre Dame
 * All modifications copyrighted to XPDevs and James Turner
 * * login.c — Static TGA Background Login
 */

#include "library/nwindow.h"
#include "library/syscalls.h"
#include "library/stdio.h"
#include "library/string.h"
#include "library/malloc.h"

extern void nw_draw_scaled_tga(struct nwindow *nw, int x, int y, int w, int h, const char *path);

/* Helper to get screen resolution from the BGA/bootloader */
static void read_boot_dimensions(int *w_out, int *h_out) {
    unsigned short w, h;
    __asm__ __volatile__ (
        "movw $0x1, %%ax\n\t"
        "movw $0x1CE, %%dx\n\t"
        "outw %%ax, %%dx\n\t"
        "movw $0x1D0, %%dx\n\t"
        "inw %%dx, %%ax"
        : "=a"(w) : : "dx"
    );
    __asm__ __volatile__ (
        "movw $0x2, %%ax\n\t"
        "movw $0x1CE, %%dx\n\t"
        "outw %%ax, %%dx\n\t"
        "movw $0x1D0, %%dx\n\t"
        "inw %%dx, %%ax"
        : "=a"(h) : : "dx"
    );
    if (w >= 320 && w <= 4096 && h >= 200 && h <= 4096) {
        *w_out = w; *h_out = h;
    } else {
        *w_out = 1024; *h_out = 768;
    }
}

/*
 * Temporarily disable/enable mouse interrupts (IRQ 12) by masking
 * the corresponding bit on the slave PIC. This prevents the mouse
 * interrupt handler from running during a screen redraw, which avoids
 * a race condition where the handler could restore a stale background
 * tile, creating a "ghost" of the previous screen.
 */
static void disable_mouse_interrupt(void) {
    unsigned char mask;
    // Read current mask from slave PIC
    __asm__ __volatile__ ("inb $0xA1, %%al" : "=a"(mask));
    // Set bit 4 to mask IRQ 12 (8+4)
    mask |= 0x10;
    __asm__ __volatile__ ("outb %%al, $0xA1" : : "a"(mask));
}

static void enable_mouse_interrupt(void) {
    unsigned char mask;
    __asm__ __volatile__ ("inb $0xA1, %%al" : "=a"(mask));
    mask &= ~0x10; // Unmask IRQ 12
    __asm__ __volatile__ ("outb %%al, $0xA1" : : "a"(mask));
}

int main(int argc, char *argv[])
{
    struct nwindow *nw = nw_create_default();
    if (!nw) {
        return 1;
    }

    int sw, sh;
    read_boot_dimensions(&sw, &sh);

    /*
     * Atomically update the screen and cursor state to prevent ghosting.
     * 1. Disable mouse interrupts to prevent the cursor from being redrawn
     *    with a stale background during the screen update.
     * 2. Draw the new background image and flush it to the screen.
     * 3. Refresh the cursor, which saves the new background and redraws the sprite.
     * 4. Re-enable mouse interrupts.
     */
    disable_mouse_interrupt();

    /* Load the background image scaled to screen dimensions */
    nw_draw_scaled_tga(nw, 0, 0, sw, sh, "/shell/login/login_def.tga");
    /* Final flush to the framebuffer */
    nw_flush(nw);
    syscall_mouse_show();
    enable_mouse_interrupt();

    /* Wait for Ctrl+E to exit */
    struct event e;
    while (1) {
        if (nw_next_event(nw, &e)) {
            if (e.type == EVENT_KEY_DOWN && e.code == 0x05) { /* Ctrl+E */
                break;
            }
        } else {
            syscall_process_sleep(10);
        }
    }

    /*
     * Atomically clear the screen and refresh the cursor before exiting.
     * This ensures the mouse driver saves a clean (black) background,
     * preventing a "ghost" of the login image from being drawn on top
     * of the next screen (e.g., the kernel shell).
     */
    disable_mouse_interrupt();
    nw_fgcolor(nw, 0, 0, 0);
    nw_rect(nw, 0, 0, sw, sh);
    nw_flush(nw);
    syscall_mouse_show();
    enable_mouse_interrupt();

    return 0;
}