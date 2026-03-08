/*
Copyright (C) 2016-2019 The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file LICENSE for details.

All modifications are copyrighted to XPDevs and James Turner
XPDevs and James Turner use basekernel as a base where mods are added and updated
for the NexShell kernel

*/

#include "GUI.h"
#include "printf.h"
#include "graphics.h"
#include "string.h"
#include "interrupt.h"
#include "console.h"
#include "kernelcore.h"
#include "kmalloc.h"
#include "memorylayout.h"
#include "fs.h"

extern int ms_mx;
extern int ms_my;
extern int ms_left;
extern int ms_middle;
extern void mouse_refresh();
extern void mouse_set_cursor(int type);
extern void FORCE_MENU();

static void fill_rect(struct graphics*g,int x,int y,int w,int h,struct graphics_color c);
static void draw_rounded_rect(struct graphics*g,int x,int y,int w,int h,int r,struct graphics_color c);
static void draw_tga(struct graphics*g,const char*path,int dx,int dy,int mw,int mh);
static void draw_fallback_logo(struct graphics*g, int lx, int ly);


/* ============================================================
 * Screen dimensions
 * ============================================================ */
int boot_screen_w = 0;
int boot_screen_h = 0;

static void read_boot_dimensions(void) {
    uint16_t w, h;

    // Read directly from BGA ports to ensure accuracy, matching main.c
    asm volatile (
        "movw $0x1, %%ax\n\t"
        "movw $0x1CE, %%dx\n\t"
        "outw %%ax, %%dx\n\t"
        "movw $0x1D0, %%dx\n\t"
        "inw %%dx, %%ax"
        : "=a"(w) : : "dx"
    );
    asm volatile (
        "movw $0x2, %%ax\n\t"
        "movw $0x1CE, %%dx\n\t"
        "outw %%ax, %%dx\n\t"
        "movw $0x1D0, %%dx\n\t"
        "inw %%dx, %%ax"
        : "=a"(h) : : "dx"
    );

    if (w >= 320 && w <= 4096 && h >= 200 && h <= 4096) {
        boot_screen_w = w;
        boot_screen_h = h;
    } else {
        boot_screen_w = 1024; boot_screen_h = 768;
    }
}

/* ============================================================
 * Boot screen layout constants.
 * These mirror every hard-coded value from the original HTML/CSS
 * so the visual result is pixel-identical.
 *
 *   .logo        { width:78px; height:78px; margin-bottom:72px }
 *   .container   { width:215px }
 *   .progress-*  { height:4px; border-radius:10px }
 *   body         { background:#000 }
 *   .container   { transform:translateY(-15px) }   ← centre bias
 *
 * fade_step counts from 0 (fully visible) to BOOT_FADE_STEPS
 * (fully black).  Each step blacks out one more row in every
 * BOOT_FADE_STEPS-row tile, giving a smooth scanline fade that
 * needs no alpha-blending support from the graphics driver.
 * ============================================================ */
#define BOOT_LOGO_W        78
#define BOOT_LOGO_H        78
#define BOOT_CONTAINER_W  215
#define BOOT_BAR_H          4
#define BOOT_BAR_RADIUS    10
#define BOOT_LOGO_MARGIN   72   /* px between logo bottom and bar top */
#define BOOT_CENTRE_BIAS  -15   /* translateY(-15px) from the CSS     */
#define BOOT_FADE_STEPS    16   /* number of scanline-fade increments  */

