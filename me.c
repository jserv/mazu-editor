/* Mazu Editor:
 * A minimalist editor with syntax highlight, copy/paste, undo, and search.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CTRL_(k) ((k) & (0x1f))
#define TAB_STOP 4

/* UTF-8 handling functions */

/* Get the byte length of a UTF-8 character from its first byte */
static inline int utf8_byte_length(uint8_t c)
{
    /* Quick check for ASCII */
    if ((c & 0x80) == 0)
        return 1;

    /* Use proper validation for multibyte sequences */
    if ((c & 0xE0) == 0xC0 && c >= 0xC2) /* Valid 2-byte start */
        return 2;
    if ((c & 0xF0) == 0xE0) /* 3-byte start */
        return 3;
    if ((c & 0xF8) == 0xF0 && c <= 0xF4) /* Valid 4-byte start */
        return 4;

    return 1; /* Invalid UTF-8 start byte, treat as single byte */
}

/* Check if byte is a UTF-8 continuation byte (10xxxxxx) */
static inline bool is_utf8_continuation(uint8_t c)
{
    return (c & 0xC0) == 0x80;
}

static int utf8_to_codepoint(const char *s, size_t max_len);

/* Get display width of a UTF-8 character (handles wide characters)
 * Returns 2 for CJK characters, 1 for most others, 0 for combining marks
 */
static inline int utf8_char_width(const char *s)
{
    /* Use the enhanced UTF-8 to codepoint conversion */
    int codepoint = utf8_to_codepoint(s, 4);

    /* Handle invalid UTF-8 */
    if (codepoint < 0)
        return 1;

    /* ASCII control characters */
    if (codepoint < 0x20 || codepoint == 0x7F)
        return 0;

    /* CJK Unified Ideographs and common fullwidth ranges */
    if ((codepoint >= 0x4E00 &&
         codepoint <= 0x9FFF) || /* CJK Unified Ideographs */
        (codepoint >= 0x3400 && codepoint <= 0x4DBF) || /* CJK Extension A */
        (codepoint >= 0xF900 && codepoint <= 0xFAFF) || /* CJK Compatibility */
        (codepoint >= 0x2E80 && codepoint <= 0x2EFF) || /* CJK Radicals */
        (codepoint >= 0x3000 && codepoint <= 0x303F) || /* CJK Punctuation */
        (codepoint >= 0xFF00 && codepoint <= 0xFFEF)) { /* Fullwidth forms */
        return 2;
    }

    /* Combining marks have zero width */
    if ((codepoint >= 0x0300 &&
         codepoint <= 0x036F) || /* Combining Diacritical Marks */
        (codepoint >= 0x1AB0 &&
         codepoint <= 0x1AFF) || /* Combining Diacritical Extended */
        (codepoint >= 0x1DC0 &&
         codepoint <= 0x1DFF)) { /* Combining Diacritical Supplement */
        return 0;
    }

    return 1;
}

/* Move to the next UTF-8 character boundary */
static inline const char *utf8_next_char(const char *s)
{
    if (*s == '\0')
        return s;
    int len = utf8_byte_length((uint8_t) *s);
    return s + len;
}

/* Move to the previous UTF-8 character boundary */
static inline const char *utf8_prev_char(const char *start, const char *s)
{
    if (s <= start)
        return start;
    --s;
    while (s > start && is_utf8_continuation((uint8_t) *s))
        --s;
    return s;
}

/* Forward declaration needed early for debugging */
static void set_status_message(const char *msg, ...);

/* Validate UTF-8 byte sequence and return its length
 * Returns 0 if invalid, otherwise returns number of bytes (1-4)
 */
static int utf8_validate(const char *s, size_t max_len)
{
    if (!s || max_len == 0)
        return 0;

    unsigned char c = (unsigned char) *s;

    /* ASCII character (0xxxxxxx) */
    if (c <= 0x7F)
        return 1;

    /* Invalid UTF-8 start byte */
    if (c < 0xC0 || c > 0xF7)
        return 0;

    /* 2-byte sequence (110xxxxx 10xxxxxx) */
    if (c <= 0xDF) {
        if (max_len < 2)
            return 0;
        if ((s[1] & 0xC0) != 0x80)
            return 0;
        /* Check for overlong encoding */
        if (c < 0xC2)
            return 0;
        return 2;
    }

    /* 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx) */
    if (c <= 0xEF) {
        if (max_len < 3)
            return 0;
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
            return 0;
        /* Check for overlong encoding and surrogates */
        if (c == 0xE0 && (unsigned char) s[1] < 0xA0)
            return 0;
        if (c == 0xED && (unsigned char) s[1] > 0x9F)
            return 0;
        return 3;
    }

    /* 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx) */
    if (c <= 0xF4) {
        if (max_len < 4)
            return 0;
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
            (s[3] & 0xC0) != 0x80)
            return 0;
        /* Check for overlong encoding and valid range */
        if (c == 0xF0 && (unsigned char) s[1] < 0x90)
            return 0;
        if (c == 0xF4 && (unsigned char) s[1] > 0x8F)
            return 0;
        return 4;
    }

    return 0;
}

/* Convert UTF-8 sequence to Unicode codepoint
 * Returns the codepoint value, or -1 if invalid
 */
static int utf8_to_codepoint(const char *s, size_t max_len)
{
    int len = utf8_validate(s, max_len);
    if (len == 0)
        return -1;

    unsigned char c = (unsigned char) *s;

    if (len == 1)
        return c;

    if (len == 2)
        return ((c & 0x1F) << 6) | (s[1] & 0x3F);

    if (len == 3)
        return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);

    if (len == 4)
        return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
               ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);

    return -1;
}

/* A gap buffer maintains a gap (empty space) at the cursor position,
 * making insertions and deletions at that position O(1) operations.
 * Memory layout: [text before gap][    GAP    ][text after gap]
 */
typedef struct {
    char *buffer;  /* Start of buffer */
    char *gap;     /* Start of gap */
    char *egap;    /* End of gap */
    char *ebuffer; /* End of buffer */
    size_t size;   /* Total allocated size */
    bool modified; /* Modified flag */
} gap_buffer_t;

#define GB_INITIAL_SIZE 65536 /* 64KB initial buffer */
#define GB_GROW_SIZE 4096     /* 4KB growth increment */

/* Initialize a gap buffer with given size */
static gap_buffer_t *gb_init(size_t initial_size)
{
    gap_buffer_t *gb = malloc(sizeof(gap_buffer_t));
    if (!gb)
        return NULL;

    gb->buffer = malloc(initial_size);
    if (!gb->buffer) {
        free(gb);
        return NULL;
    }

    gb->size = initial_size;
    gb->gap = gb->buffer;
    gb->egap = gb->ebuffer = gb->buffer + initial_size;
    gb->modified = false;

    return gb;
}

/* Get total text length (excluding gap) */
static size_t gb_length(gap_buffer_t *gb)
{
    return (gb->gap - gb->buffer) + (gb->ebuffer - gb->egap);
}

/* Convert file position to buffer pointer */
static char *gb_ptr(gap_buffer_t *gb, size_t pos)
{
    size_t front_size = gb->gap - gb->buffer;

    if (pos <= front_size)
        return gb->buffer + pos;
    return gb->egap + (pos - front_size);
}

/* Move gap to position */
static void gb_move_gap(gap_buffer_t *gb, size_t pos)
{
    char *dest = gb_ptr(gb, pos);

    if (dest < gb->gap) {
        /* Move gap backward - shift text forward */
        size_t len = gb->gap - dest;
        gb->egap -= len;
        gb->gap -= len;
        memmove(gb->egap, gb->gap, len);
    } else if (dest > gb->gap) {
        /* Move gap forward - shift text backward */
        size_t len = dest - gb->gap;
        memmove(gb->gap, gb->egap, len);
        gb->gap += len;
        gb->egap += len;
    }
}

