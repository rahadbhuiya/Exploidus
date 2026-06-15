/*
 * term_buf.c — framebuffer terminal emulator
 *
 * Renders a scrollable interactive terminal inside a GUI window.
 * Only depends on syscall.h + fb syscalls (fb_str, fb_rect etc.)
 */

#include "term_buf.h"
#include "../libc/syscall.h"

/*  colors  */
#define TC_BG      0x0A0A14
#define TC_FG      0xE6EDF3
#define TC_PROMPT  0xA78BFA   /* purple  */
#define TC_GREEN   0x10B981
#define TC_BLUE    0x58A6FF
#define TC_YELLOW  0xF0B429
#define TC_RED     0xEF4444
#define TC_GRAY    0x484F58
#define TC_CYAN    0x67E8F9
#define TC_DIM     0x6E7681

#define FONT_W  8
#define FONT_H  16
#define LINE_H  17   /* line height with 1px gap */

/*  helpers  */
static int tstrlen(const char *s) { int n=0; while(s[n]) n++; return n; }
static void tstrcpy(char *d, const char *s, int max)
{ int i=0; while(i<max-1&&s[i]){d[i]=s[i];i++;} d[i]='\0'; }

/*  init  */
void term_init(term_buf_t *t, int wx, int wy, int ww, int wh, int content_y)
{
    for (int i=0; i<TERM_ROWS; i++) { t->lines[i].text[0]='\0'; t->lines[i].color=TC_FG; }
    t->count      = 0;
    t->scroll_off = 0;
    t->input[0]   = '\0';
    t->input_len  = 0;
    t->input_pos  = 0;
    t->blink_tick = 0;
    t->blink_on   = 1;
    t->wx=wx; t->wy=wy; t->ww=ww; t->wh=wh;
    t->content_y  = content_y;

    /* history */
    t->hist_count   = 0;
    t->hist_head    = 0;
    t->hist_idx     = -1;
    t->hist_saved[0] = '\0';
    t->esc_state    = 0;

    /* welcome lines */
    term_println(t, "Exploidus v0.1.0  --  exploish", TC_GREEN);
    term_println(t, "Copyright (c) Exploidus Project. MIT License.", TC_DIM);
    term_println(t, "", TC_FG);
    term_println(t, "Type 'help' for commands.", TC_GRAY);
    term_println(t, "", TC_FG);
}

/*  append helpers  */
static term_line_t *cur_line(term_buf_t *t)
{
    int slot = t->count % TERM_ROWS;
    return &t->lines[slot];
}

static void new_line(term_buf_t *t)
{
    t->count++;
    term_line_t *l = cur_line(t);
    l->text[0] = '\0';
    l->color   = TC_FG;
}

void term_print(term_buf_t *t, const char *s, uint32_t color)
{
    term_line_t *l = cur_line(t);
    l->color = color;
    int li = tstrlen(l->text);
    while (*s && li < TERM_COLS) {
        if (*s == '\n') { new_line(t); l = cur_line(t); l->color = color; li = 0; s++; continue; }
        l->text[li++] = *s++;
        l->text[li]   = '\0';
    }
}

void term_println(term_buf_t *t, const char *s, uint32_t color)
{
    term_print(t, s, color);
    new_line(t);
}

void term_clear(term_buf_t *t)
{
    for (int i=0; i<TERM_ROWS; i++) { t->lines[i].text[0]='\0'; t->lines[i].color=TC_FG; }
    t->count = 0;
    t->scroll_off = 0;
}

const char *term_get_line(term_buf_t *t) { return t->input; }

/*  history helpers  */
static void _hist_push(term_buf_t *t, const char *s)
{
    if (!s || !*s) return;
    /* skip duplicate of last entry */
    if (t->hist_count > 0) {
        int last = (t->hist_head - 1 + TERM_HIST_SIZE) % TERM_HIST_SIZE;
        const char *a = t->hist_buf[last]; const char *b = s;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) return;   /* identical — don't store */
    }
    int i = 0;
    while (s[i] && i < TERM_HIST_LEN - 1) { t->hist_buf[t->hist_head][i] = s[i]; i++; }
    t->hist_buf[t->hist_head][i] = '\0';
    t->hist_head  = (t->hist_head + 1) % TERM_HIST_SIZE;
    if (t->hist_count < TERM_HIST_SIZE) t->hist_count++;
}