static void draw_boot_screen(struct graphics *g, int sw, int sh,
                              int progress, int fade_step, int partial)
{
    struct graphics_color black  = {  0,   0,   0, 0};
    struct graphics_color bar_bg = {0x33,0x33,0x33, 0};  /* CSS #333333 */
    struct graphics_color bar_fg = {0xFF,0xFF,0xFF, 0};  /* CSS #ffffff */

    /* ---- Layout (matches the flex-column, justify-content:center CSS) ---- */
    int content_h   = BOOT_LOGO_H + BOOT_LOGO_MARGIN + BOOT_BAR_H;
    int container_x = (sw - BOOT_CONTAINER_W) / 2;
    int container_y = (sh - content_h) / 2 + BOOT_CENTRE_BIAS;
    if(container_y < 0) container_y = 0;

    /* Logo: horizontally centred inside the 215 px container */
    int logo_x = container_x + (BOOT_CONTAINER_W - BOOT_LOGO_W) / 2;
    int logo_y = container_y;

    /* Progress bar: full container width, below the logo gap */
    int bar_x  = container_x;
    int bar_y  = container_y + BOOT_LOGO_H + BOOT_LOGO_MARGIN;
    int fill_w = (BOOT_CONTAINER_W * progress) / 100;
    if(fill_w < 0) fill_w = 0;

    if (!partial) {
        /* Black background — always painted first */
        fill_rect(g, 0, 0, sw, sh, black);

        /* Nothing more to draw once the fade is complete */
        if(fade_step >= BOOT_FADE_STEPS) return;

        /* ---- Draw logo ----
         * draw_tga is currently a stub; draw_fallback_logo renders the 2×2
         * leaf-tile logo (yellow/blue/red/black) whenever the TGA is absent. */
        draw_fallback_logo(g, logo_x, logo_y);

        /* ---- Draw progress track (#333333 background) ---- */
        draw_rounded_rect(g, bar_x, bar_y,
                          BOOT_CONTAINER_W, BOOT_BAR_H,
                          BOOT_BAR_RADIUS, bar_bg);
    }

    /* ---- Draw progress fill (#ffffff, grows left→right) ----
       Only draw if not fully faded out. If partial and fading, we skip this. */
    if((!partial || fade_step == 0) && fill_w > 0 && fade_step < BOOT_FADE_STEPS)
        draw_rounded_rect(g, bar_x, bar_y,
                          fill_w, BOOT_BAR_H,
                          BOOT_BAR_RADIUS, bar_fg);

    /* ---- Scanline fade-to-black overlay ----
     *
     * Divides every vertical tile of BOOT_FADE_STEPS rows into
     * "black" and "visible" zones.  At fade_step=N the first N
     * rows of each tile are overwritten with black, so:
     *
     *   fade_step  0 → 0/16 rows black  → 100% visible
     *   fade_step  4 → 4/16 rows black  →  75% visible
     *   fade_step  8 → 8/16 rows black  →  50% visible
     *   fade_step 12 →12/16 rows black  →  25% visible
     *   fade_step 16 → all black        →   0% visible (not reached here)
     *
     * This replicates the CSS `appleFadeOut` animation without
     * requiring alpha-blending in the graphics driver.
     */
    if(fade_step > 0){
        graphics_fgcolor(g, black);
        for(int y = 0; y < sh; y++){
            if (partial) {
                /* Draw only new lines */
                if((y & (BOOT_FADE_STEPS - 1)) == fade_step - 1)
                    graphics_line(g, 0, y, sw, 0);
            } else {
                /* Draw all lines up to current step */
                if((y & (BOOT_FADE_STEPS - 1)) < fade_step)
                    graphics_line(g, 0, y, sw, 0);
            }
        }
    }
}
/* ============================================================
 * Color sentinel: alpha=255 means "not set / transparent".
 * All opaque CSS colours have alpha=0.
 * ============================================================ */
#define COLOR_TRANSPARENT ((struct graphics_color){0,0,0,255})
#define COLOR_IS_SET(c)   ((c).a != 255)

/* ============================================================
 * Drawing primitives
 * ============================================================ */
static void fill_rect(struct graphics*g,int x,int y,int w,int h,struct graphics_color c){
    if(w<=0||h<=0)return;
    graphics_fgcolor(g,c);graphics_rect(g,x,y,w,h);
}
static void draw_rounded_rect(struct graphics*g,int x,int y,int w,int h,int r,struct graphics_color c){
    if(w<=0||h<=0)return;
    if(r<=0){fill_rect(g,x,y,w,h,c);return;}
    if(r>w/2) r=w/2;
    if(r>h/2) r=h/2;
    graphics_fgcolor(g,c);
    graphics_rect(g,x+r,y,w-2*r,h);
    graphics_rect(g,x,y+r,r,h-2*r);
    graphics_rect(g,x+w-r,y+r,r,h-2*r);
    for(int dy=0;dy<r;dy++){
        int span=r-dy;if(span<0)span=0;
        graphics_rect(g,x+r-span,y+r-dy-1,span,1);
        graphics_rect(g,x+w-r,   y+r-dy-1,span,1);
        graphics_rect(g,x+r-span,y+h-r+dy,span,1);
        graphics_rect(g,x+w-r,   y+h-r+dy,span,1);
    }
}
static void draw_circle(struct graphics*g,int x,int y,int d,struct graphics_color c){
    if(d<=0) return;
    int r=d/2,cx=x+r,cy=y+r; graphics_fgcolor(g,c);
    for(int dy=-r;dy<=r;dy++){int dx2=r*r-dy*dy;if(dx2<0)dx2=0;int sx=0,s2=dx2;while(s2>0){sx++;s2-=2*sx-1;}if(sx>0)graphics_rect(g,cx-sx,cy+dy,2*sx,1);}
}
static void draw_border_box(struct graphics*g,int x,int y,int w,int h,int bw,struct graphics_color bc){
    if(bw<=0||!COLOR_IS_SET(bc)||w<=0||h<=0)return;
    graphics_fgcolor(g,bc);
    for(int i=0;i<bw;i++){graphics_line(g,x,y+i,w,0);graphics_line(g,x,y+h-bw+i,w,0);graphics_line(g,x+i,y,0,h);graphics_line(g,x+w-bw+i,y,0,h);}
}
static void draw_text(struct graphics*g,int*x,int y,const char*text,
                      struct graphics_color fg,int bold,int italic,int ul,int st,int ol,int scale,int lsp){
    int cw=(scale>0)?16:(scale<0?6:8);cw+=lsp;
    if(!g){*x+=cw*(int)strlen(text);return;}
    graphics_fgcolor(g,fg);
    while(*text){
        graphics_char(g,*x,y,*text);
        if(bold)graphics_char(g,*x+1,y,*text);
        if(scale>0){graphics_char(g,*x,y+1,*text);graphics_char(g,*x+1,y+1,*text);}
        if(ul)graphics_line(g,*x,y+14,cw,0);
        if(st)graphics_line(g,*x,y+7, cw,0);
        if(ol)graphics_line(g,*x,y,   cw,0);
        *x+=cw;text++;
    }
}

