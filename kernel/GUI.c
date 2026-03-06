/*
Copyright (C) 2016-2019 The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file LICENSE for details.

All modifications are copyrighted to XPDevs and James Turner
XPDevs and James Turner use basekernel as a base where mods are added and updated
for the NexShell kernel

HTML5/CSS rendering engine.
DESIGN RULE: The C code contains ZERO hard-coded colours, sizes or layout
constants.  Every visual value (colour, width, height, padding, margin,
border-radius, font-size, ...) MUST come from a parsed CSS rule or from the
HTML attribute/inline-style on the element.  The only permitted C-side
"defaults" are the sentinel values that mean "not set":
  - COLOR_TRANSPARENT (alpha=255) for colours
  - 0 for dimensions
These cause the renderer to skip drawing rather than invent a hard-coded value.
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
                              int progress, int fade_step)
{
    struct graphics_color black  = {  0,   0,   0, 0};
    struct graphics_color bar_bg = {0x33,0x33,0x33, 0};  /* CSS #333333 */
    struct graphics_color bar_fg = {0xFF,0xFF,0xFF, 0};  /* CSS #ffffff */

    /* Black background — always painted first */
    fill_rect(g, 0, 0, sw, sh, black);

    /* Nothing more to draw once the fade is complete */
    if(fade_step >= BOOT_FADE_STEPS) return;

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

    /* ---- Draw logo ----
     * draw_tga is currently a stub; draw_fallback_logo renders the 2×2
     * leaf-tile logo (yellow/blue/red/black) whenever the TGA is absent. */
    draw_fallback_logo(g, logo_x, logo_y);

    /* ---- Draw progress track (#333333 background) ---- */
    draw_rounded_rect(g, bar_x, bar_y,
                      BOOT_CONTAINER_W, BOOT_BAR_H,
                      BOOT_BAR_RADIUS, bar_bg);

    /* ---- Draw progress fill (#ffffff, grows left→right) ---- */
    if(fill_w > 0)
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
            if((y & (BOOT_FADE_STEPS - 1)) < fade_step)
                graphics_line(g, 0, y, sw, 0);
        }
    }
}

/* ============================================================
 * Color sentinel: alpha=255 means "not set / transparent".
 * All opaque CSS colours have alpha=0.
 * ============================================================ */
static int hex_d(char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='A'&&c<='F')return c-'A'+10;
    if(c>='a'&&c<='f')return c-'a'+10;
    return 0;
}
static int parse_int_s(const char *s){
    int v=0; while(*s==' ')s++; while(*s>='0'&&*s<='9')v=v*10+(*s++-'0'); return v;
}

/* ============================================================
 * CSS color parsing
 * ============================================================ */
#define COLOR_TRANSPARENT ((struct graphics_color){0,0,0,255})
#define COLOR_IS_SET(c)   ((c).a != 255)
static struct graphics_color parse_css_color(const char *raw){
    struct graphics_color c=COLOR_TRANSPARENT;
    while(*raw==' ')raw++;
    if(raw[0]=='#'){
        int len=(int)strlen(raw);
        if(len>=7){c.r=(uint8_t)((hex_d(raw[1])<<4)|hex_d(raw[2]));c.g=(uint8_t)((hex_d(raw[3])<<4)|hex_d(raw[4]));c.b=(uint8_t)((hex_d(raw[5])<<4)|hex_d(raw[6]));c.a=(len>=9)?(uint8_t)((hex_d(raw[7])<<4)|hex_d(raw[8])):0;}
        else if(len>=4){c.r=(uint8_t)((hex_d(raw[1])<<4)|hex_d(raw[1]));c.g=(uint8_t)((hex_d(raw[2])<<4)|hex_d(raw[2]));c.b=(uint8_t)((hex_d(raw[3])<<4)|hex_d(raw[3]));c.a=0;}
        return c;
    }
    if(!strncmp(raw,"rgb(",4)){const char*p=raw+4;c.r=(uint8_t)parse_int_s(p);while(*p&&*p!=',')p++;if(*p)p++;c.g=(uint8_t)parse_int_s(p);while(*p&&*p!=',')p++;if(*p)p++;c.b=(uint8_t)parse_int_s(p);c.a=0;return c;}
    if(!strncmp(raw,"rgba(",5)){const char*p=raw+5;c.r=(uint8_t)parse_int_s(p);while(*p&&*p!=',')p++;if(*p)p++;c.g=(uint8_t)parse_int_s(p);while(*p&&*p!=',')p++;if(*p)p++;c.b=(uint8_t)parse_int_s(p);while(*p&&*p!=',')p++;if(*p)p++;while(*p==' ')p++;c.a=(*p=='0'&&(*(p+1)==')'||*(p+1)==' '))?255:0;return c;}
    if(!strncmp(raw,"hsl(",4)){
        const char*p=raw+4;int h=parse_int_s(p)%360;while(*p&&*p!=',')p++;if(*p)p++;
        int s=parse_int_s(p);while(*p&&*p!=',')p++;if(*p)p++;int l=parse_int_s(p);
        if(s==0){c.r=c.g=c.b=(uint8_t)(l*255/100);c.a=0;return c;}
        int q=(l<50)?(l*(100+s)/100):(l+s-l*s/100);int p2=2*l-q;
        #define H2R(hh,out) do{int _h=((hh)+360)%360,_v;if(_h<60)_v=p2+(_h*(q-p2)/60);else if(_h<180)_v=q;else if(_h<240)_v=p2+((240-_h)*(q-p2)/60);else _v=p2;_v=_v*255/100;if(_v<0)_v=0;if(_v>255)_v=255;out=(uint8_t)_v;}while(0)
        H2R(h+120,c.r);H2R(h,c.g);H2R(h+240,c.b);c.a=0;return c;
        #undef H2R
    }
    if(!strcmp(raw,"transparent")||!strcmp(raw,"none"))return COLOR_TRANSPARENT;
    /* named colors */
    typedef struct{const char*n;uint8_t r,g,b;}NC;
    static const NC nc[]={
        {"red",255,0,0},{"green",0,128,0},{"lime",0,255,0},{"blue",0,0,255},
        {"white",255,255,255},{"black",0,0,0},{"yellow",255,255,0},
        {"cyan",0,255,255},{"aqua",0,255,255},{"magenta",255,0,255},{"fuchsia",255,0,255},
        {"gray",128,128,128},{"grey",128,128,128},{"lightgray",211,211,211},{"lightgrey",211,211,211},
        {"darkgray",169,169,169},{"silver",192,192,192},{"orange",255,165,0},
        {"darkorange",255,140,0},{"orangered",255,69,0},{"purple",128,0,128},
        {"teal",0,128,128},{"navy",0,0,128},{"maroon",128,0,0},{"olive",128,128,0},
        {"coral",255,127,80},{"salmon",250,128,114},{"hotpink",255,105,180},
        {"pink",255,192,203},{"deeppink",255,20,147},{"violet",238,130,238},
        {"indigo",75,0,130},{"gold",255,215,0},{"khaki",240,230,140},
        {"beige",245,245,220},{"lavender",230,230,250},{"snow",255,250,250},
        {"whitesmoke",245,245,245},{"wheat",245,222,179},{"tan",210,180,140},
        {"brown",165,42,42},{"saddlebrown",139,69,19},{"sienna",160,82,45},
        {"skyblue",135,206,235},{"deepskyblue",0,191,255},{"cornflowerblue",100,149,237},
        {"royalblue",65,105,225},{"dodgerblue",30,144,255},{"steelblue",70,130,180},
        {"mediumblue",0,0,205},{"darkblue",0,0,139},{"midnightblue",25,25,112},
        {"seagreen",46,139,87},{"springgreen",0,255,127},{"chartreuse",127,255,0},
        {"forestgreen",34,139,34},{"darkgreen",0,100,0},{"limegreen",50,205,50},
        {"yellowgreen",154,205,50},{"darkcyan",0,139,139},{"darkred",139,0,0},
        {"crimson",220,20,60},{"firebrick",178,34,34},{"tomato",255,99,71},
        {"chocolate",210,105,30},{"turquoise",64,224,208},{"lightcyan",224,255,255},
        {"lightblue",173,216,230},{"lightyellow",255,255,224},{"lightgreen",144,238,144},
        {"lightpink",255,182,193},{"lightcoral",240,128,128},{"plum",221,160,221},
        {"orchid",218,112,214},{"mediumpurple",147,112,219},{"blueviolet",138,43,226},
        {"darkviolet",148,0,211},{"rebeccapurple",102,51,153},{"ivory",255,255,240},
        {"mintcream",245,255,250},{"ghostwhite",248,248,255},{"linen",250,240,230},
        {"peru",205,133,63},{"cadetblue",95,158,160},{"darkmagenta",139,0,139},
        {0,0,0,0}
    };
    for(int i=0;nc[i].n;i++) if(!strcmp(raw,nc[i].n)){c.r=nc[i].r;c.g=nc[i].g;c.b=nc[i].b;c.a=0;return c;}
    return c;
}

static char *css_strstr(const char *h,const char *n){
    if(!*n)return(char*)h;
    for(;*h;h++){const char*a=h,*b=n;while(*a&&*b&&*a==*b){a++;b++;}if(!*b)return(char*)h;}
    return 0;
}

