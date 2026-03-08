/*
Copyright (C) 2016-2019 The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file LICENSE for details.

All modifications are copyrighted to XPDevs and James Turner
XPDevs and James Turner use basekernel as a base where mods are added and updated
for the NexShell kernel

shutdown.c — shutdown screen, structural clone of restart.c.
Displays the iOS-style 12-bar spinner (matching restart.html) for 3 seconds,
then shuts down.
*/

#include "library/nwindow.h"
#include "library/syscalls.h"
#include "library/stdio.h"
#include "library/string.h"

/* ============================================================
 * Spinner constants  — mirrored from restart.html
 *
 *  HTML spinner box : 30 px
 *  Bar size         : 8 % wide × 24 % tall  → ~2.4 × 7.2 px
 *  Translate offset : 140 % of bar-height   → ~10 px radius
 *
 *  Scaled up for a 1024×768 kernel framebuffer (×4):
 *    SPINNER_R = 40   (radius to bar centre)
 *    DOT_W     =  4   (bar width  — kept thin like the HTML)
 *    DOT_H     = 14   (bar height)
 *    DOT_RADIUS=  2   (corner rounding — border-radius: 50px on a small rect)
 *
 *  Timing: HTML setTimeout = 4000 ms; user requested 3 s.
 *    TICKS_PER_SEC = 50,  3 s → 150 ticks total.
 *    HTML animation-duration = 1 s per full revolution.
 *    Each bar advances every 4 ticks → 12 steps × 4 = 48 ticks ≈ ~1 s/rev. ✓
 * ============================================================ */
#define RESTART_DURATION_MS     4000  /* 4 s — solidly within 3–5 s range */
#define RESTART_STEP_MS         83    /* 1000 ms / 12 bars ≈ 83 ms        */

#define RESTART_BARS        12
#define RESTART_SPINNER_R   40   /* px to bar centre              */
#define RESTART_DOT_W        4   /* bar width  (8 % of 30 scaled) */
#define RESTART_DOT_H       14   /* bar height (24% of 30 scaled) */
#define RESTART_DOT_RADIUS   2   /* corner rounding               */

/*
 * Fixed-point sin/cos table (×1000) for 12 evenly-spaced angles.
 * Matches the 30-deg steps in the HTML CSS transforms.
 */
static const int rs_sin[12] = {
       0,  500,  866, 1000,  866,  500,
       0, -500, -866,-1000, -866, -500
};
static const int rs_cos[12] = {
    1000,  866,  500,    0, -500, -866,
   -1000, -866, -500,    0,  500,  866
};

/*
 * Brightness levels — mirror the CSS keyframes:
 *   from { opacity:1; color:#fff }  →  to { opacity:0.25; color:#8e8e93 }
 * Six steps cover "just lit" down to "fully faded", matching the 6 trailing
 * bars visible at any moment.
 */
struct color { uint8_t r, g, b; };
static const struct color rs_levels[6] = {
    {255, 255, 255},   /* step 0 — current bar  (opacity 1, white)     */
    {220, 220, 220},   /* step 1                                        */
    {190, 190, 190},   /* step 2                                        */
    {168, 168, 168},   /* step 3                                        */
    {152, 152, 152},   /* step 4                                        */
    {142, 142, 147},   /* step 5 — oldest bar   (#8e8e93 ≈ opacity 0.25)*/
};

static void shutdown(void) {
    __asm__ __volatile__ (
        "mov $0x604, %%dx\n\t"
        "mov $0x2000, %%ax\n\t"
        "out %%ax, %%dx\n\t"
        :
        :
        : "ax", "dx"
    );
}

