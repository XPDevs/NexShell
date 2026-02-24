/*
Copyright (C) 2016-2019 The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file LICENSE for details.

All modifiactions are copyrighted to XPDevs and James Turner
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

extern int ms_mx;
extern int ms_my;
extern int ms_left;
extern int ms_middle;
extern void mouse_refresh();
extern void mouse_set_cursor(int type);
extern void FORCE_MENU();

// Embedded HTML5 content
static const char *html_content = 
"\
<!DOCTYPE html> \
<html> \
<head> \
    <title>test Page</title> \
    <style> \
        body { \
            background-color: black; \
            color: white; \
            font-family: sans-serif; \
            display: flex; \
            justify-content: center; \
            align-items: center; \
            height: 100vh; \
            margin: 0; \
        } \
        button { \
            background-color: #444; \
            color: white; \
            padding: 10px; \
            border-radius: 5px; \
        } \
    </style> \
    <script> \
        function goBack() { \
            exit(); \
        } \
    </script> \
</head> \
<body> \
    <h1>Welcome to the test page</h1> \
    <br> \
    <button onclick=\"goBack()\">Go Back</button> \
</body> \
</html>";

static void draw_text_styled(struct graphics *g, int *x, int y, const char *text, struct graphics_color fg, struct graphics_color bg, int bold, int italic, int underline, int strike) {
    if (g) {
        graphics_fgcolor(g, fg);
        graphics_bgcolor(g, bg);
    }
    while (*text) {
        if (g) {
            graphics_char(g, *x, y, *text);
            if (bold) graphics_char(g, *x + 1, y, *text); // Poor man's bold
            if (underline) graphics_line(g, *x, y + 14, 8, 0);
            if (strike) graphics_line(g, *x, y + 7, 8, 0);
        }
        // Italic is hard without font support, skip for now or maybe shift y?
        *x += 8;
        text++;
    }
}

struct render_style {
    struct graphics_color fg;
    struct graphics_color bg;
    int font_scale;
    int width;
    int height;
    int padding;
    int margin;
    int border_radius;
    int position_absolute;
    int top;
    int right;
    int display_flex;
    int justify_center;
    int align_center;
    int has_shadow;
    int display; // 0=inline, 1=block, 2=none, 3=inline-block
    int white_space; // 0=normal, 1=pre
    int bold;
    int italic;
    int underline;
    int strike;
};

struct css_rule {
    char selector[64];
    struct render_style style;
    struct css_rule *next;
};

static struct css_rule *css_rules = 0;

static void init_style(struct render_style *s) {
    memset(s, 0, sizeof(struct render_style));
    s->fg = (struct graphics_color){0,0,0,0};
    s->bg = (struct graphics_color){0,0,0,255}; // Transparent by default (alpha 255 = transparent in this system's logic usually, or we treat it as such)
    s->display = 0; // Default inline
}

static int parse_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

static struct graphics_color parse_css_color(const char *str) {
    struct graphics_color c = {0, 0, 0, 255}; // Default to transparent if invalid
    if (str[0] == '#') {
        int len = strlen(str);
        if (len >= 7) {
            c.r = (parse_hex_digit(str[1]) << 4) | parse_hex_digit(str[2]);
            c.g = (parse_hex_digit(str[3]) << 4) | parse_hex_digit(str[4]);
            c.b = (parse_hex_digit(str[5]) << 4) | parse_hex_digit(str[6]);
            c.a = 0;
        } else if (len >= 4) { // Support for #333 format
            c.r = (parse_hex_digit(str[1]) << 4) | parse_hex_digit(str[1]);
            c.g = (parse_hex_digit(str[2]) << 4) | parse_hex_digit(str[2]);
            c.b = (parse_hex_digit(str[3]) << 4) | parse_hex_digit(str[3]);
            c.a = 0;
        }
    } else if (!strcmp(str, "red")) { c.r = 255; c.a=0; }
    else if (!strcmp(str, "green")) { c.g = 128; c.a=0; }
    else if (!strcmp(str, "blue")) { c.b = 255; c.a=0; }
    else if (!strcmp(str, "white")) { c.r=255; c.g=255; c.b=255; c.a=0; }
    else if (!strcmp(str, "black")) { c.a=0; }
    else if (!strcmp(str, "yellow")) { c.r=255; c.g=255; c.a=0; }
    else if (!strcmp(str, "cyan")) { c.g=255; c.b=255; c.a=0; }
    else if (!strcmp(str, "magenta")) { c.r=255; c.b=255; c.a=0; }
    else if (!strcmp(str, "gray")) { c.r=128; c.g=128; c.b=128; c.a=0; }
    else if (!strcmp(str, "orange")) { c.r=255; c.g=165; c.a=0; }
    else if (!strcmp(str, "purple")) { c.r=128; c.g=0; c.b=128; c.a=0; }
    else if (!strcmp(str, "teal")) { c.g=128; c.b=128; c.a=0; }
    else if (!strcmp(str, "navy")) { c.b=128; c.a=0; }
    else if (!strcmp(str, "maroon")) { c.r=128; c.a=0; }
    else if (!strcmp(str, "silver")) { c.r=192; c.g=192; c.b=192; c.a=0; }
    else if (!strcmp(str, "lime")) { c.g=255; c.a=0; }
    else if (!strcmp(str, "olive")) { c.r=128; c.g=128; c.a=0; }
    return c;
}

static char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) return (char *)haystack;
    }
    return 0;
}

static void parse_css_block(const char *block, struct render_style *s) {
    char buf[128];
    strncpy(buf, block, 127);
    buf[127] = 0;
    char *p = buf;
    
    // Very basic parser: split by ;
    while (*p) {
        char *end = strchr(p, ';');
        if (end) *end = 0;
        
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = 0;
            char *val = colon + 1;
            while (*val == ' ') val++;
            
            // Trim trailing whitespace
            int len = strlen(val);
            while(len > 0 && val[len-1] <= ' ') val[--len] = 0;

            if (!strcmp(p, "color")) {
                s->fg = parse_css_color(val);
            } else if (!strcmp(p, "background-color")) {
                s->bg = parse_css_color(val);
            } else if (!strcmp(p, "width")) {
                str2int(val, &s->width);
            } else if (!strcmp(p, "height")) {
                if (strstr(val, "vh")) s->height = -1; // -1 for 100vh hack
                else str2int(val, &s->height);
            } else if (!strcmp(p, "padding")) {
                str2int(val, &s->padding);
            } else if (!strcmp(p, "border-radius")) {
                if (strstr(val, "%")) s->border_radius = -1; // -1 for 50%
                else str2int(val, &s->border_radius);
            } else if (!strcmp(p, "position")) {
                if (strstr(val, "absolute")) s->position_absolute = 1;
            } else if (!strcmp(p, "top")) {
                str2int(val, &s->top);
            } else if (!strcmp(p, "right")) {
                str2int(val, &s->right);
            } else if (!strcmp(p, "display")) {
                if (strstr(val, "flex")) s->display_flex = 1;
            } else if (!strcmp(p, "justify-content")) {
                if (strstr(val, "center")) s->justify_center = 1;
            } else if (!strcmp(p, "align-items")) {
                if (strstr(val, "center")) s->align_center = 1;
            } else if (!strcmp(p, "box-shadow")) {
                s->has_shadow = 1;
            }
            else if (!strcmp(p, "display")) {
                if (strstr(val, "none")) s->display = 2;
                else if (strstr(val, "block")) s->display = 1;
            }
        }
        
        if (!end) break;
        p = end + 1;
        while (*p == ' ') p++;
    }
}

static void parse_css_sheet(const char *sheet) {
    const char *p = sheet;
    while (*p) {
        // Skip whitespace
        while (*p && *p <= ' ') p++;
        if (!*p) break;
        
        // Read selector
        const char *sel_start = p;
        while (*p && *p != '{') p++;
        if (!*p) break;
        
        int sel_len = p - sel_start;
        char selector[64];
        if (sel_len > 63) sel_len = 63;
        strncpy(selector, sel_start, sel_len);
        selector[sel_len] = 0;
        
        // Trim selector
        while (sel_len > 0 && selector[sel_len-1] <= ' ') selector[--sel_len] = 0;
        
        p++; // Skip '{'
        
        // Read block
        const char *block_start = p;
        while (*p && *p != '}') p++;
        int block_len = p - block_start;
        char block[512];
        if (block_len > 511) block_len = 511;
        strncpy(block, block_start, block_len);
        block[block_len] = 0;
        
        if (*p == '}') p++;
        
        // Store rule
        struct css_rule *rule = kmalloc(sizeof(struct css_rule));
        strcpy(rule->selector, selector);
        init_style(&rule->style);
        parse_css_block(block, &rule->style);
        rule->next = css_rules;
        css_rules = rule;
    }
}

static void get_style_for(const char *tag, const char *cls, struct render_style *s) {
    struct css_rule *r = css_rules;
    while (r) {
        int match = 0;
        if (cls && r->selector[0] == '.' && !strcmp(r->selector + 1, cls)) match = 1;
        if (tag && !strcmp(r->selector, tag)) match = 1;
        
        if (match) {
            // Merge styles (simple overwrite)
            if (r->style.bg.a != 255) s->bg = r->style.bg; // If not transparent, set it
            if (r->style.width) s->width = r->style.width;
            if (r->style.height) s->height = r->style.height;
            if (r->style.padding) s->padding = r->style.padding;
            if (r->style.border_radius) s->border_radius = r->style.border_radius;
            if (r->style.position_absolute) s->position_absolute = 1;
            if (r->style.top) s->top = r->style.top;
            if (r->style.right) s->right = r->style.right;
            if (r->style.display_flex) s->display_flex = 1;
            if (r->style.justify_center) s->justify_center = 1;
            if (r->style.align_center) s->align_center = 1;
            if (r->style.has_shadow) s->has_shadow = 1;
        }
        r = r->next;
    }
}

static void draw_rounded_rect(struct graphics *g, int x, int y, int w, int h, int r, struct graphics_color c) {
    graphics_fgcolor(g, c);
    // Simple fill for now, maybe improve later
    // Center rect
    graphics_rect(g, x + r, y, w - 2*r, h);
    graphics_rect(g, x, y + r, r, h - 2*r);
    graphics_rect(g, x + w - r, y + r, r, h - 2*r);
    
    // Corners (approximated)
    // Top-left
    graphics_rect(g, x+2, y+2, r-2, r-2); 
    // Top-right
    graphics_rect(g, x+w-r, y+2, r-2, r-2);
    // Bottom-left
    graphics_rect(g, x+2, y+h-r, r-2, r-2);
    // Bottom-right
    graphics_rect(g, x+w-r, y+h-r, r-2, r-2);
}

static void draw_circle(struct graphics *g, int x, int y, int d, struct graphics_color c) {
    graphics_fgcolor(g, c);
    // Approximate circle
    graphics_rect(g, x + d/4, y, d/2, d);
    graphics_rect(g, x, y + d/4, d, d/2);
}

static int run_js_script(const char *script) {
    char buf[128];
    const char *p = script;
    while (*p) {
        while (*p && *p <= ' ') p++;
        if (!*p) break;

        if (!strncmp(p, "console.log(\"", 13)) {
            p += 13;
            int i = 0;
            while (*p && *p != '"' && i < 127) buf[i++] = *p++;
            buf[i] = 0;
            printf("JS: %s\n", buf);
            while (*p && *p != ';') p++;
        } else if (!strncmp(p, "alert(\"", 7)) {
            p += 7;
            int i = 0;
            while (*p && *p != '"' && i < 127) buf[i++] = *p++;
            buf[i] = 0;
            printf("JS ALERT: %s\n", buf);
            while (*p && *p != ';') p++;
        } else if (!strncmp(p, "exit()", 6)) {
            return 1;
        } else if (!strncmp(p, "goBack()", 8)) {
            return 1;
        } else if (!strncmp(p, "onButton()", 10)) {
            return 1; // Map to exit for this demo
        } else {
            p++;
        }
    }
    return 0;
}

static int render_html(struct graphics *g, int scroll_y, int execute_js, int draw, int *hover_state) {
    // Reset CSS rules on re-render? No, parse once ideally. But for simplicity we parse every time or check if parsed.
    // For this demo, let's just clear and re-parse to allow "drop in" dynamic updates if we wanted.
    // But memory leak... let's assume static for now or free.
    // Free old rules
    while(css_rules) { struct css_rule *n = css_rules->next; kfree(css_rules); css_rules = n; }
    
    if (draw) {
        struct graphics_color white = {255, 255, 255, 0};
        graphics_bgcolor(g, white);
        graphics_clear(g, 0, 0, graphics_width(g), graphics_height(g));
    }

    const char *p = html_content;
    int in_tag = 0;
    int in_style = 0;
    int in_script = 0;
    char style_buf[2048];
    int style_idx = 0;
    char script_buf[2048];
    int script_idx = 0;
    
    int hover_detected = 0;
    int should_exit = 0;

    // Layout state
    int screen_w = graphics_width(g);
    int screen_h = graphics_height(g);
    
    // Simple DOM stack
    struct dom_node {
        char tag[32];
        char cls[32];
        char onclick[64];
        struct render_style style;
        int x, y, w, h;
    } stack[16];
    int sp = 0;
    
    // Root (Window)
    strcpy(stack[0].tag, "window");
    init_style(&stack[0].style);
    stack[0].w = screen_w;
    stack[0].h = screen_h;
    stack[0].x = 0;
    stack[0].y = -scroll_y;
    stack[0].style.display = 1; // Root is block
    stack[0].style.white_space = 0;
    stack[0].onclick[0] = 0;

    while (*p) {
        if (*p == '<') {
            in_tag = 1;
            p++;
            char tag[64];
            int ti = 0;
            int closing = 0;
            if (*p == '/') { closing = 1; p++; }
            
            while (*p && *p != '>' && *p != ' ' && ti < 63) tag[ti++] = *p++;
            tag[ti] = 0;
            strtolower(tag);
            
            if (!strcmp(tag, "style")) {
                if (closing) {
                    in_style = 0;
                    style_buf[style_idx] = 0;
                    parse_css_sheet(style_buf);
                } else {
                    in_style = 1;
                    style_idx = 0;
                }
            } else if (!strcmp(tag, "script")) {
                if (closing) {
                    in_script = 0;
                    script_buf[script_idx] = 0;
                    if (execute_js) run_js_script(script_buf);
                } else {
                    in_script = 1;
                    script_idx = 0;
                }
            }
            
            if (closing) {
                if (sp > 0) sp--;
                // Block closing newline
                if (stack[sp+1].style.display == 1) {
                    if (stack[sp].x > 20) { stack[sp].x = 20; stack[sp].y += 16; }
                }
            } else if (!in_style && !in_script) {
                sp++;
                struct dom_node *node = &stack[sp];
                strcpy(node->tag, tag);
                node->cls[0] = 0;
                node->onclick[0] = 0;
                init_style(&node->style);

                // Inherit styles from parent
                if (sp > 0) {
                    struct dom_node *parent = &stack[sp-1];
                    node->style.fg = parent->style.fg;
                    node->style.font_scale = parent->style.font_scale;
                    node->style.bold = parent->style.bold;
                    node->style.italic = parent->style.italic;
                    node->style.underline = parent->style.underline;
                    node->style.strike = parent->style.strike;
                    node->style.white_space = parent->style.white_space;
                }
                
                // Parse attributes (simple class only)
                const char *attr = p;
                while (*attr && *attr != '>') {
                    if (!strncmp(attr, "class=\"", 7)) {
                        attr += 7;
                        int ci = 0;
                        while (*attr && *attr != '"' && ci < 31) node->cls[ci++] = *attr++;
                        node->cls[ci] = 0;
                    } else if (!strncmp(attr, "onclick=\"", 9)) {
                        attr += 9;
                        int ci = 0;
                        while (*attr && *attr != '"' && ci < 63) node->onclick[ci++] = *attr++;
                        node->onclick[ci] = 0;
                    }
                    attr++;
                }
                
                // Tag Defaults
                if (!strcmp(tag, "head") || !strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "meta") || !strcmp(tag, "link") || !strcmp(tag, "title") || !strcmp(tag, "template")) {
                    node->style.display = 2; // None
                } else if (!strcmp(tag, "div") || !strcmp(tag, "p") || !strcmp(tag, "h1") || !strcmp(tag, "h2") || !strcmp(tag, "h3") || !strcmp(tag, "h4") || !strcmp(tag, "h5") || !strcmp(tag, "h6") || !strcmp(tag, "ul") || !strcmp(tag, "ol") || !strcmp(tag, "li") || !strcmp(tag, "dl") || !strcmp(tag, "dt") || !strcmp(tag, "dd") || !strcmp(tag, "blockquote") || !strcmp(tag, "pre") || !strcmp(tag, "hr") || !strcmp(tag, "header") || !strcmp(tag, "footer") || !strcmp(tag, "main") || !strcmp(tag, "section") || !strcmp(tag, "article") || !strcmp(tag, "aside") || !strcmp(tag, "nav") || !strcmp(tag, "figure") || !strcmp(tag, "figcaption") || !strcmp(tag, "details") || !strcmp(tag, "summary") || !strcmp(tag, "dialog") || !strcmp(tag, "form") || !strcmp(tag, "fieldset") || !strcmp(tag, "legend") || !strcmp(tag, "table") || !strcmp(tag, "tr") || !strcmp(tag, "thead") || !strcmp(tag, "tbody") || !strcmp(tag, "tfoot")) {
                    node->style.display = 1; // Block
                } else if (!strcmp(tag, "img") || !strcmp(tag, "video") || !strcmp(tag, "audio") || !strcmp(tag, "canvas") || !strcmp(tag, "iframe") || !strcmp(tag, "embed") || !strcmp(tag, "object") || !strcmp(tag, "input") || !strcmp(tag, "button") || !strcmp(tag, "select") || !strcmp(tag, "textarea") || !strcmp(tag, "progress") || !strcmp(tag, "meter")) {
                    node->style.display = 3; // Inline-Block
                }
                
                if (!strcmp(tag, "pre")) node->style.white_space = 1;
                if (!strcmp(tag, "b") || !strcmp(tag, "strong") || !strcmp(tag, "h1") || !strcmp(tag, "h2") || !strcmp(tag, "h3") || !strcmp(tag, "th")) node->style.bold = 1;
                if (!strcmp(tag, "i") || !strcmp(tag, "em") || !strcmp(tag, "cite") || !strcmp(tag, "dfn") || !strcmp(tag, "var") || !strcmp(tag, "address")) node->style.italic = 1;
                if (!strcmp(tag, "u") || !strcmp(tag, "ins")) node->style.underline = 1;
                if (!strcmp(tag, "s") || !strcmp(tag, "del") || !strcmp(tag, "strike")) node->style.strike = 1;
                
                // Apply styles
                get_style_for(tag, node->cls[0] ? node->cls : 0, &node->style);
                
                // Layout Logic
                struct dom_node *parent = &stack[sp-1];
                
                // Dimensions
                if (node->style.height == -1) node->h = screen_h; // 100vh
                else if (node->style.height > 0) node->h = node->style.height;
                else node->h = parent->h; // Default stretch?
                
                if (node->style.width > 0) node->w = node->style.width;
                else node->w = parent->w; // Block default
                
                // Block start newline
                if (node->style.display == 1) {
                    if (parent->x > 20) { parent->x = 20; parent->y += 16; }
                }

                // Positioning
                if (node->style.position_absolute) {
                    node->x = parent->x + parent->w - node->style.right - node->w;
                    node->y = parent->y + node->style.top;
                } else {
                    // Flex centering from parent
                    if (parent->style.display_flex && parent->style.justify_center) {
                        node->x = parent->x + (parent->w - node->w) / 2;
                    } else {
                        node->x = parent->x + node->style.margin; // Simple flow
                    }
                    
                    if (parent->style.display_flex && parent->style.align_center) {
                        node->y = parent->y + (parent->h - node->h) / 2;
                    } else {
                        node->y = parent->y + node->style.margin;
                    }
                }
                
                if (node->style.display == 2) { /* Hidden */ }
                else {
                // Render Background
                if (draw) {
                if (node->style.has_shadow) {
                    // Simulate shadow
                    graphics_fgcolor(g, (struct graphics_color){200,200,200,0});
                    graphics_rect(g, node->x + 10, node->y + 10, node->w, node->h);
                }
                
                if (node->style.bg.a != 255) { // If background is not transparent
                    if (node->style.border_radius == -1) { // 50%
                        draw_circle(g, node->x, node->y, node->w, node->style.bg);
                    } else if (node->style.border_radius > 0) {
                        draw_rounded_rect(g, node->x, node->y, node->w, node->h, node->style.border_radius, node->style.bg);
                    } else {
                        graphics_fgcolor(g, node->style.bg);
                        graphics_rect(g, node->x, node->y, node->w, node->h);
                    }
                }
                }
                
                // Render specific tag visuals
                if (!strcmp(tag, "hr")) {
                    if (draw) {
                        graphics_fgcolor(g, (struct graphics_color){128,128,128,0});
                        graphics_line(g, 20, node->y+8, screen_w-40, 0);
                    }
                    node->y += 16;
                }
                if (!strcmp(tag, "button")) {
                    if (draw) {
                        graphics_fgcolor(g, (struct graphics_color){200,200,200,0});
                        if (node->style.bg.a != 255) graphics_fgcolor(g, node->style.bg);
                        graphics_rect(g, node->x, node->y, 60, 20);
                    }
                    node->x += 5; node->y += 4; // Padding
                }
                if (!strcmp(tag, "input") || !strcmp(tag, "select") || !strcmp(tag, "textarea")) {
                    if (draw) {
                        graphics_fgcolor(g, (struct graphics_color){255,255,255,0});
                        graphics_rect(g, node->x, node->y, 100, 20);
                        graphics_fgcolor(g, (struct graphics_color){100,100,100,0});
                        graphics_line(g, node->x, node->y, 100, 0); graphics_line(g, node->x, node->y+20, 100, 0);
                        graphics_line(g, node->x, node->y, 0, 20); graphics_line(g, node->x+100, node->y, 0, 20);
                    }
                    node->x += 110;
                }
                if (!strcmp(tag, "progress") || !strcmp(tag, "meter")) {
                    if (draw) {
                        graphics_fgcolor(g, (struct graphics_color){200,200,200,0});
                        graphics_rect(g, node->x, node->y, 100, 16);
                        graphics_fgcolor(g, (struct graphics_color){0,200,0,0});
                        graphics_rect(g, node->x, node->y, 50, 16); // 50% dummy
                    }
                    node->x += 110;
                }
                if (!strcmp(tag, "img") || !strcmp(tag, "video") || !strcmp(tag, "canvas") || !strcmp(tag, "iframe")) {
                    int w = node->style.width > 0 ? node->style.width : 50;
                    int h = node->style.height > 0 ? node->style.height : 50;
                    if (draw) {
                        graphics_fgcolor(g, (struct graphics_color){200,200,200,0});
                        if (node->style.bg.a != 255) graphics_fgcolor(g, node->style.bg);
                        graphics_rect(g, node->x, node->y, w, h);
                        graphics_fgcolor(g, (struct graphics_color){0,0,0,0});
                        graphics_line(g, node->x, node->y, w, h); graphics_line(g, node->x, node->y+h, w, -h); // X
                    }
                    node->x += w + 10;
                }
                if (!strcmp(tag, "li")) {
                    node->x += 20;
                    if (draw) {
                        graphics_fgcolor(g, (struct graphics_color){0,0,0,0});
                        graphics_rect(g, node->x - 12, node->y + 6, 4, 4); // Bullet
                    }
                }
                if (!strcmp(tag, "blockquote")) {
                    node->x += 20;
                    if (draw) {
                        graphics_fgcolor(g, (struct graphics_color){150,150,150,0});
                        graphics_line(g, node->x - 10, node->y, 0, 40);
                    }
                }

                // Check for pseudo-element ::after
                char pseudo[64];
                strcpy(pseudo, node->cls);
                strcat(pseudo, "::after");
                struct render_style pstyle;
                init_style(&pstyle);
                get_style_for(0, pseudo, &pstyle);
                
                if (pstyle.width > 0) { // It exists
                    int px = node->x + node->w - pstyle.right - pstyle.width;
                    int py = node->y + pstyle.top;
                    if (draw && pstyle.border_radius == -1) {
                        draw_circle(g, px, py, pstyle.width, pstyle.bg);
                    }
                }
                
                // Interaction Check
                if (ms_mx >= node->x && ms_mx < node->x + node->w &&
                    ms_my >= node->y && ms_my < node->y + node->h) {
                    if (node->onclick[0]) {
                        hover_detected = 1;
                        if (ms_left) {
                            if (run_js_script(node->onclick)) should_exit = 1;
                        }
                    }
                }
                
                } // End !hidden
            }
            
            while (*p && *p != '>') p++;
            in_tag = 0;
        } else if (*p == '>') {
            in_tag = 0;
        } else if (stack[sp].style.display == 2 && !in_style && !in_script) {
            // Hidden content, skip
        } else {
            if (!in_tag && !in_style && !in_script) {
                // Render text
                struct dom_node *node = &stack[sp];
                char word[256];
                int wi = 0;
                
                // Simple word collection
                while (*p && *p != '<' && wi < 255) {
                    if (*p == '\n' && !node->style.white_space) { p++; if(node->x > 20) { node->x += 4; } continue; } // Collapse space
                    if (*p == '\n' && node->style.white_space) { node->x = 20; node->y += 16; p++; continue; }
                    word[wi++] = *p++;
                    if (wi > 0 && (word[wi-1] == ' ' || word[wi-1] == '\t')) break;
                }
                word[wi] = 0;
                p--; // Backtrack one for loop
                
                if (node->x + wi * 8 > screen_w - 20) { node->x = 20; node->y += 16; }
                draw_text_styled(draw ? g : 0, &node->x, node->y, word, node->style.fg, node->style.bg, node->style.bold, node->style.italic, node->style.underline, node->style.strike);
            }
            
            if (in_style && style_idx < 2047) {
                style_buf[style_idx++] = *p;
            }
            if (in_script && script_idx < 2047) {
                script_buf[script_idx++] = *p;
            }
        }
        p++;
    }
    
    if (hover_state) *hover_state = hover_detected;
    
    return should_exit;
}