/* Grow the gap to ensure minimum size */
static bool gb_grow_gap(gap_buffer_t *gb, size_t min_gap)
{
    size_t gap_size = gb->egap - gb->gap;

    if (gap_size >= min_gap)
        return true; /* Already large enough */

    /* Calculate new size */
    size_t text_size = gb_length(gb);
    size_t new_size = text_size + min_gap + GB_GROW_SIZE;

    /* Save current gap position */
    size_t gap_pos = gb->gap - gb->buffer;
    size_t after_gap_size = gb->ebuffer - gb->egap;

    /* Reallocate buffer */
    char *new_buffer = realloc(gb->buffer, new_size);
    if (!new_buffer)
        return false; /* Allocation failed */

    /* Update pointers */
    gb->buffer = new_buffer;
    gb->gap = new_buffer + gap_pos;
    gb->ebuffer = new_buffer + new_size;
    gb->egap = gb->ebuffer - after_gap_size;

    /* Move text after gap to end of new buffer */
    if (after_gap_size > 0)
        memmove(gb->egap, gb->gap + gap_size, after_gap_size);

    gb->size = new_size;
    return true;
}

/* Insert text at position */
static bool gb_insert(gap_buffer_t *gb,
                      size_t pos,
                      const char *text,
                      size_t len)
{
    /* Move gap to insertion point */
    gb_move_gap(gb, pos);

    /* Ensure gap is large enough */
    if (!gb_grow_gap(gb, len))
        return false; /* Failed to grow */

    /* Copy text into gap */
    memcpy(gb->gap, text, len);
    gb->gap += len;
    gb->modified = true;

    return true;
}

/* Delete text from position */
static void gb_delete(gap_buffer_t *gb, size_t pos, size_t len)
{
    /* Move gap to deletion point */
    gb_move_gap(gb, pos);

    /* Extend gap to cover deleted text */
    size_t available = gb->ebuffer - gb->egap;
    if (len > available)
        len = available; /* Can't delete more than exists */

    gb->egap += len;
    gb->modified = true;
}

/* Get character at position */
static int gb_get_char(gap_buffer_t *gb, size_t pos)
{
    if (pos >= gb_length(gb))
        return -1; /* Out of bounds */

    char *ptr = gb_ptr(gb, pos);
    return (unsigned char) *ptr;
}

/* Load file into gap buffer */
static bool gb_load_file(gap_buffer_t *gb, FILE *fp)
{
    /* Clear existing content */
    gb->gap = gb->buffer;
    gb->egap = gb->ebuffer;

    char buf[4096];
    size_t nread;

    while ((nread = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (!gb_insert(gb, gb_length(gb), buf, nread))
            return false; /* Insert failed */
    }

    gb->modified = false; /* Just loaded, not modified */
    return true;
}

/* Undo/Redo Implementation */

typedef enum { UNDO_INSERT, UNDO_DELETE, UNDO_REPLACE } undo_type_t;

typedef struct undo_node {
    undo_type_t type;
    size_t pos; /* Position where change occurred */
    size_t len; /* Length of text */
    char *text; /* Text that was inserted/deleted */
    struct undo_node *next, *prev;
} undo_node_t;

typedef struct {
    undo_node_t *current;     /* Current position in undo history */
    undo_node_t *head, *tail; /* First/Last undo node */
    int max_undos;            /* Maximum number of undo levels */
    int count;                /* Current number of undo nodes */
} undo_stack_t;

#define MAX_UNDO_LEVELS 100

/* Initialize undo stack */
static undo_stack_t *undo_init(int max_levels)
{
    undo_stack_t *stack = malloc(sizeof(undo_stack_t));
    if (!stack)
        return NULL;

    stack->head = NULL;
    stack->tail = NULL;
    stack->current = NULL;
    stack->max_undos = max_levels;
    stack->count = 0;

    return stack;
}

/* Free a single undo node */
static void undo_free_node(undo_node_t *node)
{
    if (node) {
        free(node->text);
        free(node);
    }
}

/* Clear redo history (called when new edit is made) */
static void undo_clear_redo(undo_stack_t *stack)
{
    if (!stack || !stack->current)
        return;

    /* Remove all nodes after current */
    undo_node_t *node = stack->current->next;
    while (node) {
        undo_node_t *next = node->next;
        undo_free_node(node);
        stack->count--;
        node = next;
    }

    /* Update tail */
    stack->tail = stack->current;
    if (stack->tail)
        stack->tail->next = NULL;
}

/* Add a new undo operation */
static void undo_push(undo_stack_t *stack,
                      undo_type_t type,
                      size_t pos,
                      const char *text,
                      size_t len)
{
    if (!stack || !text || len == 0)
        return;

    /* Clear any redo history */
    undo_clear_redo(stack);

    /* Create new node */
    undo_node_t *node = malloc(sizeof(undo_node_t));
    if (!node)
        return;

    node->type = type;
    node->pos = pos;
    node->len = len;
    node->text = malloc(len + 1);
    if (!node->text) {
        free(node);
        return;
    }
    memcpy(node->text, text, len);
    node->text[len] = '\0';

    /* Link node into list */
    node->prev = stack->current;
    node->next = NULL;

    if (stack->current) {
        stack->current->next = node;
    } else {
        stack->head = node;
    }

    stack->tail = node;
    stack->current = node;
    stack->count++;

    /* Remove oldest if we exceed max */
    while (stack->count > stack->max_undos && stack->head) {
        undo_node_t *old = stack->head;
        stack->head = old->next;
        if (stack->head)
            stack->head->prev = NULL;
        undo_free_node(old);
        stack->count--;
    }
}

/* Forward declarations - will be defined later */
static void gb_sync_to_rows(gap_buffer_t *gb);

/* Perform undo operation */
static bool undo_perform(gap_buffer_t *gb, undo_stack_t *stack)
{
    if (!gb || !stack || !stack->current)
        return false; /* Nothing to undo */

    undo_node_t *node = stack->current;

    /* Reverse the operation WITHOUT adding to undo stack again */
    switch (node->type) {
    case UNDO_INSERT:
        /* Was an insert, so delete it */
        gb_delete(gb, node->pos, node->len);
        break;

    case UNDO_DELETE:
        /* Was a delete, so insert it back */
        gb_insert(gb, node->pos, node->text, node->len);
        break;

    case UNDO_REPLACE:
        /* For replace, we need the old text (stored in next node) */
        /* For now, treat as delete + insert */
        gb_delete(gb, node->pos, node->len);
        if (node->prev && node->prev->type == UNDO_DELETE)
            gb_insert(gb, node->pos, node->prev->text, node->prev->len);
        break;
    }

    /* Move current pointer back */
    stack->current = node->prev;

    /* Sync gap buffer back to rows for display */
    gb_sync_to_rows(gb);

    /* Mark as modified if we have undo history */
    gb->modified = (stack->current != NULL);

    return true;
}

/* Perform redo operation */
static bool undo_redo(gap_buffer_t *gb, undo_stack_t *stack)
{
    if (!gb || !stack)
        return false;

    /* Find the node to redo */
    undo_node_t *node = NULL;
    if (stack->current) {
        node = stack->current->next;
    } else if (stack->head) {
        node = stack->head;
    }

    if (!node)
        return false; /* Nothing to redo */

    /* Re-apply the operation */
    switch (node->type) {
    case UNDO_INSERT:
        /* Re-insert the text */
        gb_insert(gb, node->pos, node->text, node->len);
        break;

    case UNDO_DELETE:
        /* Re-delete the text */
        gb_delete(gb, node->pos, node->len);
        break;

    case UNDO_REPLACE:
        /* Re-replace the text */
        gb_delete(gb, node->pos, node->len);
        gb_insert(gb, node->pos, node->text, node->len);
        break;
    }

    /* Move current pointer forward */
    stack->current = node;

    /* Sync gap buffer back to rows for display */
    gb_sync_to_rows(gb);

    return true;
}

/* Track insertion for undo (wrapper for gb_insert) */
static bool gb_insert_with_undo(gap_buffer_t *gb,
                                undo_stack_t *undo,
                                size_t pos,
                                const char *text,
                                size_t len)
{
    if (!gb_insert(gb, pos, text, len))
        return false;

    if (undo)
        undo_push(undo, UNDO_INSERT, pos, text, len);

    return true;
}

/* Track deletion for undo (wrapper for gb_delete) */
static void gb_delete_with_undo(gap_buffer_t *gb,
                                undo_stack_t *undo,
                                size_t pos,
                                size_t len)
{
    if (undo && len > 0 && pos < gb_length(gb)) {
        /* Save the text being deleted */
        char *text = malloc(len + 1);
        if (text) {
            size_t i;
            for (i = 0; i < len && pos + i < gb_length(gb); i++) {
                int ch = gb_get_char(gb, pos + i);
                if (ch == -1)
                    break;
                text[i] = ch;
            }
            text[i] = '\0';

            if (i > 0)
                undo_push(undo, UNDO_DELETE, pos, text, i);
            free(text);
        }
    }

    gb_delete(gb, pos, len);
}

typedef struct {
    int idx;
    int size;
    int render_size;
    char *chars;
    char *render;
    unsigned char *highlight;
    bool hl_open_comment;
} editor_row_t;

/* Syntax highlighting structure */
typedef struct {
    char *file_type;
    char **file_match;
    char **keywords;
    char *sl_comment_start;                  /* single line */
    char *ml_comment_start, *ml_comment_end; /* multiple lines */
    int flags;
} editor_syntax_t;

/* Editor config structure */
struct {
    int cursor_x, cursor_y, render_x;
    int row_offset, col_offset;
    int screen_rows, screen_cols;
    int num_rows;
    editor_row_t *row;
    bool modified;
    char *file_name;
    char status_msg[90];
    time_t status_msg_time;
    char *copied_char_buffer;
    editor_syntax_t *syntax;
    struct termios orig_termios;
    /* Gap buffer and undo/redo support */
    gap_buffer_t *gb;
    undo_stack_t *undo_stack;
} ec = {
    .cursor_x = 0,
    .cursor_y = 0,
    .render_x = 0,
    .row_offset = 0,
    .col_offset = 0,
    .num_rows = 0,
    .row = NULL,
    .modified = false,
    .file_name = NULL,
    .status_msg[0] = '\0',
    .status_msg_time = 0,
    .copied_char_buffer = NULL,
    .syntax = NULL,
    .gb = NULL,
    .undo_stack = NULL,
};

typedef struct {
    char *buf;
    int len;
} editor_buf_t;

/* clang-format off */
enum editor_key {
    BACKSPACE = 0x7f,
    ARROW_LEFT = 0x3e8, ARROW_RIGHT, ARROW_UP, ARROW_DOWN,
    PAGE_UP, PAGE_DOWN,
    HOME_KEY, END_KEY, DEL_KEY,
};

enum editor_highlight {
    NORMAL,     MATCH,
    SL_COMMENT, ML_COMMENT,
    KEYWORD_1,  KEYWORD_2,  KEYWORD_3,
    STRING,     NUMBER,
};
/* clang-format on */

#define HIGHLIGHT_NUMBERS (1 << 0)
#define HIGHLIGHT_STRINGS (1 << 1)

char *C_extensions[] = {".c", ".cc", ".cxx", ".cpp", ".h", NULL};

char *C_keywords[] = {
    "switch",  "if",       "while",   "for",      "break",    "continue",
    "return",  "else",     "struct",  "union",    "typedef",  "static",
    "enum",    "class",    "case",    "volatile", "register", "sizeof",
    "typedef", "union",    "goto",    "const",    "auto",     "#if",
    "#endif",  "#error",   "#ifdef",  "#ifndef",  "#elif",    "#define",
    "#undef",  "#include",

    "int|",    "long|",    "double|", "float|",   "char|",    "unsigned|",
    "signed|", "void|",    "bool|",   NULL,
};

editor_syntax_t DB[] = {
    {
        "c",
        C_extensions,
        C_keywords,
        "//",
        "/*",
        "*/",
        HIGHLIGHT_NUMBERS | HIGHLIGHT_STRINGS,
    },
};

#define DB_ENTRIES (sizeof(DB) / sizeof(DB[0]))

static char *prompt(const char *msg, void (*callback)(char *, int));

static void clear_screen()
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
}