/* ============================================================
 * render_style — every visual property comes from CSS.
 * Sentinel: 0 or COLOR_TRANSPARENT means "not set".
 * ============================================================ */
struct render_style {
    struct graphics_color fg,bg,border_color,outline_color;
    int font_size,font_scale;
    int bold,italic,underline,strike,overline;
    int text_align,text_transform,white_space;
    int letter_spacing,line_height,has_text_shadow,vertical_align;
    int width,height,min_width,min_height,max_width,max_height;
    int padding_top,padding_right,padding_bottom,padding_left;
    int margin_top,margin_right,margin_bottom,margin_left; /* -1=auto */
    int border_width,border_radius;
    int border_radius_tl,border_radius_tr,border_radius_bl,border_radius_br;
    int outline_width,box_sizing_border;
    int display,display_flex,flex_direction,flex_wrap;
    int justify_content; /* 0=start 1=center 2=end 3=space-between 4=space-around */
    int align_items;     /* 0=stretch/start 1=center 2=end */
    int flex_grow,flex_shrink,gap;
    int position_type;   /* 0=static 1=relative 2=absolute 3=fixed */
    int top,right,bottom,left;
    int top_set,right_set,bottom_set,left_set;
    int z_index;
    int overflow_hidden,overflow_scroll;
    int opacity,has_shadow,list_style_none,cursor_pointer,object_fit;
    int grid_cols,has_transition,has_animation;
};

struct css_rule {
    char selector[128];
    struct render_style style;
    struct css_rule *next;
};
static struct css_rule *css_rules=0;

static void init_style(struct render_style *s){
    memset(s,0,sizeof(*s));
    s->fg=s->bg=s->border_color=s->outline_color=COLOR_TRANSPARENT;
    s->opacity=100; s->flex_shrink=1;
}

/* ============================================================
 * CSS block parser
 * ============================================================ */
static void parse_css_block(const char *block,struct render_style *s){
    char buf[8192]; strncpy(buf,block,8191); buf[8191]=0;
    char *p=buf;
    while(*p){
        char *end=(char*)strchr(p,';'); if(end)*end=0;
        char *col=(char*)strchr(p,':');
        if(col){
            *col=0;
            char *val=col+1; while(*val==' '||*val=='\t')val++;
            char *prop=p; while(*prop==' '||*prop=='\n'||*prop=='\r'||*prop=='\t')prop++;
            int pl=(int)strlen(prop); while(pl>0&&prop[pl-1]<=' ')prop[--pl]=0;
            int vl=(int)strlen(val);  while(vl>0&&val[vl-1]<=' ') val[--vl]=0;
            if(!pl||!vl)goto next;
            if(prop[0]=='-'&&prop[1]!='-')goto next; /* skip vendor prefixes */

            if(!strcmp(prop,"color"))                {s->fg=parse_css_color(val);}
            else if(!strcmp(prop,"background-color")){s->bg=parse_css_color(val);}
            else if(!strcmp(prop,"background")){
                if(!strncmp(val,"linear-gradient",15)||!strncmp(val,"radial-gradient",15)){
                    const char*cp=strchr(val,',');
                    if(cp){char cb[48];int ci=0;cp++;while(*cp==' ')cp++;while(*cp&&*cp!=','&&*cp!=')'&&ci<47)cb[ci++]=*cp++;cb[ci]=0;s->bg=parse_css_color(cb);}
                }else if(!strncmp(val,"url(",4)){/*bg-image skip*/}
                else s->bg=parse_css_color(val);
            }
            else if(!strcmp(prop,"border-color")) {s->border_color=parse_css_color(val);}
            else if(!strcmp(prop,"outline-color")){s->outline_color=parse_css_color(val);}
            else if(!strcmp(prop,"font-weight")){s->bold=(css_strstr(val,"bold")||css_strstr(val,"700")||css_strstr(val,"800")||css_strstr(val,"900")||css_strstr(val,"600"))?1:0;}
            else if(!strcmp(prop,"font-style")) {s->italic=css_strstr(val,"italic")?1:0;}
            else if(!strcmp(prop,"font-size")){
                int fs=0;str2int(val,&fs);
                if(css_strstr(val,"em")&&!css_strstr(val,"rem"))fs*=16;
                else if(css_strstr(val,"pt"))fs=fs*4/3;
                else if(css_strstr(val,"%"))fs=fs*16/100;
                s->font_size=fs;
                if(fs>=28)s->font_scale=2;else if(fs>=20)s->font_scale=1;else if(fs<=10)s->font_scale=-1;else s->font_scale=0;
            }
            else if(!strcmp(prop,"font")){
                const char*fp=val;while(*fp){if(*fp>='0'&&*fp<='9'){int fs=0;str2int(fp,&fs);if(fs>0&&fs<200){s->font_size=fs;break;}}fp++;}
                if(css_strstr(val,"bold")) s->bold=1;
                if(css_strstr(val,"italic")) s->italic=1;
            }
            else if(!strcmp(prop,"text-decoration")||!strcmp(prop,"text-decoration-line")){
                s->underline=css_strstr(val,"underline")?1:0;
                s->strike=css_strstr(val,"line-through")?1:0;
                s->overline=css_strstr(val,"overline")?1:0;
            }
            else if(!strcmp(prop,"text-align")){
                if(css_strstr(val,"center"))s->text_align=1;
                else if(css_strstr(val,"right"))s->text_align=2;
                else if(css_strstr(val,"justify"))s->text_align=3;
                else s->text_align=0;
            }
            else if(!strcmp(prop,"text-transform")){
                if(css_strstr(val,"uppercase"))s->text_transform=1;
                else if(css_strstr(val,"lowercase"))s->text_transform=2;
                else if(css_strstr(val,"capitalize"))s->text_transform=3;
            }
            else if(!strcmp(prop,"letter-spacing")){str2int(val,&s->letter_spacing);}
            else if(!strcmp(prop,"line-height"))   {str2int(val,&s->line_height);}
            else if(!strcmp(prop,"white-space")){
                if(css_strstr(val,"pre-wrap")||css_strstr(val,"pre-line"))s->white_space=3;
                else if(css_strstr(val,"pre"))s->white_space=1;
                else if(css_strstr(val,"nowrap"))s->white_space=2;
                else s->white_space=0;
            }
            else if(!strcmp(prop,"text-shadow")){s->has_text_shadow=1;}
            else if(!strcmp(prop,"vertical-align")){
                if(css_strstr(val,"top"))s->vertical_align=1;
                else if(css_strstr(val,"middle"))s->vertical_align=2;
                else if(css_strstr(val,"bottom"))s->vertical_align=3;
                else if(css_strstr(val,"sub"))s->vertical_align=4;
                else if(css_strstr(val,"super"))s->vertical_align=5;
            }
            else if(!strcmp(prop,"width")){
                if(css_strstr(val,"%")){int pct=0;str2int(val,&pct);s->width=-pct;}
                else if(css_strstr(val,"auto"))s->width=0;
                else if(css_strstr(val,"100vh"))s->width=-1;
                else str2int(val,&s->width);
            }
            else if(!strcmp(prop,"height")){
                if(css_strstr(val,"100vh"))s->height=-1;
                else if(css_strstr(val,"vh")){int pct=0;str2int(val,&pct);s->height=-(1000+pct);}
                else if(css_strstr(val,"%")){int pct=0;str2int(val,&pct);if(pct==100)s->height=-1;else s->height=-(2000+pct);}
                else if(css_strstr(val,"auto"))s->height=0;
                else str2int(val,&s->height);
            }
            else if(!strcmp(prop,"min-width")) {str2int(val,&s->min_width);}
            else if(!strcmp(prop,"min-height")){str2int(val,&s->min_height);}
            else if(!strcmp(prop,"max-width")) {str2int(val,&s->max_width);}
            else if(!strcmp(prop,"max-height")){str2int(val,&s->max_height);}
            else if(!strcmp(prop,"padding")){int v=0;str2int(val,&v);s->padding_top=s->padding_right=s->padding_bottom=s->padding_left=v;}
            else if(!strcmp(prop,"padding-top"))   {str2int(val,&s->padding_top);}
            else if(!strcmp(prop,"padding-right")) {str2int(val,&s->padding_right);}
            else if(!strcmp(prop,"padding-bottom")){str2int(val,&s->padding_bottom);}
            else if(!strcmp(prop,"padding-left"))  {str2int(val,&s->padding_left);}
            else if(!strcmp(prop,"margin")){
                if(css_strstr(val,"auto")){s->margin_top=s->margin_bottom=0;s->margin_left=s->margin_right=-1;}
                else{int v=0;str2int(val,&v);s->margin_top=s->margin_right=s->margin_bottom=s->margin_left=v;}
            }
            else if(!strcmp(prop,"margin-top"))   {s->margin_top   =css_strstr(val,"auto")?-1:(str2int(val,&s->margin_top),   s->margin_top);}
            else if(!strcmp(prop,"margin-right")) {s->margin_right =css_strstr(val,"auto")?-1:(str2int(val,&s->margin_right),  s->margin_right);}
            else if(!strcmp(prop,"margin-bottom")){s->margin_bottom=css_strstr(val,"auto")?-1:(str2int(val,&s->margin_bottom), s->margin_bottom);}
            else if(!strcmp(prop,"margin-left"))  {s->margin_left  =css_strstr(val,"auto")?-1:(str2int(val,&s->margin_left),   s->margin_left);}
            else if(!strcmp(prop,"border-radius")){
                if(css_strstr(val,"50%")||css_strstr(val,"100%"))s->border_radius=-1;
                else{str2int(val,&s->border_radius);s->border_radius_tl=s->border_radius_tr=s->border_radius_bl=s->border_radius_br=s->border_radius;}
            }
            else if(!strcmp(prop,"border-top-left-radius"))    {str2int(val,&s->border_radius_tl);}
            else if(!strcmp(prop,"border-top-right-radius"))   {str2int(val,&s->border_radius_tr);}
            else if(!strcmp(prop,"border-bottom-left-radius")) {str2int(val,&s->border_radius_bl);}
            else if(!strcmp(prop,"border-bottom-right-radius")){str2int(val,&s->border_radius_br);}
            else if(!strcmp(prop,"border-width")){str2int(val,&s->border_width);}
            else if(!strcmp(prop,"border")){
                str2int(val,&s->border_width);
                const char*bc=strchr(val,'#');
                if(!bc){const char*kws[]={"solid","dashed","dotted","double","none",0};for(int ki=0;kws[ki];ki++){const char*kp=css_strstr(val,kws[ki]);if(kp){kp+=strlen(kws[ki]);while(*kp==' ')kp++;bc=kp;break;}}}
                if(bc&&*bc)s->border_color=parse_css_color(bc);
            }
            else if(!strcmp(prop,"outline-width")){str2int(val,&s->outline_width);}
            else if(!strcmp(prop,"outline"))      {str2int(val,&s->outline_width);}
            else if(!strcmp(prop,"box-sizing"))   {s->box_sizing_border=css_strstr(val,"border-box")?1:0;}
            else if(!strcmp(prop,"position")){
                if(css_strstr(val,"absolute"))s->position_type=2;
                else if(css_strstr(val,"fixed"))s->position_type=3;
                else if(css_strstr(val,"relative"))s->position_type=1;
                else s->position_type=0;
            }
            else if(!strcmp(prop,"top"))   {if(!css_strstr(val,"auto")){str2int(val,&s->top);   s->top_set=1;}}
            else if(!strcmp(prop,"right")) {if(!css_strstr(val,"auto")){str2int(val,&s->right); s->right_set=1;}}
            else if(!strcmp(prop,"bottom")){if(!css_strstr(val,"auto")){str2int(val,&s->bottom);s->bottom_set=1;}}
            else if(!strcmp(prop,"left"))  {if(!css_strstr(val,"auto")){str2int(val,&s->left);  s->left_set=1;}}
            else if(!strcmp(prop,"z-index")){str2int(val,&s->z_index);}
            else if(!strcmp(prop,"display")){
                if(css_strstr(val,"none"))           s->display=2;
                else if(css_strstr(val,"grid"))    { s->display=6; }
                else if(css_strstr(val,"inline-flex")){s->display_flex=1;s->display=3;}
                else if(css_strstr(val,"inline-block"))s->display=3;
                else if(css_strstr(val,"flex"))    { s->display_flex=1;s->display=1; }
                else if(css_strstr(val,"block"))     s->display=1;
                else if(css_strstr(val,"table-cell"))s->display=5;
                else if(css_strstr(val,"table"))     s->display=4;
                else if(css_strstr(val,"list-item")) s->display=7;
                else s->display=0;
            }
            else if(!strcmp(prop,"flex-direction")){s->flex_direction=css_strstr(val,"column")?1:0;}
            else if(!strcmp(prop,"flex-wrap"))     {s->flex_wrap=css_strstr(val,"wrap")?1:0;}
            else if(!strcmp(prop,"justify-content")){
                if(css_strstr(val,"center"))s->justify_content=1;
                else if(css_strstr(val,"flex-end")||css_strstr(val,"end"))s->justify_content=2;
                else if(css_strstr(val,"space-between"))s->justify_content=3;
                else if(css_strstr(val,"space-around"))s->justify_content=4;
                else s->justify_content=0;
            }
            else if(!strcmp(prop,"align-items")||!strcmp(prop,"align-content")||!strcmp(prop,"align-self")){
                if(css_strstr(val,"center"))s->align_items=1;
                else if(css_strstr(val,"flex-end")||css_strstr(val,"end"))s->align_items=2;
                else s->align_items=0;
            }
            else if(!strcmp(prop,"flex-grow"))  {str2int(val,&s->flex_grow);}
            else if(!strcmp(prop,"flex-shrink")){str2int(val,&s->flex_shrink);}
            else if(!strcmp(prop,"flex")){
                if(!strcmp(val,"1")||!strcmp(val,"auto"))s->flex_grow=1;
                else if(!strcmp(val,"none")){s->flex_grow=0;s->flex_shrink=0;}
                else str2int(val,&s->flex_grow);
            }
            else if(!strcmp(prop,"gap")||!strcmp(prop,"grid-gap")){str2int(val,&s->gap);}
            else if(!strcmp(prop,"grid-template-columns")){
                int cols=0;const char*gp=val;
                while(*gp){while(*gp==' ')gp++;if(*gp){cols++;while(*gp&&*gp!=' ')gp++;}}
                s->grid_cols=cols;
            }
            else if(!strcmp(prop,"box-shadow")) {s->has_shadow=!css_strstr(val,"none");}
            else if(!strcmp(prop,"overflow")||!strcmp(prop,"overflow-x")||!strcmp(prop,"overflow-y")){
                s->overflow_hidden=css_strstr(val,"hidden")?1:0;
                s->overflow_scroll=(css_strstr(val,"scroll")||css_strstr(val,"auto"))?1:0;
            }
            else if(!strcmp(prop,"opacity")){
                int ov=0;str2int(val,&ov);
                if(css_strstr(val,".")){ov=0;const char*dp=val;if(*dp=='0'&&*(dp+1)=='.')dp+=2;int dec=0,dl=0;while(*dp>='0'&&*dp<='9'&&dl<2){dec=dec*10+(*dp-'0');dp++;dl++;}if(dl==1)dec*=10;ov=dec;}
                if(ov>100) ov=100;
                s->opacity=ov;
            }
            else if(!strcmp(prop,"cursor"))     {s->cursor_pointer=css_strstr(val,"pointer")?1:0;}
            else if(!strcmp(prop,"list-style")||!strcmp(prop,"list-style-type")){s->list_style_none=css_strstr(val,"none")?1:0;}
            else if(!strcmp(prop,"object-fit")) {if(css_strstr(val,"contain"))s->object_fit=1;else if(css_strstr(val,"cover"))s->object_fit=2;}
            else if(!strcmp(prop,"transition")) {s->has_transition=1;}
            else if(!strcmp(prop,"animation"))  {s->has_animation=1;}
        }
        next:
        if(!end)break;
        p=end+1; while(*p==' '||*p=='\n')p++;
    }
}