static const char *_hist_get(term_buf_t *t, int idx)
{
    if (idx < 0 || idx >= t->hist_count) return 0;
    int slot = (t->hist_head - 1 - idx + TERM_HIST_SIZE * 2) % TERM_HIST_SIZE;
    return t->hist_buf[slot];
}

/* load a history entry into the input buffer */
static void _hist_load(term_buf_t *t, const char *s)
{
    int i = 0;
    while (s[i] && i < TERM_INPUT - 1) { t->input[i] = s[i]; i++; }
    t->input[i]  = '\0';
    t->input_len = i;
    t->input_pos = i;
}

/*  keyboard handler 
   Returns 1 when Enter pressed (line ready).
   Handles: printable, backspace, left/right, home/end, delete,
            up/down history, ctrl-A/E/K/C/L, ESC sequences.
*/
int term_key(term_buf_t *t, char c)
{
    /*  ESC sequence state machine  */
    if (t->esc_state == 1) {
        if (c == '[') { t->esc_state = 2; return 0; }
        t->esc_state = 0;  /* unknown ESC sequence, abort */
        return 0;
    }
    if (t->esc_state == 2) {
        t->esc_state = 0;
        /* Arrow UP — older history */
        if (c == 'A') {
            if (t->hist_idx == -1) {
                /* save current live input */
                tstrcpy(t->hist_saved, t->input, TERM_HIST_LEN);
            }
            int next = t->hist_idx + 1;
            const char *h = _hist_get(t, next);
            if (h) { t->hist_idx = next; _hist_load(t, h); }
            return 0;
        }
        /* Arrow DOWN — newer / back to live */
        if (c == 'B') {
            if (t->hist_idx > 0) {
                t->hist_idx--;
                const char *h = _hist_get(t, t->hist_idx);
                if (h) _hist_load(t, h);
            } else if (t->hist_idx == 0) {
                t->hist_idx = -1;
                _hist_load(t, t->hist_saved);
            }
            return 0;
        }
        /* Arrow LEFT */
        if (c == 'D') {
            if (t->input_pos > 0) t->input_pos--;
            return 0;
        }
        /* Arrow RIGHT */
        if (c == 'C') {
            if (t->input_pos < t->input_len) t->input_pos++;
            return 0;
        }
        /* Home \033[H */
        if (c == 'H') { t->input_pos = 0; return 0; }
        /* End  \033[F */
        if (c == 'F') { t->input_pos = t->input_len; return 0; }
        /* Delete \033[3~ — need one more char '~' */
        if (c == '3') {
            /* We can't easily consume the ~ here without blocking;
               handle it on the next call via a small sub-state.
               Simple approach: just delete char at pos now. */
            if (t->input_pos < t->input_len) {
                for (int i = t->input_pos; i < t->input_len - 1; i++)
                    t->input[i] = t->input[i+1];
                t->input_len--;
                t->input[t->input_len] = '\0';
            }
            return 0;
        }
        /* ignore other CSI sequences */
        return 0;
    }

    /* start of ESC sequence */
    if (c == 0x1B) { t->esc_state = 1; return 0; }

    /* Enter */
    if (c == '\r' || c == '\n') {
        t->input[t->input_len] = '\0';
        _hist_push(t, t->input);
        t->hist_idx      = -1;
        t->hist_saved[0] = '\0';
        return 1;
    }
    /* Backspace */
    if (c == 0x7F || c == 0x08) {
        if (t->input_pos > 0) {
            for (int i = t->input_pos - 1; i < t->input_len - 1; i++)
                t->input[i] = t->input[i+1];
            t->input_len--;
            t->input_pos--;
            t->input[t->input_len] = '\0';
        }
        return 0;
    }
    /* Ctrl-C */
    if (c == 0x03) {
        term_println(t, "^C", TC_RED);
        t->input[0]  = '\0'; t->input_len = 0; t->input_pos = 0;
        t->hist_idx  = -1;
        return 0;
    }
    /* Ctrl-L */
    if (c == 0x0C) { term_clear(t); return 0; }
    /* Ctrl-A — go to beginning */
    if (c == 0x01) { t->input_pos = 0; return 0; }
    /* Ctrl-E — go to end */
    if (c == 0x05) { t->input_pos = t->input_len; return 0; }
    /* Ctrl-K — kill to end */
    if (c == 0x0B) { t->input[t->input_pos] = '\0'; t->input_len = t->input_pos; return 0; }
    /* printable — insert at cursor */
    if ((unsigned char)c >= 0x20 && t->input_len < TERM_INPUT - 1) {
        for (int i = t->input_len; i > t->input_pos; i--)
            t->input[i] = t->input[i-1];
        t->input[t->input_pos] = c;
        t->input_len++;
        t->input_pos++;
        t->input[t->input_len] = '\0';
    }
    return 0;
}