static void panic(const char *s)
{
    clear_screen();
    perror(s);
    puts("\r\n");
    exit(1);
}

static void open_buffer()
{
    if (write(STDOUT_FILENO, "\x1b[?47h", 6) == -1)
        panic("Error changing terminal buffer");
}

static void disable_raw_mode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ec.orig_termios) == -1)
        panic("Failed to disable raw mode");
}

static void enable_raw_mode()
{
    if (tcgetattr(STDIN_FILENO, &ec.orig_termios) == -1)
        panic("Failed to get current terminal state");
    atexit(disable_raw_mode);
    struct termios raw = ec.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    open_buffer();
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        panic("Failed to set raw mode");
}

static int read_key()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if ((nread == -1) && (errno != EAGAIN))
            panic("Error reading input");
    }
    if (c == '\x1b') {
        char seq[3];
        if ((read(STDIN_FILENO, &seq[0], 1) != 1) ||
            (read(STDIN_FILENO, &seq[1], 1) != 1))
            return '\x1b';
        if (seq[0] == '[') {
            if (isdigit(seq[1])) {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '1':
                    case '7':
                        return HOME_KEY;
                    case '4':
                    case '8':
                        return END_KEY;
                    case '3':
                        return DEL_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }
        return '\x1b';
    }
    return c;
}

static int get_window_size(int *screen_rows, int *screen_cols)
{
    struct winsize ws;
    if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) || (ws.ws_col == 0))
        return -1;
    *screen_cols = ws.ws_col;
    *screen_rows = ws.ws_row;
    return 0;
}

static void update_window_size()
{
    if (get_window_size(&ec.screen_rows, &ec.screen_cols) == -1)
        panic("Failed to get window size");
    ec.screen_rows -= 2;
}

static void close_buffer()
{
    if (write(STDOUT_FILENO, "\x1b[?9l", 5) == -1 ||
        write(STDOUT_FILENO, "\x1b[?47l", 6) == -1)
        panic("Error restoring buffer state");
    clear_screen();
}

static bool is_token_separator(int c)
{
    return isspace(c) || (c == '\0') || strchr(",.()+-/*=~%<>[]:;", c);
}

static bool is_part_of_number(int c)
{
    return c == '.' || c == 'x' || c == 'a' || c == 'b' || c == 'c' ||
           c == 'd' || c == 'e' || c == 'f' || c == 'A' || c == 'X' ||
           c == 'B' || c == 'C' || c == 'D' || c == 'E' || c == 'F' ||
           c == 'h' || c == 'H';
}