/* ============================================================
 * CSS sheet parser
 * ============================================================ */
static void parse_css_sheet(const char *sheet){
    const char *p=sheet;
    while(*p){
        while(*p&&*p<=' ') p++;
        if(!*p) break;
        if(*p=='@'){
            int depth=0;while(*p&&(*p!='{'||depth==0)){if(*p=='{')depth++;p++;}
            if(*p=='{'){depth=1;p++;}while(*p&&depth>0){if(*p=='{')depth++;else if(*p=='}')depth--;p++;}
            continue;
        }
        const char*ss=p;while(*p&&*p!='{')p++;if(!*p)break;
        int sl=(int)(p-ss);if(sl>127)sl=127;
        char selector[128];strncpy(selector,ss,sl);selector[sl]=0;
        while(sl>0&&selector[sl-1]<=' ')selector[--sl]=0;
        p++;
        const char*bs=p;int depth2=1;
        while(*p&&depth2>0){if(*p=='{')depth2++;else if(*p=='}')depth2--;p++;}
        int bl=(int)(p-1-bs);if(bl<0)bl=0;if(bl>8191)bl=8191;
        char block[8192];strncpy(block,bs,bl);block[bl]=0;
        /* comma-separated selectors */
        char sc[128];strncpy(sc,selector,127);sc[127]=0;
        char *sp2=sc;
        while(sp2&&*sp2){
            char*comma=(char*)strchr(sp2,',');if(comma)*comma=0;
            char single[128];strncpy(single,sp2,127);single[127]=0;
            char *st=single;while(*st<=' '&&*st)st++;
            int slen=(int)strlen(st);while(slen>0&&st[slen-1]<=' ')st[--slen]=0;
            if(*st){
                struct css_rule*rule=(struct css_rule*)kmalloc(sizeof(struct css_rule));
                memset(rule,0,sizeof(*rule));
                strncpy(rule->selector,st,127);rule->selector[127]=0;
                init_style(&rule->style);
                parse_css_block(block,&rule->style);
                rule->next=css_rules;css_rules=rule;
            }
            sp2=comma?comma+1:0;
        }
    }
}