/*  render  */
void term_render(term_buf_t *t, uint32_t bg_color)
{
    int cx = t->wx + 8;          /* content left margin  */
    int cy = t->content_y + 4;   /* content top          */
    int max_h = t->wy + t->wh - cy - LINE_H - 4;  /* leave room for input */
    int visible = max_h / LINE_H;
    if (visible < 1) visible = 1;
    if (visible > TERM_ROWS) visible = TERM_ROWS;

    /* clear content area */
    fb_rect((uint32_t)t->wx + 1, (uint32_t)t->content_y,
            (uint32_t)(t->ww - 2),
            (uint32_t)(t->wh - (t->content_y - t->wy) - 1),
            bg_color);

    /* figure out which lines to show */
    int total = t->count;                          /* total lines written */
    int start = total - visible - t->scroll_off;   /* oldest visible line */
    if (start < 0) start = 0;

    int row = 0;
    for (int li = start; li < start + visible && li < total; li++) {
        int slot = li % TERM_ROWS;
        const char *text = t->lines[slot].text;
        uint32_t   color = t->lines[slot].color;
        if (*text)
            fb_str((uint32_t)cx, (uint32_t)(cy + row * LINE_H),
                   text, color, bg_color);
        row++;
    }

    /*  input line  */
    int input_y = t->wy + t->wh - LINE_H - 6;

    /* separator above input */
    fb_rect((uint32_t)(t->wx+1), (uint32_t)(input_y - 2),
            (uint32_t)(t->ww-2), 1, TC_GRAY);

    /* clear input area */
    fb_rect((uint32_t)(t->wx+1), (uint32_t)(input_y),
            (uint32_t)(t->ww-2), (uint32_t)LINE_H, bg_color);

    /* prompt */
    const char *prompt = "root@exploidus:~$ ";
    fb_str((uint32_t)cx, (uint32_t)input_y, prompt, TC_PROMPT, bg_color);

    int prompt_px = cx + tstrlen(prompt) * FONT_W;

    /* input text */
    if (t->input_len > 0)
        fb_str((uint32_t)prompt_px, (uint32_t)input_y,
               t->input, TC_FG, bg_color);

    /* cursor (blinking block) */
    int cursor_x = prompt_px + t->input_pos * FONT_W;
    uint64_t now = uptime() * 2;   /* ~0.5s blink */
    if ((now & 1) == 0) {
        /* cursor on */
        char cur_ch[2] = {' ', '\0'};
        if (t->input_pos < t->input_len)
            cur_ch[0] = t->input[t->input_pos];
        fb_rect((uint32_t)cursor_x, (uint32_t)input_y,
                FONT_W, LINE_H, TC_PROMPT);
        fb_str((uint32_t)cursor_x, (uint32_t)input_y,
               cur_ch, bg_color, TC_PROMPT);
    }

    /* scroll indicator */
    if (t->scroll_off > 0) {
        fb_str((uint32_t)(t->wx + t->ww - 48),
               (uint32_t)(t->content_y + 2),
               "^scroll^", TC_YELLOW, bg_color);
    }
}