static void highlight(editor_row_t *row)
{
    row->highlight = realloc(row->highlight, row->render_size);
    memset(row->highlight, NORMAL, row->render_size);
    if (!ec.syntax)
        return;
    char **keywords = ec.syntax->keywords;
    char *scs = ec.syntax->sl_comment_start;
    char *mcs = ec.syntax->ml_comment_start;
    char *mce = ec.syntax->ml_comment_end;
    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;
    bool prev_sep = true;
    int in_string = 0;
    bool in_comment = (row->idx > 0 && ec.row[row->idx - 1].hl_open_comment);
    int i = 0;
    while (i < row->render_size) {
        char c = row->render[i];
        unsigned char prev_highlight = (i > 0) ? row->highlight[i - 1] : NORMAL;
        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->highlight[i], SL_COMMENT, row->render_size - i);
                break;
            }
        }
        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->highlight[i] = ML_COMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->highlight[i], ML_COMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = true;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->highlight[i], ML_COMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }
        if (ec.syntax->flags & HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->highlight[i] = STRING;
                if ((c == '\\') && (i + 1 < row->render_size)) {
                    row->highlight[i + 1] = STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string)
                    in_string = 0;
                i++;
                prev_sep = true;
                continue;
            } else {
                if ((c == '"') || (c == '\'')) {
                    in_string = c;
                    row->highlight[i] = STRING;
                    i++;
                    continue;
                }
            }
        }
        if (ec.syntax->flags & HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_sep || (prev_highlight == NUMBER))) ||
                (is_part_of_number(c) && (prev_highlight == NUMBER))) {
                row->highlight[i] = NUMBER;
                i++;
                prev_sep = false;
                continue;
            }
        }
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int kw_len = strlen(keywords[j]);
                bool kw_2 = keywords[j][kw_len - 1] == '|';
                /* FIXME: specific to C/C++, should be generic */
                bool kw_3 = keywords[j][0] == '#';
                if (kw_2)
                    kw_len--;
                if (!strncmp(&row->render[i], keywords[j], kw_len) &&
                    is_token_separator(row->render[i + kw_len])) {
                    memset(&row->highlight[i],
                           kw_2 ? KEYWORD_2 : (kw_3 ? KEYWORD_3 : KEYWORD_1),
                           kw_len);
                    i += kw_len;
                    break;
                }
            }
            if (keywords[j]) {
                prev_sep = false;
                continue;
            }
        }
        prev_sep = is_token_separator(c);
        i++;
    }
    bool changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < ec.num_rows)
        highlight(&ec.row[row->idx + 1]);
}

/* Reference: https://misc.flogisoft.com/bash/tip_colors_and_formatting */
static int token_to_color(int highlight)
{
    switch (highlight) {
    case SL_COMMENT:
    case ML_COMMENT:
        return 36; /* Cyan */
    case KEYWORD_1:
        return 93; /* Light yellow */
    case KEYWORD_2:
        return 92; /* Light green */
    case KEYWORD_3:
        return 36; /* Cyan */
    case STRING:
        return 91; /* Light red */
    case NUMBER:
        return 31; /* Red */
    case MATCH:
        return 43; /* Yellow background */
    default:
        return 97;
    }
}

static void select_highlight()
{
    ec.syntax = NULL;
    if (!ec.file_name)
        return;
    for (size_t j = 0; j < DB_ENTRIES; j++) {
        editor_syntax_t *es = &DB[j];
        for (size_t i = 0; es->file_match[i]; i++) {
            char *p = strstr(ec.file_name, es->file_match[i]);
            if (!p)
                continue;
            int pat_len = strlen(es->file_match[i]);
            if ((es->file_match[i][0] != '.') || (p[pat_len] == '\0')) {
                ec.syntax = es;
                for (int file_row = 0; file_row < ec.num_rows; file_row++)
                    highlight(&ec.row[file_row]);
                return;
            }
        }
    }
}

static int row_cursorx_to_renderx(editor_row_t *row, int cursor_x)
{
    int render_x = 0;
    int byte_pos = 0;

    while (byte_pos < row->size && byte_pos < cursor_x) {
        if (row->chars[byte_pos] == '\t') {
            render_x += (TAB_STOP - 1) - (render_x % TAB_STOP);
            render_x++;
            byte_pos++;
        } else {
            int char_width = utf8_char_width(&row->chars[byte_pos]);
            render_x += char_width;
            byte_pos += utf8_byte_length((uint8_t) row->chars[byte_pos]);
        }
    }
    return render_x;
}

static int row_renderx_to_cursorx(editor_row_t *row, int render_x)
{
    int cur_render_x = 0;
    int byte_pos = 0;

    while (byte_pos < row->size) {
        int next_render_x = cur_render_x;

        if (row->chars[byte_pos] == '\t') {
            next_render_x += (TAB_STOP - 1) - (cur_render_x % TAB_STOP);
            next_render_x++;
        } else {
            next_render_x += utf8_char_width(&row->chars[byte_pos]);
        }

        if (next_render_x > render_x)
            return byte_pos;

        cur_render_x = next_render_x;

        if (row->chars[byte_pos] == '\t') {
            byte_pos++;
        } else {
            byte_pos += utf8_byte_length((uint8_t) row->chars[byte_pos]);
        }
    }
    return byte_pos;
}

static void update_row(editor_row_t *row)
{
    int tabs = 0;
    int wide_chars = 0;
    int byte_pos = 0;

    /* Count tabs and wide characters for buffer allocation */
    while (byte_pos < row->size) {
        if (row->chars[byte_pos] == '\t') {
            tabs++;
            byte_pos++;
        } else {
            int char_len = utf8_byte_length((uint8_t) row->chars[byte_pos]);
            int char_width = utf8_char_width(&row->chars[byte_pos]);
            if (char_width > 1) {
                wide_chars += (char_width - 1);
            }
            byte_pos += char_len;
        }
    }

    free(row->render);
    /* Allocate extra space for tabs and wide characters */
    row->render = malloc(row->size + tabs * (TAB_STOP - 1) + wide_chars + 1);

    int idx = 0;
    byte_pos = 0;

    while (byte_pos < row->size) {
        if (row->chars[byte_pos] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0)
                row->render[idx++] = ' ';
            byte_pos++;
        } else {
            int char_len = utf8_byte_length((uint8_t) row->chars[byte_pos]);
            /* Copy the UTF-8 sequence as-is */
            for (int i = 0; i < char_len && byte_pos + i < row->size; i++)
                row->render[idx++] = row->chars[byte_pos + i];
            byte_pos += char_len;
        }
    }
    row->render[idx] = '\0';
    row->render_size = idx;
    highlight(row);
}

static void insert_row(int at, char *s, size_t line_len)
{
    if ((at < 0) || (at > ec.num_rows))
        return;
    ec.row = realloc(ec.row, sizeof(editor_row_t) * (ec.num_rows + 1));
    memmove(&ec.row[at + 1], &ec.row[at],
            sizeof(editor_row_t) * (ec.num_rows - at));
    for (int j = at + 1; j <= ec.num_rows; j++)
        ec.row[j].idx++;
    ec.row[at].idx = at;
    ec.row[at].size = line_len;
    ec.row[at].chars = malloc(line_len + 1);
    memcpy(ec.row[at].chars, s, line_len);
    ec.row[at].chars[line_len] = '\0';
    ec.row[at].render_size = 0;
    ec.row[at].render = NULL;
    ec.row[at].highlight = NULL;
    ec.row[at].hl_open_comment = false;
    update_row(&ec.row[at]);
    ec.num_rows++;
    ec.modified++;
}

static void copy(int cut)
{
    if (!ec.gb || ec.cursor_y >= ec.num_rows)
        return;

    size_t len = strlen(ec.row[ec.cursor_y].chars) + 1;
    ec.copied_char_buffer = realloc(ec.copied_char_buffer, len);
    if (!ec.copied_char_buffer) {
        set_status_message("Memory allocation failed");
        return;
    }
    snprintf(ec.copied_char_buffer, len, "%s", ec.row[ec.cursor_y].chars);
    set_status_message(cut ? "Text cut" : "Text copied");
}