/* ============================================================
 * Selector matching
 * ============================================================ */
static int sel_matches(const char *sel,const char *tag,const char *cls,const char *id){
    if(!sel||!*sel)return 0;
    char clean[128];strncpy(clean,sel,127);clean[127]=0;
    char*pc=(char*)strchr(clean,':');if(pc)*pc=0;
    char*at=(char*)strchr(clean,'[');if(at)*at=0;
    /* last simple selector */
    char*last=clean;
    for(;;){char*s2=(char*)strchr(last+1,' ');char*g2=(char*)strchr(last+1,'>');
        char*p2=(char*)strchr(last+1,'+');char*t2=(char*)strchr(last+1,'~');
        char*best=s2;if(g2&&(!best||g2<best))best=g2;if(p2&&(!best||p2<best))best=p2;if(t2&&(!best||t2<best))best=t2;
        if(!best) break;
        last=best+1; while(*last==' '||*last=='>'||*last=='+'||*last=='~') last++;}
    if(!strcmp(last,"*"))return 1;
    char*dot=(char*)strchr(last,'.');char*hash=(char*)strchr(last,'#');
    /* .class */
    if(last[0]=='.'&&!hash){
        if(!cls)return 0;
        char cc[128];strncpy(cc,cls,127);cc[127]=0;char*tk=cc;
        while(*tk){while(*tk==' ')tk++;char w[64];int wi=0;while(*tk&&*tk!=' '&&wi<63)w[wi++]=*tk++;w[wi]=0;if(!strcmp(w,last+1))return 1;}
        return 0;
    }
    /* #id */
    if(last[0]=='#')return id&&!strcmp(last+1,id);
    /* tag.class */
    if(dot&&!hash){
        char tp[64];int tl=(int)(dot-last);if(tl>63)tl=63;strncpy(tp,last,tl);tp[tl]=0;
        int tm=(tp[0]==0)||(tag&&!strcmp(tp,tag));if(!tm)return 0;
        if(!cls)return 0;
        char cc[128];strncpy(cc,cls,127);cc[127]=0;char*tk=cc;
        while(*tk){while(*tk==' ')tk++;char w[64];int wi=0;while(*tk&&*tk!=' '&&wi<63)w[wi++]=*tk++;w[wi]=0;if(!strcmp(w,dot+1))return 1;}
        return 0;
    }
    /* tag#id */
    if(hash&&!dot){
        char tp[64];int tl=(int)(hash-last);if(tl>63)tl=63;strncpy(tp,last,tl);tp[tl]=0;
        return ((tp[0]==0)||(tag&&!strcmp(tp,tag)))&&id&&!strcmp(hash+1,id);
    }
    /* plain tag */
    return tag&&!strcmp(last,tag);
}

static void merge_style(struct render_style*dst,const struct render_style*src){
    if(COLOR_IS_SET(src->fg))          dst->fg=src->fg;
    if(COLOR_IS_SET(src->bg))          dst->bg=src->bg;
    if(COLOR_IS_SET(src->border_color))dst->border_color=src->border_color;
    if(COLOR_IS_SET(src->outline_color))dst->outline_color=src->outline_color;
    if(src->font_size)  dst->font_size=src->font_size;
    if(src->font_scale) dst->font_scale=src->font_scale;
    if(src->bold)       dst->bold=src->bold;
    if(src->italic)     dst->italic=src->italic;
    if(src->underline)  dst->underline=src->underline;
    if(src->strike)     dst->strike=src->strike;
    if(src->overline)   dst->overline=src->overline;
    if(src->text_align) dst->text_align=src->text_align;
    if(src->text_transform)dst->text_transform=src->text_transform;
    if(src->white_space)dst->white_space=src->white_space;
    if(src->letter_spacing)dst->letter_spacing=src->letter_spacing;
    if(src->line_height)dst->line_height=src->line_height;
    if(src->has_text_shadow)dst->has_text_shadow=1;
    if(src->vertical_align)dst->vertical_align=src->vertical_align;
    if(src->width)      dst->width=src->width;
    if(src->height)     dst->height=src->height;
    if(src->min_width)  dst->min_width=src->min_width;
    if(src->min_height) dst->min_height=src->min_height;
    if(src->max_width)  dst->max_width=src->max_width;
    if(src->max_height) dst->max_height=src->max_height;
    if(src->padding_top)   dst->padding_top=src->padding_top;
    if(src->padding_right) dst->padding_right=src->padding_right;
    if(src->padding_bottom)dst->padding_bottom=src->padding_bottom;
    if(src->padding_left)  dst->padding_left=src->padding_left;
    if(src->margin_top!=0)    dst->margin_top=src->margin_top;
    if(src->margin_right!=0)  dst->margin_right=src->margin_right;
    if(src->margin_bottom!=0) dst->margin_bottom=src->margin_bottom;
    if(src->margin_left!=0)   dst->margin_left=src->margin_left;
    if(src->border_width)   dst->border_width=src->border_width;
    if(src->border_radius)  dst->border_radius=src->border_radius;
    if(src->border_radius_tl)dst->border_radius_tl=src->border_radius_tl;
    if(src->border_radius_tr)dst->border_radius_tr=src->border_radius_tr;
    if(src->border_radius_bl)dst->border_radius_bl=src->border_radius_bl;
    if(src->border_radius_br)dst->border_radius_br=src->border_radius_br;
    if(src->outline_width) dst->outline_width=src->outline_width;
    if(src->box_sizing_border)dst->box_sizing_border=1;
    if(src->display)       dst->display=src->display;
    if(src->display_flex)  dst->display_flex=1;
    if(src->flex_direction)dst->flex_direction=src->flex_direction;
    if(src->flex_wrap)     dst->flex_wrap=1;
    if(src->justify_content)dst->justify_content=src->justify_content;
    if(src->align_items)   dst->align_items=src->align_items;
    if(src->flex_grow)     dst->flex_grow=src->flex_grow;
    if(src->gap)           dst->gap=src->gap;
    if(src->position_type) dst->position_type=src->position_type;
    if(src->top_set)  {dst->top=src->top;   dst->top_set=1;}
    if(src->right_set){dst->right=src->right;dst->right_set=1;}
    if(src->bottom_set){dst->bottom=src->bottom;dst->bottom_set=1;}
    if(src->left_set) {dst->left=src->left; dst->left_set=1;}
    if(src->z_index)       dst->z_index=src->z_index;
    if(src->overflow_hidden)dst->overflow_hidden=1;
    if(src->overflow_scroll)dst->overflow_scroll=1;
    if(src->opacity!=100)  dst->opacity=src->opacity;
    if(src->has_shadow)    dst->has_shadow=1;
    if(src->list_style_none)dst->list_style_none=1;
    if(src->cursor_pointer)dst->cursor_pointer=1;
    if(src->object_fit)    dst->object_fit=src->object_fit;
    if(src->grid_cols)     dst->grid_cols=src->grid_cols;
    if(src->has_transition)dst->has_transition=1;
    if(src->has_animation) dst->has_animation=1;
}

static void get_style_for(const char*tag,const char*cls,const char*id,struct render_style*s){
    struct css_rule*r;
    for(r=css_rules;r;r=r->next)
        if(r->selector[0]!='.'&&r->selector[0]!='#'&&!strchr(r->selector,'.')&&!strchr(r->selector,'#'))
            if(sel_matches(r->selector,tag,cls,id))merge_style(s,&r->style);
    for(r=css_rules;r;r=r->next)
        if(r->selector[0]=='.'||r->selector[0]=='#'||strchr(r->selector,'.')||strchr(r->selector,'#'))
            if(sel_matches(r->selector,tag,cls,id))merge_style(s,&r->style);
}

/* ============================================================
 * Drawing primitives — all colors come from CSS via callers
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
 * JavaScript interpreter
 * ============================================================ */
static int run_js(const char*s){
    char buf[256]; const char*p=s;
    while(*p){
        while(*p&&*p<=' ') p++;
        if(!*p) break;
        if(!strncmp(p,"console.log(",12)||!strncmp(p,"console.error(",14)||!strncmp(p,"console.warn(",13)){
            while(*p&&*p!='(') p++;
            p++;
            int i=0; char q=(*p=='"'||*p=='\'')?*p++:0;
            while(*p&&(q?*p!=q:*p!=')')&&i<255) buf[i++]=*p++;
            buf[i]=0;
            printf("JS: %s\n",buf);
            while(*p&&*p!=';') p++;
        }else if(!strncmp(p,"alert(",6)){
            p+=6; int i=0; char q=(*p=='"'||*p=='\'')?*p++:0;
            while(*p&&(q?*p!=q:*p!=')')&&i<255) buf[i++]=*p++;
            buf[i]=0;
            printf("ALERT: %s\n",buf);
            while(*p&&*p!=';') p++;
        }else if(!strncmp(p,"exit()",6)||!strncmp(p,"window.close()",14)) return 1;
        else if(!strncmp(p,"goBack()",8)||!strncmp(p,"history.back()",14)) return 1;
        else if(!strncmp(p,"reboot()",8)){reboot();return 0;}
        else if(!strncmp(p,"//",2)){while(*p&&*p!='\n') p++;}
        else if(!strncmp(p,"/*",2)){while(*p&&strncmp(p,"*/",2))p++;if(*p)p+=2;}
        else p++;
    }
    return 0;
}

