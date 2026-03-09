/*
Copyright (C) 2016-2019 The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file LICENSE for details.

All modifications are copyrighted to XPDevs and James Turner
XPDevs and James Turner use basekernel as a base where mods are added and updated
for the NexShell kernel

shutdown.c — shutdown screen.
Displays the SVG arc spinner with "Shutting down" text,
then powers off.

Anti-flash technique mirrors GUI.c:
  - Black background + "Shutting down" text drawn ONCE before the loop.
  - Each frame erases only the previous arc, then draws the new one.
  - The full screen is never cleared mid-animation.
  - nw_flush() called only after both erase+draw (one atomic visual update).
*/

#include "library/nwindow.h"
#include "library/syscalls.h"
#include "library/stdio.h"
#include "library/string.h"

/* ============================================================
 * Timing
 * ============================================================ */
#define ANIM_TOTAL_MS      4000
#define ANIM_STEP_MS         33
#define ANIM_CYCLE_STEPS     60

/* ============================================================
 * Ring geometry — thin Win11-style arc
 * ============================================================ */
#define RING_RADIUS        32
#define ARC_STROKE          3
#define ARC_DRAW_STEP       1
#define ERASE_PAD           1

/* ── Text ──────────────────────────────────────────────────────── */
#define TEXT_GAP           18
#define CHAR_W              8    /* nw_string character cell width */

/* ============================================================
 * Pre-computed animation tables — 60 steps, one 2-second cycle.
 *
 *   ease1 = cubic-bezier(0.61, 1, 0.88, 1)  — ease-out (grow)
 *   ease2 = cubic-bezier(0.12, 0, 0.39, 0)  — ease-in  (shrink)
 *
 * arc_len_tab[i]:    arc length in degrees (0→180→0)
 * arc_offset_tab[i]: arc start shift in degrees (0→0→180)
 * Rotation computed inline: -90 + 15 * step  (900°/cycle)
 * ============================================================ */
static const int arc_len_tab[ANIM_CYCLE_STEPS] = {
      0,   9,  19,  28,  37,  47,  56,  64,  73,  81,
     89,  97, 104, 112, 119, 126, 132, 138, 144, 149,
    155, 159, 164, 168, 171, 174, 176, 178, 179, 180,
    180, 180, 179, 178, 176, 174, 171, 168, 164, 160,
    155, 149, 144, 138, 132, 125, 119, 112, 104,  97,
     89,  81,  73,  65,  56,  47,  37,  29,  19,   9
};

static const int arc_offset_tab[ANIM_CYCLE_STEPS] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  1,  2,  4,  6,  9, 12, 16, 21,
    25, 31, 36, 42, 48, 54, 61, 68, 76, 83,
    91, 99,107,116,124,133,143,152,161,171
};

/* ============================================================
 * sin/cos lookup (×1000) for 0–359 degrees, 1° resolution.
 * ============================================================ */
static const short sin360[360] = {
       0,  17,  35,  52,  70,  87, 105, 122, 139, 156,
     174, 191, 208, 225, 242, 259, 276, 292, 309, 326,
     342, 358, 375, 391, 407, 423, 438, 454, 469, 485,
     500, 515, 530, 545, 559, 574, 588, 602, 616, 629,
     643, 656, 669, 682, 695, 707, 719, 731, 743, 755,
     766, 777, 788, 799, 809, 819, 829, 839, 848, 857,
     866, 875, 883, 891, 899, 906, 914, 921, 927, 934,
     940, 946, 951, 956, 961, 966, 970, 974, 978, 982,
     985, 988, 990, 993, 995, 996, 998, 999, 999,1000,
    1000,1000, 999, 999, 998, 996, 995, 993, 990, 988,
     985, 982, 978, 974, 970, 966, 961, 956, 951, 946,
     940, 934, 927, 921, 914, 906, 899, 891, 883, 875,
     866, 857, 848, 839, 829, 819, 809, 799, 788, 777,
     766, 755, 743, 731, 719, 707, 695, 682, 669, 656,
     643, 629, 616, 602, 588, 574, 559, 545, 530, 515,
     500, 485, 469, 454, 438, 423, 407, 391, 375, 358,
     342, 326, 309, 292, 276, 259, 242, 225, 208, 191,
     174, 156, 139, 122, 105,  87,  70,  52,  35,  17,
       0, -17, -35, -52, -70, -87,-105,-122,-139,-156,
    -174,-191,-208,-225,-242,-259,-276,-292,-309,-326,
    -342,-358,-375,-391,-407,-423,-438,-454,-469,-485,
    -500,-515,-530,-545,-559,-574,-588,-602,-616,-629,
    -643,-656,-669,-682,-695,-707,-719,-731,-743,-755,
    -766,-777,-788,-799,-809,-819,-829,-839,-848,-857,
    -866,-875,-883,-891,-899,-906,-914,-921,-927,-934,
    -940,-946,-951,-956,-961,-966,-970,-974,-978,-982,
    -985,-988,-990,-993,-995,-996,-998,-999,-999,-1000,
    -1000,-1000,-999,-999,-998,-996,-995,-993,-990,-988,
    -985,-982,-978,-974,-970,-966,-961,-956,-951,-946,
    -940,-934,-927,-921,-914,-906,-899,-891,-883,-875,
    -866,-857,-848,-839,-829,-819,-809,-799,-788,-777,
    -766,-755,-743,-731,-719,-707,-695,-682,-669,-656,
    -643,-629,-616,-602,-588,-574,-559,-545,-530,-515,
    -500,-485,-469,-454,-438,-423,-407,-391,-375,-358,
    -342,-326,-309,-292,-276,-259,-242,-225,-208,-191,
    -174,-156,-139,-122,-105, -87, -70, -52, -35, -17
};