static void cut()
{
    if (!ec.gb || ec.cursor_y >= ec.num_rows)
        return;

    copy(-1);

    /* Calculate line position in gap buffer */
    size_t line_start = 0;
    for (int i = 0; i < ec.cursor_y; i++)
        line_start += ec.row[i].size + 1;

    /* Delete entire line including newline */
    size_t line_len = ec.row[ec.cursor_y].size;
    if (ec.cursor_y < ec.num_rows - 1)
        line_len++; /* Include newline */

    gb_delete_with_undo(ec.gb, ec.undo_stack, line_start, line_len);

    /* Remove row from display structure */
    editor_row_t *row = &ec.row[ec.cursor_y];
    free(row->render);
    free(row->chars);
    free(row->highlight);

    if (ec.num_rows > 1) {
        memmove(&ec.row[ec.cursor_y], &ec.row[ec.cursor_y + 1],
                sizeof(editor_row_t) * (ec.num_rows - ec.cursor_y - 1));
        for (int j = ec.cursor_y; j < ec.num_rows - 1; j++)
            ec.row[j].idx--;
        ec.num_rows--;
    } else {
        /* Last row - replace with empty row */
        ec.row[0].size = 0;
        ec.row[0].chars = malloc(1);
        ec.row[0].chars[0] = '\0';
        ec.row[0].render = NULL;
        ec.row[0].render_size = 0;
        ec.row[0].highlight = NULL;
        update_row(&ec.row[0]);
    }

    /* Adjust cursor */
    if (ec.cursor_y >= ec.num_rows && ec.num_rows > 0)
        ec.cursor_y = ec.num_rows - 1;
    ec.cursor_x = 0;
    ec.modified = true;
}

static void paste()
{
    if (!ec.copied_char_buffer || !ec.gb)
        return;

    /* Calculate position in gap buffer */
    size_t pos = 0;
    for (int i = 0; i < ec.cursor_y && i < ec.num_rows; i++)
        pos += ec.row[i].size + 1;
    pos += ec.cursor_x;

    /* Insert the copied text */
    size_t paste_len = strlen(ec.copied_char_buffer);
    if (gb_insert_with_undo(ec.gb, ec.undo_stack, pos, ec.copied_char_buffer,
                            paste_len)) {
        /* Update row directly */
        if (ec.cursor_y == ec.num_rows)
            insert_row(ec.num_rows, "", 0);
        editor_row_t *row = &ec.row[ec.cursor_y];
        row->chars = realloc(row->chars, row->size + paste_len + 1);
        memmove(&row->chars[ec.cursor_x + paste_len], &row->chars[ec.cursor_x],
                row->size - ec.cursor_x + 1);
        memcpy(&row->chars[ec.cursor_x], ec.copied_char_buffer, paste_len);
        row->size += paste_len;
        update_row(row);
        ec.cursor_x += paste_len;
        ec.modified = true;
    }
}

static void newline()
{
    if (!ec.gb)
        return;

    /* Calculate position in gap buffer */
    size_t pos = 0;
    for (int i = 0; i < ec.cursor_y && i < ec.num_rows; i++)
        pos += ec.row[i].size + 1; /* +1 for newline */
    pos += ec.cursor_x;

    /* Insert newline character into gap buffer */
    char nl = '\n';
    if (gb_insert_with_undo(ec.gb, ec.undo_stack, pos, &nl, 1)) {
        /* Update row structure directly */
        if (ec.cursor_x == 0) {
            insert_row(ec.cursor_y, "", 0);
        } else {
            editor_row_t *row = &ec.row[ec.cursor_y];
            insert_row(ec.cursor_y + 1, &row->chars[ec.cursor_x],
                       row->size - ec.cursor_x);
            row = &ec.row[ec.cursor_y];
            row->size = ec.cursor_x;
            row->chars[row->size] = '\0';
            update_row(row);
        }
        ec.cursor_y++;
        ec.cursor_x = 0;
        ec.modified = true;
    }
}

/* Sync gap buffer to rows for display */
static void gb_sync_to_rows(gap_buffer_t *gb)
{
    if (!gb)
        return;

    /* Save cursor position */
    int saved_cursor_y = ec.cursor_y;
    int saved_cursor_x = ec.cursor_x;

    /* Clear existing rows */
    for (int i = 0; i < ec.num_rows; i++) {
        free(ec.row[i].chars);
        free(ec.row[i].render);
        free(ec.row[i].highlight);
    }
    free(ec.row);
    ec.row = NULL;
    ec.num_rows = 0;

    /* Convert gap buffer to rows */
    size_t pos = 0;
    size_t len = gb_length(gb);

    while (pos < len) {
        size_t line_start = pos;
        size_t line_end = pos;

        /* Find end of line */
        while (line_end < len && gb_get_char(gb, line_end) != '\n')
            line_end++;

        /* Extract line */
        size_t line_len = line_end - line_start;
        char *line = malloc(line_len + 1);
        if (line) {
            for (size_t i = 0; i < line_len; i++)
                line[i] = gb_get_char(gb, line_start + i);
            line[line_len] = '\0';

            insert_row(ec.num_rows, line, line_len);
            free(line);
        }

        /* Move past newline */
        pos = line_end;
        if (pos < len && gb_get_char(gb, pos) == '\n')
            pos++;
    }

    /* Ensure at least one row */
    if (ec.num_rows == 0)
        insert_row(0, "", 0);

    /* Update modified flag from gap buffer */
    ec.modified = gb->modified;

    /* Restore cursor position within bounds */
    if (saved_cursor_y >= ec.num_rows) {
        ec.cursor_y = ec.num_rows - 1;
    } else {
        ec.cursor_y = saved_cursor_y;
    }

    if (ec.cursor_y >= 0 && ec.cursor_y < ec.num_rows) {
        if (saved_cursor_x > ec.row[ec.cursor_y].size) {
            ec.cursor_x = ec.row[ec.cursor_y].size;
        } else {
            ec.cursor_x = saved_cursor_x;
        }
    }
}

/* Buffer for accumulating UTF-8 bytes */
static struct {
    char bytes[4];
    int len;
    int expected;
} utf8_buffer = {.len = 0, .expected = 0};

static void insert_char(int c)
{
    if (!ec.gb)
        return;

    unsigned char byte = (unsigned char) c;

    /* Check if this is the start of a UTF-8 sequence */
    if (utf8_buffer.len == 0) {
        if (byte <= 0x7F) {
            /* ASCII - insert immediately */
            utf8_buffer.expected = 1;
        } else if ((byte & 0xE0) == 0xC0) {
            /* 2-byte UTF-8 */
            utf8_buffer.expected = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            /* 3-byte UTF-8 */
            utf8_buffer.expected = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            /* 4-byte UTF-8 */
            utf8_buffer.expected = 4;
        } else {
            /* Invalid UTF-8 start byte */
            return;
        }
    }

    /* Add byte to buffer */
    utf8_buffer.bytes[utf8_buffer.len++] = c;

    /* If we haven't collected all bytes yet, return */
    if (utf8_buffer.len < utf8_buffer.expected)
        return;

    /* We have a complete character, insert it */
    size_t pos = 0;
    for (int i = 0; i < ec.cursor_y && i < ec.num_rows; i++)
        pos += ec.row[i].size + 1;
    pos += ec.cursor_x;

    /* Insert the complete UTF-8 sequence as one undo operation */
    if (gb_insert_with_undo(ec.gb, ec.undo_stack, pos, utf8_buffer.bytes,
                            utf8_buffer.len)) {
        /* Update current row directly */
        if (ec.cursor_y == ec.num_rows)
            insert_row(ec.num_rows, "", 0);

        editor_row_t *row = &ec.row[ec.cursor_y];
        row->chars = realloc(row->chars, row->size + utf8_buffer.len + 1);
        memmove(&row->chars[ec.cursor_x + utf8_buffer.len],
                &row->chars[ec.cursor_x], row->size - ec.cursor_x + 1);
        memcpy(&row->chars[ec.cursor_x], utf8_buffer.bytes, utf8_buffer.len);
        row->size += utf8_buffer.len;
        update_row(row);
        ec.cursor_x += utf8_buffer.len;
        ec.modified = true;
    }

    /* Reset UTF-8 buffer */
    utf8_buffer.len = 0;
    utf8_buffer.expected = 0;
}