/* ============================================================
 * Integer square-root (Babylonian method) — no libm needed.
 * ============================================================ */
static int isqrt_i(int n){
    if(n<=0)return 0;
    int x=n,y=(x+1)/2;
    while(y<x){x=y;y=(x+n/x)/2;}
    return x;
}

/* ============================================================
 * draw_logo_tile — filled rectangle with independent per-corner
 * radii, implemented as row-by-row clipping.
 *
 *  r_tl / r_tr / r_br / r_bl  are the radii in pixels for
 *  top-left, top-right, bottom-right, bottom-left corners.
 *  Pass 0 for a sharp (square) corner.
 *
 * The algorithm places the circle centre at (r, r) inside each
 * corner zone and clips pixels that lie outside the circle:
 *   left boundary (TL/BL):  x_start = r - sqrt(r² - dy²)
 *   right boundary (TR/BR): x_end   = (ts-r) + sqrt(r² - dy²)
 * where dy is the row's distance from that corner's circle centre.
 * ============================================================ */
static void draw_logo_tile(struct graphics*g,
                            int tx, int ty, int ts,
                            int r_tl, int r_tr, int r_br, int r_bl,
                            struct graphics_color col)
{
    if(ts<=0)return;
    graphics_fgcolor(g,col);
    for(int row=0;row<ts;row++){
        int x1=0, x2=ts;

        /* Top-left */
        if(r_tl>0 && row<r_tl){
            int dy=r_tl-row;          /* distance above circle centre */
            int dd=r_tl*r_tl-dy*dy;
            int sq=(dd>0)?isqrt_i(dd):0;
            int b=r_tl-sq;
            if(b>x1)x1=b;
        }
        /* Top-right */
        if(r_tr>0 && row<r_tr){
            int dy=r_tr-row;
            int dd=r_tr*r_tr-dy*dy;
            int sq=(dd>0)?isqrt_i(dd):0;
            int b=(ts-r_tr)+sq;
            if(b<x2)x2=b;
        }
        /* Bottom-left */
        if(r_bl>0 && row>=(ts-r_bl)){
            int dy=row-(ts-r_bl);     /* distance below circle centre */
            int dd=r_bl*r_bl-dy*dy;
            int sq=(dd>0)?isqrt_i(dd):0;
            int b=r_bl-sq;
            if(b>x1)x1=b;
        }
        /* Bottom-right */
        if(r_br>0 && row>=(ts-r_br)){
            int dy=row-(ts-r_br);
            int dd=r_br*r_br-dy*dy;
            int sq=(dd>0)?isqrt_i(dd):0;
            int b=(ts-r_br)+sq;
            if(b<x2)x2=b;
        }

        if(x2>x1)
            graphics_rect(g, tx+x1, ty+row, x2-x1, 1);
    }
}