static int isin(int deg)
{
    return sin360[((deg % 360) + 360) % 360];
}
static int icos(int deg)
{
    return sin360[((deg + 90) % 360 + 360) % 360];
}

/* ── Types & primitives ────────────────────────────────────────── */
struct color { uint8_t r, g, b; };

static void rs_fill_rect(struct nwindow *nw,
                          int x, int y, int w, int h,
                          struct color c)
{
    if (w <= 0 || h <= 0) return;
    nw_fgcolor(nw, c.r, c.g, c.b);
    nw_rect(nw, x, y, w, h);
}

/* ============================================================
 * draw_arc
 * erase=0 → paint white dots along the arc
 * erase=1 → paint padded black rects to wipe the previous arc
 * ============================================================ */
static void draw_arc(struct nwindow *nw, int cx, int cy,
                     int start_deg, int len_deg, int erase)
{
    struct color white = {255, 255, 255};
    struct color black = {  0,   0,   0};
    struct color c     = erase ? black : white;

    int pad  = erase ? ERASE_PAD : 0;
    int size = ARC_STROKE + 2 * pad;

    if (len_deg <= 0) return;

    for (int a = 0; a <= len_deg; a += ARC_DRAW_STEP) {
        int angle = start_deg + a;
        int px    = cx + (RING_RADIUS * isin(angle)) / 1000;
        int py    = cy - (RING_RADIUS * icos(angle)) / 1000;
        rs_fill_rect(nw,
                     px - size / 2,
                     py - size / 2,
                     size, size, c);
    }
}

/* ============================================================
 * draw_static
 * Full black background + "Shutting down" text — called ONCE.
 * ============================================================ */
static void draw_static(struct nwindow *nw, int sw, int sh,
                         int cx, int cy)
{
    struct color black = {0, 0, 0};
    struct color white = {255, 255, 255};

    rs_fill_rect(nw, 0, 0, sw, sh, black);

    /* "Shutting down" = 12 chars (includes space) */
    int text_x = cx - (12 * CHAR_W / 2);
    int text_y = cy + RING_RADIUS + ARC_STROKE / 2 + TEXT_GAP;

    nw_fgcolor(nw, white.r, white.g, white.b);
    nw_string(nw, text_x, text_y, "Shutting down");
}

/* ── Hardware helpers ──────────────────────────────────────────── */
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

static void disable_mouse_interrupt(void) {
    unsigned char mask;
    __asm__ __volatile__ ("inb $0xA1, %%al" : "=a"(mask));
    mask |= 0x10;
    __asm__ __volatile__ ("outb %%al, $0xA1" : : "a"(mask));
}

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

/* ============================================================
 * Entry point
 * ============================================================ */
int main(int argc, char *argv[])
{
    disable_mouse_interrupt();

    struct nwindow *nw = nw_create_default();
    if (!nw) {
        printf("Failed to create window\n");
        return 1;
    }

    int sw, sh;
    read_boot_dimensions(&sw, &sh);
    int cx = sw / 2;
    int cy = sh / 2;

    /* ── One-time static paint: background + text ── */
    draw_static(nw, sw, sh, cx, cy);
    nw_flush(nw);

    int total_steps = ANIM_TOTAL_MS / ANIM_STEP_MS;
    int prev_start  = 0;
    int prev_len    = 0;
    int first_frame = 1;

    for (int i = 0; i < total_steps; i++) {
        int step  = i % ANIM_CYCLE_STEPS;
        int start = (-90 + 15 * step) + arc_offset_tab[step];
        int len   = arc_len_tab[step];

        if (!first_frame)
            draw_arc(nw, cx, cy, prev_start, prev_len, 1); /* erase */

        draw_arc(nw, cx, cy, start, len, 0);               /* draw  */

        nw_flush(nw);
        first_frame = 0;

        prev_start = start;
        prev_len   = len;

        syscall_process_sleep(ANIM_STEP_MS);
    }

    shutdown();
    return 0;
}