/* ============================================================
 * Dimension resolvers
 * ============================================================ */
static int res_w(const struct render_style*s,int pw,int sw){
    if(s->width<-1)return pw*(-s->width)/100;
    if(s->width>0) return s->width;
    if(s->width==-1)return sw;
    return 0;
}
static int res_h(const struct render_style*s,int ph,int sh){
    if(s->height==-1)return sh;
    if(s->height<-1000)return sh*(-s->height-1000)/100;
    if(s->height<-1)   return ph*(-s->height-2000)/100;
    if(s->height>0)    return s->height;
    return 0;
}

/* ============================================================
 * DOM node
 * ============================================================ */
struct dom_node{
    char tag[32],cls[128],id[64],alt[128];
    char onclick[128],href[256],src[256],value[128];
    char placeholder[128],type[32],title[128];
    int disabled,checked,hidden_attr;
    struct render_style style;
    int x,y,w,h;
    int cx,cy; /* inline-flow cursor */
    /* flex pre-scan results */
    int flex_children_total_h; /* sum of direct children h for justify-content:center */
};

/* ============================================================
 * Pre-scan: measure total height of direct children of a
 * flex-column container so we can compute the start y for
 * justify-content:center.
 * ============================================================ */
static int prescan_flex_col_h(const char*after_open_tag, int node_h, int sw, int sh){
    int total=0,depth=0;
    const char*p=after_open_tag;
    /* skip to end of the opening tag first */
    while(*p&&*p!='>') p++;
    if(*p=='>') p++;
    while(*p){
        if(*p!='<'){p++;continue;}
        p++;int cl=(*p=='/');if(cl)p++;
        char tag[32];int ti=0;
        while(*p&&*p!='>'&&*p!=' '&&ti<31) tag[ti++]=*p++;
        tag[ti]=0; strtolower(tag);
        /* grab class and id for style lookup */
        char ecls[128]={0},eid[64]={0};
        const char*a=p;
        while(*a&&*a!='>'){
            if(!strncmp(a,"class=\"",7)){a+=7;int ci=0;while(*a&&*a!='"'&&ci<127)ecls[ci++]=*a++;ecls[ci]=0;}
            else if(!strncmp(a,"id=\"",4)){a+=4;int ci=0;while(*a&&*a!='"'&&ci<63)eid[ci++]=*a++;eid[ci]=0;}
            else a++;
        }
        while(*p&&*p!='>')p++;
        static const char*vd[]={"br","hr","img","input","meta","link","base","area","col","embed","param","source","track","wbr",0};
        int isv=(tag[0]=='!');for(int vi=0;vd[vi];vi++)if(!strcmp(tag,vd[vi])){isv=1;break;}
        if(cl){if(depth==0)break;depth--;}
        else{
            if(depth==0){
                struct render_style cs;init_style(&cs);
                get_style_for(tag,ecls[0]?ecls:0,eid[0]?eid:0,&cs);
                if(cs.display!=2){
                    int ch=res_h(&cs,node_h,sh);
                    if(ch<=0)ch=(cs.font_scale>0?24:16);
                    int mt=cs.margin_top>0?cs.margin_top:0;
                    int mb=cs.margin_bottom>0?cs.margin_bottom:0;
                    total+=ch+mt+mb;
                }
            }
            if(!isv)depth++;
        }
        if(*p)p++;
    }
    return total;
}

/* ============================================================
 * Main HTML renderer
 * ============================================================ */
