/*
 * term_buf.h — framebuffer terminal emulator for alien-gui
 *
 * Provides a scrollable text buffer that renders inside a win_t.
 * Handles keyboard input, line editing, command dispatch.
 */

#pragma once
#include <stdint.h>

#define TERM_COLS     80     /* max chars per line            */
#define TERM_ROWS     32     /* lines kept in scroll buffer   */
#define TERM_INPUT    256    /* max input line length         */

/* one line in the scroll buffer */
typedef struct {
    char    text[TERM_COLS + 1];
    uint32_t color;          /* foreground color */
} term_line_t;

typedef struct {
    term_line_t lines[TERM_ROWS];
    int         count;       /* total lines written (wraps)   */
    int         scroll_off;  /* lines scrolled up from bottom */

    /* current input state */
    char        input[TERM_INPUT];
    int         input_len;
    int         input_pos;   /* cursor position in input      */

    /* blink state */
    uint64_t    blink_tick;
    int         blink_on;

    /* command history */
#define TERM_HIST_SIZE  32
#define TERM_HIST_LEN   256
    char        hist_buf[TERM_HIST_SIZE][TERM_HIST_LEN];
    int         hist_count;              /* total entries stored          */
    int         hist_head;               /* next write slot (ring buffer) */
    int         hist_idx;                /* -1=live  >=0=browsing         */
    char        hist_saved[TERM_HIST_LEN]; /* saved live input            */

    /* escape-sequence parser state */
    int         esc_state;   /* 0=normal  1=got-ESC  2=got-ESC-[ */

    /* window geometry (set by caller) */
    int         wx, wy, ww, wh;  /* window rect                */
    int         content_y;       /* y where content starts     */
} term_buf_t;

/* initialize */
void term_init(term_buf_t *t, int wx, int wy, int ww, int wh, int content_y);

/* append a line of text */
void term_println(term_buf_t *t, const char *s, uint32_t color);

/* append without newline */
void term_print(term_buf_t *t, const char *s, uint32_t color);

/* clear */
void term_clear(term_buf_t *t);

/* feed one keyboard char — returns 1 if line ready in t->input */
int  term_key(term_buf_t *t, char c);

/* render the terminal into the framebuffer */
void term_render(term_buf_t *t, uint32_t bg_color);

/* get the completed input line (call after term_key returns 1) */
const char *term_get_line(term_buf_t *t);