/* ============================================================
 * draw_fallback_logo — drawn when /boot/boot_logo.tga is absent
 * or unreadable (draw_tga is currently a stub).
 *
 * Replicates the HTML/CSS 2×2 "leaf-tile" logo:
 *
 *   ┌──────────┬──────────┐
 *   │  Yellow  │   Blue   │   border-radius: 25% 25%  0% 25%
 *   │ #eebc2c  │ #004ecc  │                  25% 25% 25%  0%
 *   ├──────────┼──────────┤
 *   │   Red    │  Black   │                  25%  0% 25% 25%
 *   │ #d31d25  │ #1a1a1a  │                   0% 25% 25% 25%
 *   └──────────┴──────────┘
 *
 * The total bounding box is BOOT_LOGO_W × BOOT_LOGO_H (78×78 px).
 * tile_size = (78 - gap) / 2 = 35 px,  gap = 8 px.
 * 25 % of 35 ≈ 9 px corner radius.
 *
 * CSS border-radius shorthand order: TL  TR  BR  BL
 * ============================================================ */
static void draw_fallback_logo(struct graphics*g, int lx, int ly)
{
    /* Layout */
    const int gap  = 8;
    const int ts   = (BOOT_LOGO_W - gap) / 2;   /* tile size = 35 px */
    const int r    = (ts * 25 + 50) / 100;       /* 25 % ≈ 9 px       */

    /* Colours */
    struct graphics_color yellow = {0xEE, 0xBC, 0x2C, 0};
    struct graphics_color blue   = {0x00, 0x4E, 0xCC, 0};
    struct graphics_color red    = {0xD3, 0x1D, 0x25, 0};
    struct graphics_color black  = {0x1A, 0x1A, 0x1A, 0};

    /* Tile origins */
    int tlx = lx,          tly = ly;
    int trx = lx + ts+gap, try2= ly;
    int blx = lx,          bly = ly + ts+gap;
    int brx = lx + ts+gap, bry = ly + ts+gap;

    /* Draw each tile with its unique corner radii
     * (CSS order: TL TR BR BL) */
    draw_logo_tile(g, tlx, tly, ts,  r, r, 0, r, yellow); /* TL: sharp BR      */
    draw_logo_tile(g, trx, try2,ts,  r, r, r, 0, blue);   /* TR: sharp BL      */
    draw_logo_tile(g, blx, bly, ts,  r, 0, r, r, red);    /* BL: sharp TR      */
    draw_logo_tile(g, brx, bry, ts,  0, r, r, r, black);  /* BR: sharp TL      */
}

/* ============================================================
 * TGA renderer  (stub — draw_tga is not yet implemented;
 * draw_boot_screen calls draw_fallback_logo instead)
 * ============================================================ */
struct tga_hdr{uint8_t id_len,cmap_type,img_type;uint8_t cmap[5];uint16_t xo,yo,w,h;uint8_t bpp,flags;}__attribute__((packed));
static void draw_tga(struct graphics*g,const char*path,int dx,int dy,int mw,int mh){
}
static int is_tga(const char*p){int l=(int)strlen(p);if(l<4)return 0;const char*e=p+l-4;return e[0]=='.'&&(e[1]=='t'||e[1]=='T')&&(e[2]=='g'||e[2]=='G')&&(e[3]=='a'||e[3]=='A');}
/* ============================================================
 * GUI entry point
 *
 * Boot animation state machine
 * ─────────────────────────────
 *  STATE 0  BOOT_INIT  – black screen before the bar appears (~3 s)
 *  STATE 1  BOOT_PROG  – progress bar runs 0 → 100 %
 *  STATE 2  BOOT_HOLD  – bar sits at 100 % for a moment (~1.5 s)
 *  STATE 3  BOOT_FADE  – scanline fade to black (~1.2 s, 16 steps)
 *  STATE 4  BOOT_DONE  – fully black; exit
 *
 * Tick constants are in units of interrupt_wait() calls.
 * Adjust BOOT_TICKS_PER_SEC to match your timer-interrupt rate.
 * Default assumes ~50 Hz (1 tick ≈ 20 ms).
 *
/* Progress phases (ticks between each +1 increment):
 *   0–19  %  fast start    (~1.2 s delay between increments)
 *  20–49  %  steady crawl  (~1.5 s)
 *  50–74  %  slowing down  (~1.8 s)
 *  75–91  %  "stuck" zone  (~2.5 s)
 *  92–99  %  final push    (~1.0 s)
 * ============================================================ */
#define BOOT_TICKS_PER_SEC   50
#define BOOT_INIT_TICKS      (BOOT_TICKS_PER_SEC * 1)        /* 1.0 s  */
#define BOOT_HOLD_TICKS      (BOOT_TICKS_PER_SEC / 2)        /* 0.5 s  */
#define BOOT_FADE_TICK_STEP  2                               /* ~40 ms per step */