int GUI() {
    interrupt_disable(44);
    printf("\f"); // Clear console

    struct graphics *g = &graphics_root;
    
    int scroll_y = 0;
    int prev_my = 0;
    int dragging = 0;
    int needs_redraw = 1;
    int hover = 0;
    
    if (render_html(g, scroll_y, 1, 1, &hover)) goto exit;

    mouse_refresh();
    if (hover) mouse_set_cursor(1); else mouse_set_cursor(0);
    interrupt_enable(44);

    // Wait for exit button click
    while(1) {
        needs_redraw = 0;
        if (ms_middle) {
            if (!dragging) { dragging = 1; prev_my = ms_my; }
            int dy = ms_my - prev_my;
            if (dy != 0) {
                scroll_y -= dy; // Drag up moves content down
                needs_redraw = 1;
                prev_my = ms_my;
            }
        } else { dragging = 0; }
        
        interrupt_disable(44);
        // Active render loop for interaction
        if (render_html(g, scroll_y, 0, needs_redraw, &hover)) goto exit;
        
        if (needs_redraw) {
            mouse_refresh();
        }
        if (hover) mouse_set_cursor(1); else mouse_set_cursor(0);
        
        interrupt_enable(44);

        char c;
        while(console_read_nonblock(&console_root, &c, 1)) {
            if(c == 6) { // Ctrl+F
                goto exit;
            }
        }

        interrupt_wait();
    }
    
exit:
    mouse_set_cursor(0); // Reset to arrow

    interrupt_disable(44);
    printf("\f");
    mouse_refresh();
    interrupt_enable(44);

    return 0;
}