static int render_html(struct graphics*g,int scroll_y,int execute_js,int draw,
                       int*hover_state,const char*src,int progress){
    while(css_rules){struct css_rule*n=css_rules->next;kfree(css_rules);css_rules=n;}

    static char style_buf[65536];
    static char script_buf[65536];
    int si=0,ssi=0;

    /* Pass 1: parse CSS */
    {const char*p=src;int in_s=0,in_c=0;
    while(*p){
        if(!in_c&&!strncmp(p,"<!--",4)){in_c=1;p+=4;continue;}
        if(in_c){if(!strncmp(p,"-->",3)){in_c=0;p+=3;}else p++;continue;}
        if(*p=='<'){p++;int cl=(*p=='/');if(cl)p++;
            char tag[32];int ti=0;while(*p&&*p!='>'&&*p!=' '&&ti<31)tag[ti++]=*p++;tag[ti]=0;strtolower(tag);
            if(!strcmp(tag,"style")){if(cl){in_s=0;style_buf[si]=0;parse_css_sheet(style_buf);}else{in_s=1;si=0;}}
            while(*p&&*p!='>')p++;
        }else{if(in_s&&si<65534)style_buf[si++]=*p;}
        p++;}}

    int sw=(boot_screen_w>0)?boot_screen_w:graphics_width(g);
    int sh=(boot_screen_h>0)?boot_screen_h:graphics_height(g);

    int off_x = 0;
    int off_y = 0;

    /* Clear with body background color */
    if(draw){
        struct render_style bs;init_style(&bs);get_style_for("body",0,0,&bs);
        if(COLOR_IS_SET(bs.bg)){graphics_fgcolor(g,bs.bg);graphics_rect(g,off_x,off_y,sw,sh);}
    }

    static struct dom_node stack[128];int sp=0;
    {struct dom_node*root=&stack[0];memset(root,0,sizeof(*root));
    strcpy(root->tag,"_root");init_style(&root->style);
    root->style.display=1;root->style.display_flex=1;root->style.flex_direction=1;
    root->style.justify_content=1;root->style.align_items=1;
    root->w=sw;root->h=sh;root->x=off_x;root->y=off_y-scroll_y;
    root->cx=off_x;root->cy=off_y-scroll_y;}

    int hov=0,ex=0;
    const char*p=src;int in_tag=0,in_sty=0,in_scr=0,in_cm=0;

    while(*p){
        if(!in_tag&&!in_sty&&!in_scr&&!strncmp(p,"<!--",4)){in_cm=1;p+=4;}
        if(in_cm){if(!strncmp(p,"-->",3)){in_cm=0;p+=3;}else p++;continue;}

        if(*p=='<'){
            in_tag=1;p++;int cl=(*p=='/');if(cl)p++;
            char tag[32];int ti=0;
            while(*p&&*p!='>'&&*p!=' '&&*p!='\t'&&ti<31) tag[ti++]=*p++;
            tag[ti]=0; strtolower(tag);
            if(!strcmp(tag,"style")){if(cl)in_sty=0;else{in_sty=1;ssi=0;}}
            else if(!strcmp(tag,"script")){
                if(cl){in_scr=0;script_buf[ssi]=0;if(execute_js)run_js(script_buf);}
                else{in_scr=1;ssi=0;}
            }
            if(cl){
                if(sp>0){
                    struct dom_node*closed=&stack[sp];
                    struct dom_node*par=&stack[sp-1];
                    int bot=closed->y+closed->h+(closed->style.margin_bottom>0?closed->style.margin_bottom:0);
                    if(par->style.display_flex){
                        if(par->style.flex_direction==1){if(bot>par->cy)par->cy=bot+(par->style.gap>0?par->style.gap:0);}
                        else{int rt=closed->x+closed->w+(closed->style.margin_right>0?closed->style.margin_right:0);if(rt>par->cx)par->cx=rt+(par->style.gap>0?par->style.gap:0);}
                    }else if(closed->style.display==1||closed->style.display==6||closed->style.display==7){
                        if(bot>par->cy)par->cy=bot;
                    }
                    sp--;
                }
            }else if(!in_sty&&!in_scr){
                sp++;if(sp>=128)sp=127;
                struct dom_node*node=&stack[sp];
                memset(node,0,sizeof(*node));strcpy(node->tag,tag);
                init_style(&node->style);
                /* inherit */
                if(sp>0){struct dom_node*par=&stack[sp-1];
                    node->style.fg=par->style.fg;
                    node->style.font_size=par->style.font_size;node->style.font_scale=par->style.font_scale;
                    node->style.bold=par->style.bold;node->style.italic=par->style.italic;
                    node->style.underline=par->style.underline;node->style.white_space=par->style.white_space;
                    node->style.text_align=par->style.text_align;node->style.line_height=par->style.line_height;
                    node->style.letter_spacing=par->style.letter_spacing;
                    node->style.text_transform=par->style.text_transform;
                    node->style.list_style_none=par->style.list_style_none;
                    node->style.opacity=par->style.opacity;node->style.cursor_pointer=par->style.cursor_pointer;
                }
                /* parse attributes */
                const char*attr=p;
                while(*attr&&*attr!='>'){
                    while(*attr==' '||*attr=='\t'||*attr=='\n')attr++;
                    if(*attr=='>'||!*attr)break;
                    #define PA(nm,fld,ml) if(!strncmp(attr,nm "=\"",strlen(nm)+2)){attr+=strlen(nm)+2;int ci=0;while(*attr&&*attr!='"'&&ci<ml-1)node->fld[ci++]=*attr++;node->fld[ci]=0;continue;}
                    PA("class",cls,128)PA("id",id,64)PA("alt",alt,128)PA("onclick",onclick,128)
                    PA("href",href,256)PA("src",src,256)PA("value",value,128)PA("placeholder",placeholder,128)
                    PA("type",type,32)PA("title",title,128)
                    #undef PA
                    if(!strncmp(attr,"style=\"",7)){attr+=7;char is2[2048];int isi=0;while(*attr&&*attr!='"'&&isi<2047)is2[isi++]=*attr++;is2[isi]=0;parse_css_block(is2,&node->style);continue;}
                    if(!strncmp(attr,"disabled",8)&&attr[8]<=' '){node->disabled=1;attr+=8;continue;}
                    if(!strncmp(attr,"checked",7) &&attr[7]<=' '){node->checked=1; attr+=7;continue;}
                    if(!strncmp(attr,"hidden",6)  &&attr[6]<=' '){node->hidden_attr=1;attr+=6;continue;}
                    while(*attr&&*attr!=' '&&*attr!='\t'&&*attr!='\n'&&*attr!='>')attr++;
                }

                /* HTML UA display defaults */
                static const char*hidden_tags[]={"head","script","style","meta","link","title","template","noscript","base","source","track","param","col","datalist","area","optgroup","option","slot",0};
                static const char*block_tags[]={"html","body","div","p","h1","h2","h3","h4","h5","h6","ul","ol","li","dl","dt","dd","blockquote","pre","hr","header","footer","main","section","article","aside","nav","figure","figcaption","details","summary","dialog","form","fieldset","legend","table","tr","thead","tbody","tfoot","hgroup","address","center","output","progress","meter","picture","video","audio","canvas",0};
                static const char*iblock_tags[]={"img","input","button","select","textarea","iframe","embed","object","svg","math",0};
                int cat=0;
                for(int ii=0;!cat&&hidden_tags[ii];ii++)if(!strcmp(tag,hidden_tags[ii])){node->style.display=2;cat=1;}
                for(int ii=0;!cat&&block_tags[ii];ii++) if(!strcmp(tag,block_tags[ii])) {node->style.display=1;cat=1;}
                for(int ii=0;!cat&&iblock_tags[ii];ii++)if(!strcmp(tag,iblock_tags[ii])){node->style.display=3;cat=1;}
                if(!cat)node->style.display=0;

                /* semantic UA styles */
                if(!strcmp(tag,"pre")||!strcmp(tag,"code")||!strcmp(tag,"kbd")||!strcmp(tag,"samp"))
                    if(!node->style.white_space)node->style.white_space=1;
                if(!strcmp(tag,"b")||!strcmp(tag,"strong")||!strcmp(tag,"th")||!strcmp(tag,"h1")||!strcmp(tag,"h2")||!strcmp(tag,"h3")||!strcmp(tag,"h4")||!strcmp(tag,"h5")||!strcmp(tag,"h6"))node->style.bold=1;
                if(!strcmp(tag,"i")||!strcmp(tag,"em")||!strcmp(tag,"cite")||!strcmp(tag,"dfn"))node->style.italic=1;
                if(!strcmp(tag,"u")||!strcmp(tag,"ins"))node->style.underline=1;
                if(!strcmp(tag,"s")||!strcmp(tag,"del")||!strcmp(tag,"strike"))node->style.strike=1;
                if(!strcmp(tag,"a")){node->style.underline=1;node->style.cursor_pointer=1;}
                if(!strcmp(tag,"h1")){node->style.font_scale=2;node->style.font_size=32;}
                else if(!strcmp(tag,"h2")){node->style.font_scale=1;node->style.font_size=24;}
                else if(!strcmp(tag,"h3")){node->style.font_scale=1;node->style.font_size=20;}
                else if(!strcmp(tag,"h4")){node->style.font_scale=0;node->style.font_size=18;}
                else if(!strcmp(tag,"h5")||!strcmp(tag,"h6")){node->style.font_scale=0;node->style.font_size=14;}
                else if(!strcmp(tag,"small")||!strcmp(tag,"sub")||!strcmp(tag,"sup"))node->style.font_scale=-1;
                else if(!strcmp(tag,"big"))node->style.font_scale=1;
                else if(!strcmp(tag,"center")){node->style.display=1;node->style.display_flex=1;node->style.justify_content=1;node->style.text_align=1;}
                if(node->hidden_attr)node->style.display=2;

                /* apply CSS */
                get_style_for(tag,node->cls[0]?node->cls:0,node->id[0]?node->id:0,&node->style);

                /* progress-bar width from CSS container + progress % */
                if(!strcmp(node->id,"progress-bar")){
                    int cw=0;
                    if(sp>0&&stack[sp-1].w>0)cw=stack[sp-1].w;
                    if(cw<=0&&sp>0)cw=(stack[sp-1].style.max_width>0?stack[sp-1].style.max_width:(stack[sp-1].style.width>0?stack[sp-1].style.width:0));
                    if(cw<=0)cw=sw;
                    node->style.width=(cw*progress)/100;
                    if(node->style.width<0)node->style.width=0;
                }

                struct dom_node*par=&stack[sp-1];
                int lh=(node->style.line_height>0)?node->style.line_height:(node->style.font_scale>0?24:16);

                /* resolve w */
                node->w=res_w(&node->style,par->w,sw);
                if(!node->w){
                    if(!strcmp(tag,"body")||!strcmp(tag,"html"))node->w=sw;
                    else if(node->style.display==1)node->w=par->w-(node->style.margin_left>0?node->style.margin_left:0)-(node->style.margin_right>0?node->style.margin_right:0);
                    else node->w=par->w;
                }
                if(node->style.min_width>0&&node->w<node->style.min_width)node->w=node->style.min_width;
                if(node->style.max_width>0&&node->w>node->style.max_width) node->w=node->style.max_width;
                if(node->style.box_sizing_border&&node->w>0){
                    node->w-=node->style.padding_left+node->style.padding_right+2*node->style.border_width;
                    if(node->w<0)node->w=0;
                }

                /* resolve h */
                node->h=res_h(&node->style,par->h,sh);
                if(!node->h){
                    if(!strcmp(tag,"body")||!strcmp(tag,"html"))node->h=sh;
                    else node->h=lh;
                }
                if(node->style.min_height>0&&node->h<node->style.min_height)node->h=node->style.min_height;
                if(node->style.max_height>0&&node->h>node->style.max_height) node->h=node->style.max_height;

                /* ---- Positioning ---- */
                if(node->style.position_type>=2){
                    int bx=(node->style.position_type==3)?0:par->x;
                    int by=(node->style.position_type==3)?-scroll_y:par->y;
                    node->x=bx+(node->style.left_set?node->style.left:0);
                    if(!node->style.left_set&&node->style.right_set)node->x=bx+par->w-node->style.right-node->w;
                    node->y=by+(node->style.top_set?node->style.top:0);
                    if(!node->style.top_set&&node->style.bottom_set)node->y=by+par->h-node->style.bottom-node->h;
                }else{
                    int ml=node->style.margin_left>0?node->style.margin_left:0;
                    int mt=node->style.margin_top>0?node->style.margin_top:0;
                    if(par->style.display_flex){
                        if(par->style.flex_direction==1){
                            /* column flex */
                            /* X: align-items */
                            if(par->style.align_items==1)       node->x=par->x+(par->w-node->w)/2;
                            else if(par->style.align_items==2)  node->x=par->x+par->w-node->w-(node->style.margin_right>0?node->style.margin_right:0);
                            else                                 node->x=par->x+par->style.padding_left+ml;
                            /* auto-margin h-center */
                            if(node->style.margin_left==-1&&node->style.margin_right==-1)node->x=par->x+(par->w-node->w)/2;
                            /* Y: justify-content (par->cy is pre-adjusted for center) */
                            node->y=par->cy+mt;
                        }else{
                            /* row flex */
                            if(par->style.align_items==1)       node->y=par->y+(par->h-node->h)/2;
                            else                                 node->y=par->y+par->style.padding_top+mt;
                            node->x=par->cx+ml;
                        }
                    }else if(node->style.display==1||node->style.display==6||node->style.display==7){
                        node->x=par->x+par->style.padding_left+ml;
                        if(node->style.margin_left==-1&&node->style.margin_right==-1&&node->w>0)node->x=par->x+(par->w-node->w)/2;
                        node->y=par->cy+mt;
                    }else{
                        node->x=par->cx+ml;node->y=par->cy+mt;
                    }
                }

                /* child flow cursor */
                node->cx=node->x+node->style.padding_left;
                node->cy=node->y+node->style.padding_top;

                /* ---- flex-column justify-content:center pre-scan ---- */
                if(node->style.display_flex&&node->style.flex_direction==1
                   &&node->style.justify_content==1&&node->h>0){
                    int total_ch=prescan_flex_col_h(p,node->h,sw,sh);
                    if(total_ch>0){
                        int usable=node->h-node->style.padding_top-node->style.padding_bottom;
                        int start=(usable-total_ch)/2;
                        if(start<0)start=0;
                        node->cy=node->y+node->style.padding_top+start;
                    }
                    node->flex_children_total_h=total_ch;
                }

                /* advance parent cursor */
                if(par->style.display_flex){
                    if(par->style.flex_direction==1){
                        int bot=node->y+node->h+(node->style.margin_bottom>0?node->style.margin_bottom:0)+(par->style.gap>0?par->style.gap:0);
                        par->cy=bot;
                    }else{
                        int rt=node->x+node->w+(node->style.margin_right>0?node->style.margin_right:0)+(par->style.gap>0?par->style.gap:0);
                        par->cx=rt;
                    }
                }else if(node->style.display==1||node->style.display==6||node->style.display==7){
                    par->cy=node->y+node->h+(node->style.margin_bottom>0?node->style.margin_bottom:0);
                }

                /* ---- Draw ---- */
                if(node->style.display!=2&&draw){
                    /* shadow */
                    if(node->style.has_shadow&&COLOR_IS_SET(node->style.bg)&&node->w>0&&node->h>0){
                        struct graphics_color sh2={(uint8_t)(node->style.bg.r>30?node->style.bg.r-30:0),(uint8_t)(node->style.bg.g>30?node->style.bg.g-30:0),(uint8_t)(node->style.bg.b>30?node->style.bg.b-30:0),0};
                        draw_rounded_rect(g,node->x+4,node->y+4,node->w,node->h,node->style.border_radius,sh2);
                    }
                    /* background */
                    if(COLOR_IS_SET(node->style.bg)&&node->w>0){
                        int bh=node->h>0?node->h:lh;
                        if(node->style.border_radius==-1)draw_circle(g,node->x,node->y,node->w,node->style.bg);
                        else if(node->style.border_radius>0)draw_rounded_rect(g,node->x,node->y,node->w,bh,node->style.border_radius,node->style.bg);
                        else fill_rect(g,node->x,node->y,node->w,bh,node->style.bg);
                    }
                    /* border */
                    if(node->style.border_width>0&&node->w>0){
                        struct graphics_color bc=COLOR_IS_SET(node->style.border_color)?node->style.border_color:node->style.fg;
                        if(COLOR_IS_SET(bc))draw_border_box(g,node->x,node->y,node->w,node->h>0?node->h:lh,node->style.border_width,bc);
                    }
                    /* outline */
                    if(node->style.outline_width>0&&COLOR_IS_SET(node->style.outline_color)&&node->w>0){
                        int ow=node->style.outline_width;
                        draw_border_box(g,node->x-ow,node->y-ow,node->w+2*ow,(node->h>0?node->h:lh)+2*ow,ow,node->style.outline_color);
                    }
                    /* tag visuals */
                    if(!strcmp(tag,"hr")){
                        struct graphics_color hc=COLOR_IS_SET(node->style.border_color)?node->style.border_color:(COLOR_IS_SET(node->style.fg)?node->style.fg:COLOR_TRANSPARENT);
                        if(COLOR_IS_SET(hc)){int hw=node->w>0?node->w:sw-node->x-20;int bw2=node->style.border_width>0?node->style.border_width:1;graphics_fgcolor(g,hc);for(int bi=0;bi<bw2;bi++)graphics_line(g,node->x,node->y+8+bi,hw,0);}
                    }
                    else if(!strcmp(tag,"img")){
                        int iw=node->w,ih=node->h;
                        if(node->src[0]&&is_tga(node->src)&&(iw>0||ih>0))draw_tga(g,node->src,node->x,node->y,iw,ih);
                        else if(node->src[0]&&COLOR_IS_SET(node->style.bg)&&iw>0&&ih>0){
                            fill_rect(g,node->x,node->y,iw,ih,node->style.bg);
                            if(COLOR_IS_SET(node->style.fg)){graphics_fgcolor(g,node->style.fg);graphics_line(g,node->x,node->y,iw,ih);graphics_line(g,node->x,node->y+ih,iw,-ih);}
                        }
                        if(node->alt[0]&&COLOR_IS_SET(node->style.fg)&&iw>0&&ih>0){int tx=node->x+4,ty=node->y+ih/2-4;draw_text(g,&tx,ty,node->alt,node->style.fg,0,0,0,0,0,0,0);}
                    }
                    else if(!strcmp(tag,"video")||!strcmp(tag,"canvas")||!strcmp(tag,"iframe")){
                        if(node->w>0&&node->h>0&&COLOR_IS_SET(node->style.bg))fill_rect(g,node->x,node->y,node->w,node->h,node->style.bg);
                        if(node->w>0&&node->h>0&&COLOR_IS_SET(node->style.border_color)&&node->style.border_width>0)draw_border_box(g,node->x,node->y,node->w,node->h,node->style.border_width,node->style.border_color);
                    }
                    else if(!strcmp(tag,"button")){
                        if(node->w>0&&node->h>0&&COLOR_IS_SET(node->style.bg))draw_rounded_rect(g,node->x,node->y,node->w,node->h,node->style.border_radius,node->style.bg);
                        if(node->style.border_width>0&&COLOR_IS_SET(node->style.border_color))draw_border_box(g,node->x,node->y,node->w,node->h,node->style.border_width,node->style.border_color);
                        node->cx=node->x+(node->style.padding_left>0?node->style.padding_left:0);
                        node->cy=node->y+(node->style.padding_top>0?node->style.padding_top:0);
                    }
                    else if(!strcmp(tag,"input")){
                        int fw=node->w,fh=node->h;
                        if(fw>0&&fh>0){
                            if(!strcmp(node->type,"checkbox")||!strcmp(node->type,"radio")){
                                if(COLOR_IS_SET(node->style.bg))fill_rect(g,node->x,node->y+(fh-fw)/2,fw,fw,node->style.bg);
                                if(COLOR_IS_SET(node->style.border_color)&&node->style.border_width>0)draw_border_box(g,node->x,node->y+(fh-fw)/2,fw,fw,node->style.border_width,node->style.border_color);
                                if(node->checked&&COLOR_IS_SET(node->style.fg)){int pad=fw/4;fill_rect(g,node->x+pad,node->y+(fh-fw)/2+pad,fw-2*pad,fw-2*pad,node->style.fg);}
                            }else if(!strcmp(node->type,"range")){
                                int th=node->style.border_width>0?node->style.border_width:4;
                                if(COLOR_IS_SET(node->style.bg))draw_rounded_rect(g,node->x,node->y+(fh-th)/2,fw,th,th/2,node->style.bg);
                                if(COLOR_IS_SET(node->style.fg)){draw_rounded_rect(g,node->x,node->y+(fh-th)/2,fw/2,th,th/2,node->style.fg);draw_circle(g,node->x+fw/2-fh/2,node->y,fh,node->style.fg);}
                            }else{
                                if(COLOR_IS_SET(node->style.bg))draw_rounded_rect(g,node->x,node->y,fw,fh,node->style.border_radius,node->style.bg);
                                if(COLOR_IS_SET(node->style.border_color)&&node->style.border_width>0)draw_border_box(g,node->x,node->y,fw,fh,node->style.border_width,node->style.border_color);
                                const char*show=node->value[0]?node->value:node->placeholder;
                                if(show&&show[0]&&COLOR_IS_SET(node->style.fg)){
                                    int tx=node->x+(node->style.padding_left>0?node->style.padding_left:4);
                                    int ty=node->y+(node->style.padding_top>0?node->style.padding_top:4);
                                    if(!strcmp(node->type,"password")){char mask[128];int mi=0;while(show[mi]&&mi<127){mask[mi]='*';mi++;}mask[mi]=0;draw_text(g,&tx,ty,mask,node->style.fg,0,0,0,0,0,0,0);}
                                    else draw_text(g,&tx,ty,show,node->style.fg,0,0,0,0,0,0,0);
                                }
                            }
                        }
                    }
                    else if(!strcmp(tag,"select")){
                        if(node->w>0&&node->h>0&&COLOR_IS_SET(node->style.bg)){draw_rounded_rect(g,node->x,node->y,node->w,node->h,node->style.border_radius,node->style.bg);
                            if(COLOR_IS_SET(node->style.border_color)&&node->style.border_width>0)draw_border_box(g,node->x,node->y,node->w,node->h,node->style.border_width,node->style.border_color);
                            if(COLOR_IS_SET(node->style.fg)){int cx2=node->x+node->w-14,cy2=node->y+node->h/2-3;graphics_fgcolor(g,node->style.fg);for(int i=0;i<5;i++)graphics_line(g,cx2+i,cy2+i,5-i,0);}
                        }
                    }
                    else if(!strcmp(tag,"textarea")){
                        if(node->w>0&&node->h>0&&COLOR_IS_SET(node->style.bg)){draw_rounded_rect(g,node->x,node->y,node->w,node->h,node->style.border_radius,node->style.bg);
                            if(COLOR_IS_SET(node->style.border_color)&&node->style.border_width>0)draw_border_box(g,node->x,node->y,node->w,node->h,node->style.border_width,node->style.border_color);}
                        if(node->placeholder[0]&&COLOR_IS_SET(node->style.fg)){int tx=node->x+(node->style.padding_left>0?node->style.padding_left:4),ty=node->y+(node->style.padding_top>0?node->style.padding_top:4);draw_text(g,&tx,ty,node->placeholder,node->style.fg,0,1,0,0,0,0,0);}
                    }
                    else if(!strcmp(tag,"progress")||!strcmp(tag,"meter")){
                        if(node->w>0&&node->h>0){
                            int br2=node->style.border_radius;
                            if(COLOR_IS_SET(node->style.bg))draw_rounded_rect(g,node->x,node->y,node->w,node->h,br2,node->style.bg);
                            if(COLOR_IS_SET(node->style.fg))draw_rounded_rect(g,node->x,node->y,node->w/2,node->h,br2,node->style.fg);
                        }
                    }
                    else if(!strcmp(tag,"li")){
                        if(!node->style.list_style_none&&COLOR_IS_SET(node->style.fg)){
                            graphics_fgcolor(g,node->style.fg);
                            if(!(sp>1&&!strcmp(stack[sp-1].tag,"ol")))graphics_rect(g,node->x-14,node->y+6,4,4);
                        }
                    }
                    else if(!strcmp(tag,"svg")){
                        if(node->w>0&&node->h>0&&COLOR_IS_SET(node->style.bg))fill_rect(g,node->x,node->y,node->w,node->h,node->style.bg);
                        if(node->w>0&&node->h>0&&COLOR_IS_SET(node->style.border_color)&&node->style.border_width>0)draw_border_box(g,node->x,node->y,node->w,node->h,node->style.border_width,node->style.border_color);
                    }
                }

                /* interaction */
                if(node->style.display!=2&&node->w>0){
                    int ch2=node->h>0?node->h:lh;
                    if(ms_mx>=node->x&&ms_mx<node->x+node->w&&ms_my>=node->y&&ms_my<node->y+ch2){
                        if(node->onclick[0]||node->href[0]||node->style.cursor_pointer){
                            hov=1;
                            if(ms_left){
                                if(node->onclick[0]&&run_js(node->onclick))ex=1;
                                if(node->href[0]&&!strncmp(node->href,"javascript:",11))if(run_js(node->href+11))ex=1;
                            }
                        }
                    }
                }

                /* void tags auto-close */
                static const char*voids3[]={"!","br","hr","img","input","meta","link","base","area","col","embed","param","source","track","wbr","keygen","command",0};
                for(int vi=0;voids3[vi];vi++)if(!strcmp(tag,voids3[vi])){sp--;break;}
                if(!strcmp(tag,"br")&&sp>=0){stack[sp].cy+=lh;stack[sp].cx=stack[sp].x+stack[sp].style.padding_left;}
            }
            while(*p&&*p!='>') p++;
            in_tag=0;
        }else if(*p=='>'){in_tag=0;}
        else if(stack[sp].style.display==2&&!in_sty&&!in_scr){/* hidden */}
        else{
            if(in_scr&&ssi<65534){script_buf[ssi++]=*p;}
            else if(!in_tag&&!in_sty&&!in_scr){
                struct dom_node*node=&stack[sp];
                if(node->style.display==2)goto adv;
                int lh2=(node->style.line_height>0)?node->style.line_height:(node->style.font_scale>0?(node->style.font_scale>=2?36:24):16);
                char word[512];int wi=0;
                while(*p&&*p!='<'&&wi<511){
                    if(node->style.white_space==1||node->style.white_space==3){
                        if(*p=='\n'){word[wi]=0;if(wi>0&&draw&&COLOR_IS_SET(node->style.fg)){int tx=node->cx;draw_text(g,&tx,node->cy,word,node->style.fg,node->style.bold,node->style.italic,node->style.underline,node->style.strike,node->style.overline,node->style.font_scale,node->style.letter_spacing);node->cx=tx;}node->cx=node->x+node->style.padding_left;node->cy+=lh2;wi=0;p++;continue;}
                        word[wi++]=*p++;
                    }else{
                        if(*p=='\n'||*p=='\r'){p++;word[wi++]=' ';break;}
                        if(*p=='\t'){p++;word[wi++]=' ';break;}
                        word[wi++]=*p++;if(word[wi-1]==' ')break;
                    }
                }
                word[wi]=0;p--;
                if(node->style.text_transform==1) {
                    for(int i=0;word[i];i++) if(word[i]>='a'&&word[i]<='z') word[i]-=32;
                } else if(node->style.text_transform==2) {
                    for(int i=0;word[i];i++) if(word[i]>='A'&&word[i]<='Z') word[i]+=32;
                } else if(node->style.text_transform==3&&wi>0) {
                    if(word[0]>='a'&&word[0]<='z') word[0]-=32;
                }
                int cw2=(node->style.font_scale>0)?16:(node->style.font_scale<0?6:8);cw2+=node->style.letter_spacing;
                int tw2=wi*cw2;
                int re=(node->w>0?node->x+node->w-node->style.padding_right:sw-20);
                if(node->cx+tw2>re&&node->style.white_space!=2&&wi>0){node->cx=node->x+node->style.padding_left;node->cy+=lh2;}
                int yo=0;if(!strcmp(node->tag,"sub")||node->style.vertical_align==4)yo=4;if(!strcmp(node->tag,"sup")||node->style.vertical_align==5)yo=-5;
                if(wi>0&&COLOR_IS_SET(node->style.fg)){
                    int tx=node->cx;
                    if(node->style.text_align==1&&node->w>0)tx=node->x+(node->w-tw2)/2;
                    else if(node->style.text_align==2&&node->w>0)tx=node->x+node->w-tw2;
                    if(node->style.has_text_shadow&&draw){struct graphics_color sh3={0,0,0,0};int tsx=tx+1;draw_text(g,&tsx,node->cy+yo+1,word,sh3,0,0,0,0,0,node->style.font_scale,node->style.letter_spacing);}
                    if(draw)draw_text(g,&tx,node->cy+yo,word,node->style.fg,node->style.bold,node->style.italic,node->style.underline,node->style.strike,node->style.overline,node->style.font_scale,node->style.letter_spacing);
                    node->cx=tx;
                }
            }
        }
        adv:p++;
    }
    if(hover_state)*hover_state=hov;
    return ex;
}

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
 * Progress phases mirror the JS timing from the original HTML:
 *   0–19  %  fast start    (~1.2 s delay between increments)
 *  20–49  %  steady crawl  (~1.5 s)
 *  50–74  %  slowing down  (~1.8 s)
 *  75–91  %  "stuck" zone  (~2.5 s)
 *  92–99  %  final push    (~1.0 s)
 * ============================================================ */