/* progress phase delays: ticks between each +1 increment */
static int boot_phase_delay(int p){
    if(p < 20) return 1;
    if(p < 50) return 2;
    if(p < 75) return 2;
    if(p < 92) return 1;
    return 1;
}

int GUI(){
    if(total_memory < 128){
        printf("\f");printf("Warning: Inadequate RAM detected.\n");
        printf("128MB of RAM is recommended for the GUI to run correctly.\n");
        printf("Do you want to force run? (Y/N): ");
        while(1){char c=console_getchar(&console_root);
            if(c=='y'||c=='Y'){printf("%c\n",c);break;}
            else if(c=='n'||c=='N'){printf("%c\n",c);return 0;}
        }
    }
    interrupt_disable(44);printf("\f");read_boot_dimensions();

    struct graphics*g=&graphics_root;
    int sw=(boot_screen_w>0)?boot_screen_w:graphics_width(g);
    int sh=(boot_screen_h>0)?boot_screen_h:graphics_height(g);

    /* Boot animation state */
    int boot_state    = 0;   /* current state (0–4, see above)           */
    int boot_progress = 0;   /* bar fill percentage 0–100                */
    int boot_counter  = 0;   /* general tick counter for current state   */
    int boot_fade     = 0;   /* scanline-fade step 0–BOOT_FADE_STEPS     */
    int prog_counter  = 0;   /* ticks since last progress increment      */
    int needs_redraw  = 1;
    int partial_draw  = 0;

    /* Initial paint: pure black screen while interrupts are still off.
     * IRQ 44 (PS/2 mouse) stays DISABLED for the entire boot animation so
     * that mouse-move interrupts cannot wake interrupt_wait() and artificially
     * accelerate the tick counter.  The cursor is invisible until exit. */
    {struct graphics_color blk={0,0,0,0};
     graphics_fgcolor(g,blk);graphics_rect(g,0,0,sw,sh);}
    /* IRQ 44 remains disabled — do NOT enable it yet. */

    while(1){
        /* No per-iteration IRQ44 toggle; it stays off the entire boot. */

        switch(boot_state){

        case 0: /* ── INIT: black screen wait before bar appears ── */
            if(++boot_counter >= BOOT_INIT_TICKS){
                boot_state   = 1;
                boot_counter = 0;
                prog_counter = 0;
                needs_redraw = 1;
                partial_draw = 0;
            }
            break;

        case 1: /* ── PROG: progress bar 0 → 100 % ── */
            if(++prog_counter >= boot_phase_delay(boot_progress)){
                prog_counter = 0;
                if(++boot_progress > 100) boot_progress = 100;
                needs_redraw = 1;
                partial_draw = 1;
                if(boot_progress >= 100){
                    boot_state   = 2;
                    boot_counter = 0;
                }
            }
            break;

        case 2: /* ── HOLD: pause at 100 % before fading ── */
            if(++boot_counter >= BOOT_HOLD_TICKS){
                boot_state   = 3;
                boot_counter = 0;
                boot_fade    = 0;
                needs_redraw = 1;
                partial_draw = 1;
            }
            break;

        case 3: /* ── FADE: scanline fade to black ── */
            if(++boot_counter >= BOOT_FADE_TICK_STEP){
                boot_counter = 0;
                boot_fade++;
                needs_redraw = 1;
                partial_draw = 1;
                if(boot_fade >= BOOT_FADE_STEPS){
                    boot_state   = 4;
                    needs_redraw = 1;
                    partial_draw = 0;
                }
            }
            break;

        case 4: /* ── DONE: fully black, exit ── */
            draw_boot_screen(g, sw, sh, boot_progress, BOOT_FADE_STEPS, 0);
            goto exit;
        }

        if(needs_redraw){
            draw_boot_screen(g, sw, sh, boot_progress, boot_fade, partial_draw);
        }
        needs_redraw = 0;

        /* ESC or Ctrl-E skips straight to exit */
        char c;
        while(console_read_nonblock(&console_root,&c,1)){
            if(c==5||c==0x1B) goto exit;
        }
        /* Only timer (and keyboard) interrupts wake us here; mouse is off. */
        interrupt_wait();
    }
exit:
    /* Boot done. */
    interrupt_enable(44);
    mouse_set_cursor(0);

    /* Reset console to text mode defaults */
    struct graphics_color white = {255, 255, 255, 0};
    struct graphics_color black = {0, 0, 0, 0};
    console_set_color(white, black);
    printf("\f");

    return 0;
}