static void read_boot_dimensions(int *w_out, int *h_out) {
    unsigned short w, h;

    /* Read BGA X resolution (index 1) */
    __asm__ __volatile__ (
        "movw $0x1, %%ax\n\t"
        "movw $0x1CE, %%dx\n\t"
        "outw %%ax, %%dx\n\t"
        "movw $0x1D0, %%dx\n\t"
        "inw %%dx, %%ax"
        : "=a"(w) : : "dx"
    );
    /* Read BGA Y resolution (index 2) */
    __asm__ __volatile__ (
        "movw $0x2, %%ax\n\t"
        "movw $0x1CE, %%dx\n\t"
        "outw %%ax, %%dx\n\t"
        "movw $0x1D0, %%dx\n\t"
        "inw %%dx, %%ax"
        : "=a"(h) : : "dx"
    );

    if (w >= 320 && w <= 4096 && h >= 200 && h <= 4096) {
        *w_out = w;
        *h_out = h;
    } else {
        *w_out = 1024;
        *h_out = 768;
    }
}

static void rs_fill_rect(struct nwindow *nw, int x, int y, int w, int h, struct color c)
{
    if (w <= 0 || h <= 0) return;
    nw_fgcolor(nw, c.r, c.g, c.b);
    nw_rect(nw, x, y, w, h);
}

static void rs_draw_rounded_rect(struct nwindow *nw, int x, int y, int w, int h, int r, struct color c)
{
    if (w <= 0 || h <= 0) return;
    if (r <= 0) { rs_fill_rect(nw, x, y, w, h, c); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    
    nw_fgcolor(nw, c.r, c.g, c.b);
    nw_rect(nw, x + r, y, w - 2 * r, h);
    nw_rect(nw, x, y + r, r, h - 2 * r);
    nw_rect(nw, x + w - r, y + r, r, h - 2 * r);
    
    /* Corners - simple approximation using rects */
    for (int dy = 0; dy < r; dy++) {
        int span = r - dy; if (span < 0) span = 0;
        nw_rect(nw, x + r - span, y + r - dy - 1, span, 1);
        nw_rect(nw, x + w - r,    y + r - dy - 1, span, 1);
        nw_rect(nw, x + r - span, y + h - r + dy, span, 1);
        nw_rect(nw, x + w - r,    y + h - r + dy, span, 1);
    }
}

/* ── Core draw routine ─────────────────────────────────────────── */
static void draw_restart_screen(struct nwindow *nw, int sw, int sh, int step)
{
    struct color black = {0, 0, 0};

    /* Black fill — matches body { background-color:#000 } */
    rs_fill_rect(nw, 0, 0, sw, sh, black);

    int cx = sw / 2;
    int cy = sh / 2;

    /*
     * Draw 12 bars.  For each bar i:
     *   - compute how many steps behind the "lit" position it is
     *   - look up its brightness level
     *   - place it using the sin/cos table (same 30-deg increments as CSS rotate)
     */
    for (int i = 0; i < RESTART_BARS; i++) {
        int behind = (step - i + RESTART_BARS) % RESTART_BARS;
        int level  = (behind < 6) ? behind : 5;

        /* Bar centre — matches translate(0, -140%) after rotate(N deg) */
        int bx = cx + (RESTART_SPINNER_R * rs_sin[i]) / 1000;
        int by = cy - (RESTART_SPINNER_R * rs_cos[i]) / 1000;

        /* Top-left corner of the bar rectangle */
        int rx = bx - RESTART_DOT_W / 2;
        int ry = by - RESTART_DOT_H / 2;

        rs_draw_rounded_rect(nw, rx, ry,
                             RESTART_DOT_W, RESTART_DOT_H,
                             RESTART_DOT_RADIUS,
                             rs_levels[level]);
    }
    /* No text — matches the HTML which shows only the spinner */
}

/* ============================================================
 * Entry point
 * ============================================================ */
int main(int argc, char *argv[])
{
    struct nwindow *nw = nw_create_default();
    if (!nw) {
        printf("Failed to create window\n");
        return 1;
    }

    int sw, sh;
    read_boot_dimensions(&sw, &sh);
    
    int steps = RESTART_DURATION_MS / RESTART_STEP_MS;
    int step = 0;

    for (int i = 0; i < steps; i++) {
        draw_restart_screen(nw, sw, sh, step);
        nw_flush(nw);
        
        step = (step + 1) % RESTART_BARS;
        syscall_process_sleep(RESTART_STEP_MS);
    }

    /* Shutdown */
    shutdown();
    
    return 0;
}