static void delete_char()
{
    if (!ec.gb)
        return;
    if (ec.cursor_y == ec.num_rows)
        return;
    if (ec.cursor_x == 0 && ec.cursor_y == 0)
        return;

    editor_row_t *row = &ec.row[ec.cursor_y];

    /* Calculate position in gap buffer */
    size_t pos = 0;
    for (int i = 0; i < ec.cursor_y && i < ec.num_rows; i++) {
        pos += ec.row[i].size + 1; /* +1 for newline */
    }

    if (ec.cursor_x > 0) {
        /* Delete character before cursor */
        const char *prev = utf8_prev_char(row->chars, row->chars + ec.cursor_x);
        int prev_pos = prev - row->chars;
        int char_len = ec.cursor_x - prev_pos;

        gb_delete_with_undo(ec.gb, ec.undo_stack, pos + prev_pos, char_len);

        /* Update row directly */
        memmove(&row->chars[prev_pos], &row->chars[ec.cursor_x],
                row->size - ec.cursor_x + 1);
        row->size -= char_len;
        update_row(row);
        ec.cursor_x = prev_pos;
        ec.modified = true;
    } else {
        /* Delete newline - join with previous line */
        if (ec.cursor_y > 0) {
            pos--; /* Point to previous line's newline */
            gb_delete_with_undo(ec.gb, ec.undo_stack, pos, 1);

            /* Join lines in row structure */
            ec.cursor_x = ec.row[ec.cursor_y - 1].size;
            editor_row_t *prev_row = &ec.row[ec.cursor_y - 1];
            prev_row->chars =
                realloc(prev_row->chars, prev_row->size + row->size + 1);
            memcpy(&prev_row->chars[prev_row->size], row->chars, row->size);
            prev_row->size += row->size;
            prev_row->chars[prev_row->size] = '\0';
            update_row(prev_row);

            /* Remove current row */
            free(row->render);
            free(row->chars);
            free(row->highlight);
            memmove(&ec.row[ec.cursor_y], &ec.row[ec.cursor_y + 1],
                    sizeof(editor_row_t) * (ec.num_rows - ec.cursor_y - 1));
            for (int j = ec.cursor_y; j < ec.num_rows - 1; j++)
                ec.row[j].idx--;
            ec.num_rows--;
            ec.cursor_y--;
            ec.modified = true;
        }
    }
}