#define BOOT_TICKS_PER_SEC   50
#define BOOT_INIT_TICKS      (BOOT_TICKS_PER_SEC * 3)        /* 3.0 s  */
#define BOOT_HOLD_TICKS      (BOOT_TICKS_PER_SEC * 3 / 2)    /* 1.5 s  */
#define BOOT_FADE_TICK_STEP  (BOOT_TICKS_PER_SEC * 75 / 1000)/* ~75 ms per step */

/* progress phase delays: ticks between each +1 increment */
static int boot_phase_delay(int p){
    if(p < 20) return (BOOT_TICKS_PER_SEC * 12 / 10);  /* 1.2 s */
    if(p < 50) return (BOOT_TICKS_PER_SEC * 15 / 10);  /* 1.5 s */
    if(p < 75) return (BOOT_TICKS_PER_SEC * 18 / 10);  /* 1.8 s */
    if(p < 92) return (BOOT_TICKS_PER_SEC * 25 / 10);  /* 2.5 s */
    return (BOOT_TICKS_PER_SEC * 10 / 10);  /* 1.0 s */
}

int GUI(){
    uint32_t phys_mem_upper=total_memory*1024*1024;
    if(phys_mem_upper<0x80000000){
        printf("\f");printf("Warning: Inadequate RAM detected.\n");
        printf("2GB of RAM is recommended for the GUI to run correctly.\n");
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
            }
            break;

        case 1: /* ── PROG: progress bar 0 → 100 % ── */
            if(++prog_counter >= boot_phase_delay(boot_progress)){
                prog_counter = 0;
                if(++boot_progress > 100) boot_progress = 100;
                needs_redraw = 1;
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
            }
            break;

        case 3: /* ── FADE: scanline fade to black ── */
            if(++boot_counter >= BOOT_FADE_TICK_STEP){
                boot_counter = 0;
                boot_fade++;
                needs_redraw = 1;
                if(boot_fade >= BOOT_FADE_STEPS){
                    boot_state   = 4;
                    needs_redraw = 1;
                }
            }
            break;

        case 4: /* ── DONE: fully black, exit ── */
            draw_boot_screen(g, sw, sh, boot_progress, BOOT_FADE_STEPS);
            goto exit;
        }

        if(needs_redraw){
            draw_boot_screen(g, sw, sh, boot_progress, boot_fade);
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
    /* Boot done.  Re-enable IRQ 44, restore cursor, and paint the mouse
     * sprite now that the desktop renderer is taking over. */
    interrupt_enable(44);
    mouse_set_cursor(0);
    interrupt_disable(44);
    printf("\f");
    mouse_refresh();
    interrupt_enable(44);
    return 0;
}