static char *rows_tostring(int *buf_len)
{
    int total_len = 0;
    for (int j = 0; j < ec.num_rows; j++)
        total_len += ec.row[j].size + 1;
    *buf_len = total_len;
    char *buf = malloc(total_len);
    char *p = buf;
    for (int j = 0; j < ec.num_rows; j++) {
        memcpy(p, ec.row[j].chars, ec.row[j].size);
        p += ec.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

static void open_file(char *file_name)
{
    free(ec.file_name);
    ec.file_name = strdup(file_name);
    select_highlight();
    FILE *file = fopen(file_name, "r+");
    if (!file)
        panic("Failed to open the file");

    /* Load file into gap buffer if available */
    if (ec.gb) {
        /* Rewind file to beginning */
        fseek(file, 0, SEEK_SET);
        gb_load_file(ec.gb, file);

        /* Rewind for row loading */
        fseek(file, 0, SEEK_SET);
    }

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    while ((line_len = getline(&line, &line_cap, file)) != -1) {
        if (line_len > 0 &&
            (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
            line_len--;
        insert_row(ec.num_rows, line, line_len);
    }
    free(line);
    fclose(file);
    ec.modified = false;
}

static void save_file()
{
    if (!ec.file_name) {
        ec.file_name = prompt("Save as: %s (ESC to cancel)", NULL);
        if (!ec.file_name) {
            set_status_message("Save aborted");
            return;
        }
        select_highlight();
    }
    int len;
    char *buf = rows_tostring(&len);
    int fd = open(ec.file_name, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if ((ftruncate(fd, len) != -1) && (write(fd, buf, len) == len)) {
            close(fd);
            free(buf);
            ec.modified = false;
            if (len >= 1024)
                set_status_message("%d KiB written to disk", len >> 10);
            else
                set_status_message("%d B written to disk", len);
            return;
        }
        close(fd);
    }
    free(buf);
    set_status_message("Error: %s", strerror(errno));
}

/* Global variables for search state - needed for prompt display */
static int search_last_match = -1;
static int search_total_matches = 0;
static int search_current_match = 0;

static void search_cb(char *query, int key)
{
    static int direction = 1;
    static int saved_highlight_line;
    static char *saved_hightlight = NULL;

    if (saved_hightlight) {
        memcpy(ec.row[saved_highlight_line].highlight, saved_hightlight,
               ec.row[saved_highlight_line].render_size);
        free(saved_hightlight);
        saved_hightlight = NULL;
    }
    if (key == '\r' || key == '\x1b') {
        search_last_match = -1;
        direction = 1;
        search_total_matches = 0;
        search_current_match = 0;
        return;
    }
    if ((key == ARROW_RIGHT) || (key == ARROW_DOWN))
        direction = 1;
    else if ((key == ARROW_LEFT) || (key == ARROW_UP)) {
        if (search_last_match == -1)
            return;
        direction = -1;
    } else {
        search_last_match = -1;
        direction = 1;
        /* Count total matches when search term changes */
        search_total_matches = 0;
        search_current_match = 0;
        if (strlen(query) > 0) {
            for (int i = 0; i < ec.num_rows; i++) {
                char *p = ec.row[i].render;
                while ((p = strstr(p, query)) != NULL) {
                    search_total_matches++;
                    p++;
                }
            }
        }
    }
    int current = search_last_match;
    for (int i = 0; i < ec.num_rows; i++) {
        current += direction;
        if (current == -1)
            current = ec.num_rows - 1;
        else if (current == ec.num_rows)
            current = 0;
        editor_row_t *row = &ec.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            search_last_match = current;
            ec.cursor_y = current;
            ec.cursor_x = row_renderx_to_cursorx(row, match - row->render);
            ec.row_offset = ec.num_rows;
            saved_highlight_line = current;
            saved_hightlight = malloc(row->render_size);
            if (saved_hightlight)
                memcpy(saved_hightlight, row->highlight, row->render_size);
            memset(&row->highlight[match - row->render], MATCH, strlen(query));

            /* Update match counter */
            if (direction == 1)
                search_current_match =
                    (search_current_match % search_total_matches) + 1;
            else
                search_current_match = (search_current_match - 1) > 0
                                           ? search_current_match - 1
                                           : search_total_matches;
            break;
        }
    }
}

static void search()
{
    int saved_cursor_x = ec.cursor_x;
    int saved_cursor_y = ec.cursor_y;
    int saved_col_offset = ec.col_offset;
    int saved_row_offset = ec.row_offset;

    /* Reset search state for new search */
    search_last_match = -1;
    search_total_matches = 0;
    search_current_match = 0;

    char *query = prompt("Search", search_cb);
    if (query)
        free(query);
    else {
        ec.cursor_x = saved_cursor_x;
        ec.cursor_y = saved_cursor_y;
        ec.col_offset = saved_col_offset;
        ec.row_offset = saved_row_offset;
    }
}

static void buf_append(editor_buf_t *eb, const char *s, int len)
{
    char *new = realloc(eb->buf, eb->len + len);
    if (!new)
        return;
    memcpy(&new[eb->len], s, len);
    eb->buf = new;
    eb->len += len;
}

static void buf_free(editor_buf_t *eb)
{
    free(eb->buf);
}

static void scroll()
{
    ec.render_x = 0;
    if (ec.cursor_y < ec.num_rows)
        ec.render_x = row_cursorx_to_renderx(&ec.row[ec.cursor_y], ec.cursor_x);
    if (ec.cursor_y < ec.row_offset)
        ec.row_offset = ec.cursor_y;
    if (ec.cursor_y >= ec.row_offset + ec.screen_rows)
        ec.row_offset = ec.cursor_y - ec.screen_rows + 1;
    if (ec.render_x < ec.col_offset)
        ec.col_offset = ec.render_x;
    if (ec.render_x >= ec.col_offset + ec.screen_cols)
        ec.col_offset = ec.render_x - ec.screen_cols + 1;
}

static void draw_statusbar(editor_buf_t *eb)
{
    time_t now = time(NULL);
    struct tm currtime_buf;
    struct tm *currtime;
    buf_append(eb, "\x1b[100m", 6); /* Dark gray */
    char status[80], r_status[80];
    currtime = localtime_r(&now, &currtime_buf);
    int len = snprintf(status, sizeof(status), "  File: %.20s %s",
                       ec.file_name ? ec.file_name : "< New >",
                       ec.modified ? "(modified)" : "");
    int col_size = ec.row &&ec.cursor_y <= ec.num_rows - 1
                       ? col_size = ec.row[ec.cursor_y].size
                       : 0;
    int r_len = snprintf(
        r_status, sizeof(r_status), "%d/%d lines  %d/%d cols [ %2d:%2d:%2d ]",
        (ec.cursor_y + 1 > ec.num_rows) ? ec.num_rows : ec.cursor_y + 1,
        ec.num_rows, ec.cursor_x + 1, col_size, currtime->tm_hour,
        currtime->tm_min, currtime->tm_sec);
    if (len > ec.screen_cols)
        len = ec.screen_cols;
    buf_append(eb, status, len);
    while (len < ec.screen_cols) {
        if (ec.screen_cols - len == r_len) {
            buf_append(eb, r_status, r_len);
            break;
        }
        buf_append(eb, " ", 1);
        len++;
    }
    buf_append(eb, "\x1b[m", 3);
    buf_append(eb, "\r\n", 2);
}

static void draw_messagebar(editor_buf_t *eb)
{
    buf_append(eb, "\x1b[93m\x1b[44m\x1b[K", 13);
    int msg_len = strlen(ec.status_msg);

    /* For search messages, try to show as much as possible */
    if (strstr(ec.status_msg, "Search:")) {
        /* Always show search messages, even if long */
        if (msg_len > ec.screen_cols) {
            /* Truncate but try to keep the help text visible */
            buf_append(eb, ec.status_msg, ec.screen_cols);
        } else {
            buf_append(eb, ec.status_msg, msg_len);
        }
    } else {
        /* Regular messages: truncate and show for 5 seconds */
        if (msg_len > ec.screen_cols)
            msg_len = ec.screen_cols;
        if (msg_len && time(NULL) - ec.status_msg_time < 5)
            buf_append(eb, ec.status_msg, msg_len);
    }
    buf_append(eb, "\x1b[0m", 4);
}

static void set_status_message(const char *msg, ...)
{
    va_list args;
    va_start(args, msg);
    vsnprintf(ec.status_msg, sizeof(ec.status_msg), msg, args);
    va_end(args);
    ec.status_msg_time = time(NULL);
}

static void draw_rows(editor_buf_t *eb)
{
    for (int y = 0; y < ec.screen_rows; y++) {
        int file_row = y + ec.row_offset;
        if (file_row >= ec.num_rows) {
            buf_append(eb, "~", 1);
        } else {
            int len = ec.row[file_row].render_size - ec.col_offset;
            if (len < 0)
                len = 0;
            if (len > ec.screen_cols)
                len = ec.screen_cols;
            char *c = &ec.row[file_row].render[ec.col_offset];
            unsigned char *hl = &ec.row[file_row].highlight[ec.col_offset];
            int current_color = -1;
            for (int j = 0; j < len; j++) {
                if (iscntrl(c[j])) {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    buf_append(eb, "\x1b[7m", 4);
                    buf_append(eb, &sym, 1);
                    buf_append(eb, "\x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int c_len = snprintf(buf, sizeof(buf), "\x1b[%dm",
                                             current_color);
                        buf_append(eb, buf, c_len);
                    }
                } else if (hl[j] == NORMAL) {
                    if (current_color != -1) {
                        buf_append(eb, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    buf_append(eb, &c[j], 1);
                } else {
                    int color = token_to_color(hl[j]);
                    if (hl[j] == MATCH) {
                        /* Use inverse video for search matches */
                        buf_append(eb, "\x1b[7m", 4);
                        buf_append(eb, &c[j], 1);
                        buf_append(eb, "\x1b[27m", 5);
                        if (current_color != -1) {
                            char buf[16];
                            int c_len = snprintf(buf, sizeof(buf), "\x1b[%dm",
                                                 current_color);
                            buf_append(eb, buf, c_len);
                        }
                    } else {
                        if (color != current_color) {
                            current_color = color;
                            char buf[16];
                            int c_len =
                                snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                            buf_append(eb, buf, c_len);
                        }
                        buf_append(eb, &c[j], 1);
                    }
                }
            }
            buf_append(eb, "\x1b[39m", 5);
        }
        buf_append(eb, "\x1b[K", 3);
        buf_append(eb, "\r\n", 2);
    }
}

static void refresh_screen()
{
    scroll();
    editor_buf_t eb = {NULL, 0};
    buf_append(&eb, "\x1b[?25l", 6);
    buf_append(&eb, "\x1b[H", 3);
    draw_rows(&eb);
    draw_statusbar(&eb);
    draw_messagebar(&eb);
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (ec.cursor_y - ec.row_offset) + 1,
             (ec.render_x - ec.col_offset) + 1);
    buf_append(&eb, buf, strlen(buf));
    buf_append(&eb, "\x1b[?25h", 6);
    write(STDOUT_FILENO, eb.buf, eb.len);
    buf_free(&eb);
}

static void handle_sigwinch()
{
    update_window_size();
    if (ec.cursor_y > ec.screen_rows)
        ec.cursor_y = ec.screen_rows - 1;
    if (ec.cursor_x > ec.screen_cols)
        ec.cursor_x = ec.screen_cols - 1;
    refresh_screen();
}

static void handle_sigcont()
{
    disable_raw_mode();
    open_buffer();
    enable_raw_mode();
    refresh_screen();
}

static bool confirm_dialog(const char *msg)
{
    bool choice = false; /* false = No (default), true = Yes */

    while (1) {
        /* Build the message with highlighted options */
        char status_msg[256];
        if (!choice) {
            snprintf(status_msg, sizeof(status_msg),
                     "%s  \x1b[7m[ No ]\x1b[m   Yes   (ESC: cancel)", msg);
        } else {
            snprintf(status_msg, sizeof(status_msg),
                     "%s   No   \x1b[7m[ Yes ]\x1b[m  (ESC: cancel)", msg);
        }

        set_status_message("%s", status_msg);
        refresh_screen();

        int c = read_key();
        switch (c) {
        case '\r': /* Enter key */
            set_status_message("");
            return choice;
        case '\x1b': /* ESC key */
        case CTRL_('q'):
            set_status_message("");
            return false; /* Cancel = No */
        case ARROW_LEFT:
        case ARROW_RIGHT:
            choice = !choice; /* Toggle between Yes and No */
            break;
        case 'y':
        case 'Y':
            choice = true; /* Quick key for Yes */
            break;
        case 'n':
        case 'N':
            choice = false; /* Quick key for No */
            break;
        }
    }
}

static char *prompt(const char *msg, void (*callback)(char *, int))
{
    size_t buf_size = 128;
    char *buf = malloc(buf_size);
    if (!buf)
        return NULL;
    size_t buf_len = 0;
    buf[0] = '\0';

    /* Check if this is a search prompt */
    bool is_search = (callback == search_cb);

    while (1) {
        /* Special formatting for search prompt */
        if (is_search) {
            /* Build the complete search message with help text */
            char display_msg[512];

            /* Format: "Search: [query] [match info] (help text)" */
            if (search_total_matches > 0 && buf_len > 0) {
                snprintf(display_msg, sizeof(display_msg),
                         "Search: %s [%d/%d] (arrows: navigate, Enter: exit, "
                         "ESC: cancel)",
                         buf, search_current_match, search_total_matches);
            } else {
                snprintf(
                    display_msg, sizeof(display_msg),
                    "Search: %s (arrows: navigate, Enter: exit, ESC: cancel)",
                    buf);
            }

            set_status_message("%s", display_msg);
        } else {
            /* For non-search prompts, use the format string */
            char formatted_msg[256];
            snprintf(formatted_msg, sizeof(formatted_msg), msg, buf);
            set_status_message("%s", formatted_msg);
        }
        refresh_screen();
        int c = read_key();
        if ((c == DEL_KEY) || (c == CTRL_('h')) || (c == BACKSPACE)) {
            if (buf_len != 0)
                buf[--buf_len] = '\0';
        } else if (c == '\x1b') {
            set_status_message("");
            if (callback)
                callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buf_len != 0) {
                set_status_message("");
                if (callback)
                    callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && isprint(c)) {
            if (buf_len == buf_size - 1) {
                buf_size *= 2;
                /* In case realloc fails */
                char *new_buf = realloc(buf, buf_size);
                if (NULL == new_buf) {
                    free(buf);
                    return NULL;
                }
                buf = new_buf;
            }
            buf[buf_len++] = c;
            buf[buf_len] = '\0';
        }
        if (callback)
            callback(buf, c);
    }
}

static void move_cursor(int key)
{
    editor_row_t *row =
        (ec.cursor_y >= ec.num_rows) ? NULL : &ec.row[ec.cursor_y];
    switch (key) {
    case ARROW_LEFT:
        if (ec.cursor_x != 0) {
            /* Move to previous UTF-8 character boundary */
            if (row) {
                const char *prev =
                    utf8_prev_char(row->chars, row->chars + ec.cursor_x);
                ec.cursor_x = prev - row->chars;
            } else {
                ec.cursor_x--;
            }
        } else if (ec.cursor_y > 0) {
            ec.cursor_y--;
            ec.cursor_x = ec.row[ec.cursor_y].size;
        }
        break;
    case ARROW_RIGHT:
        if (row && ec.cursor_x < row->size) {
            /* Move to next UTF-8 character boundary */
            const char *next = utf8_next_char(row->chars + ec.cursor_x);
            ec.cursor_x = next - row->chars;
            if (ec.cursor_x > row->size)
                ec.cursor_x = row->size;
        } else if (row && ec.cursor_x == row->size) {
            ec.cursor_y++;
            ec.cursor_x = 0;
        }
        break;
    case ARROW_UP:
        if (ec.cursor_y != 0)
            ec.cursor_y--;
        break;
    case ARROW_DOWN:
        if (ec.cursor_y < ec.num_rows)
            ec.cursor_y++;
        break;
    }
    row = (ec.cursor_y >= ec.num_rows) ? NULL : &ec.row[ec.cursor_y];
    int row_len = row ? row->size : 0;
    if (ec.cursor_x > row_len)
        ec.cursor_x = row_len;
}

static void process_key()
{
    static int indent_level = 0;
    int c = read_key();
    switch (c) {
    case '\r':
        newline();
        for (int i = 0; i < indent_level; i++)
            insert_char('\t');
        break;
    case CTRL_('q'):
        if (ec.modified) {
            if (!confirm_dialog("File has been modified. Quit without saving?"))
                return;
        }
        clear_screen();
        close_buffer();
        exit(0);
        break;
    case CTRL_('s'):
        save_file();
        break;
    case CTRL_('x'):
        if (ec.cursor_y < ec.num_rows)
            cut();
        break;
    case CTRL_('c'):
        if (ec.cursor_y < ec.num_rows)
            copy(0);
        break;
    case CTRL_('v'):
        paste();
        break;
    case CTRL_('z'):
        /* Undo last operation */
        if (ec.gb && ec.undo_stack) {
            if (undo_perform(ec.gb, ec.undo_stack)) {
                ec.modified = ec.gb->modified;
                set_status_message("Undo performed");
            } else {
                set_status_message("Nothing to undo");
            }
        } else {
            set_status_message("Undo system not initialized");
        }
        break;
    case CTRL_('r'):
        /* Redo last undone operation */
        if (ec.gb && ec.undo_stack) {
            if (undo_redo(ec.gb, ec.undo_stack)) {
                ec.modified = ec.gb->modified;
                set_status_message("Redo performed");
            } else {
                set_status_message("Nothing to redo");
            }
        } else {
            set_status_message("Undo system not initialized");
        }
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        move_cursor(c);
        break;
    case PAGE_UP:
    case PAGE_DOWN: {
        if (c == PAGE_UP)
            ec.cursor_y = ec.row_offset;
        else if (c == PAGE_DOWN)
            ec.cursor_y = ec.row_offset + ec.screen_rows - 1;
        int times = ec.screen_rows;
        while (times--)
            move_cursor((c == PAGE_UP) ? ARROW_UP : ARROW_DOWN);
    } break;
    case HOME_KEY:
        ec.cursor_x = 0;
        break;
    case END_KEY:
        if (ec.cursor_y < ec.num_rows)
            ec.cursor_x = ec.row[ec.cursor_y].size;
        break;
    case CTRL_('f'):
        search();
        break;
    case BACKSPACE:
    case CTRL_('h'):
    case DEL_KEY:
        if (c == DEL_KEY)
            move_cursor(ARROW_RIGHT);
        delete_char();
        break;
    case CTRL_('l'):
    case '\x1b':
        break;
    case '{':
        insert_char(c);
        indent_level++;
        break;
    case '}':
        if (ec.cursor_y == ec.num_rows)
            goto none;
        if ((ec.cursor_x == 0) && (ec.cursor_y == 0))
            goto none;
        editor_row_t *row = &ec.row[ec.cursor_y];
        if ((ec.cursor_x > 0) && (row->chars[ec.cursor_x - 1] == '\t'))
            delete_char();
    none:
        insert_char(c);
        indent_level--;
        break;
    default:
        insert_char(c);
    }
}

static void init_editor()
{
    update_window_size();
    signal(SIGWINCH, handle_sigwinch);
    signal(SIGCONT, handle_sigcont);

    /* Initialize gap buffer and undo/redo - always enabled */
    ec.gb = gb_init(GB_INITIAL_SIZE);
    if (ec.gb)
        ec.undo_stack = undo_init(MAX_UNDO_LEVELS);
}

int main(int argc, char *argv[])
{
    init_editor();
    if (argc >= 2)
        open_file(argv[1]);
    enable_raw_mode();
    set_status_message(
        "Mazu Editor | ^Q Exit | ^S Save | ^F Search | "
        "^Z Undo | ^R Redo | ^C Copy | ^X Cut | ^V Paste");
    refresh_screen();
    while (1) {
        process_key();
        refresh_screen();
    }
    /* not reachable */
    return 0;
}
