/* Mazu Editor:
 * A minimalist editor with syntax highlight, copy/paste, undo, and search.
 */

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE /* Enable SIGWINCH on macOS */
#endif

/* Timer feature: Set to 1 to enable automatic clock updates, 0 to disable */
#ifndef ENABLE_TIMER
#define ENABLE_TIMER 1
#endif

/* Standard library headers */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* System headers */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Timer-specific headers */
#if ENABLE_TIMER
#include <poll.h>
#include <sys/time.h>
#include <time.h>
#else
/* time() is still needed for status messages even without timer */
#include <time.h>
#endif

#define CTRL_(k) ((k) & (0x1f))
#define TAB_STOP 4

/* UTF-8 handling functions */

/* Get the byte length of a UTF-8 character from its first byte */
static inline int utf8_byte_length(uint8_t c)
{
    if (!(c & 0x80))
        return 1; /* ASCII */
    if ((c & 0xE0) == 0xC0 && c >= 0xC2)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0 && c <= 0xF4)
        return 4;
    return 1; /* Invalid UTF-8, treat as single byte */
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
    return *s ? s + utf8_byte_length((uint8_t) *s) : s;
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
static void ui_set_message(const char *msg, ...);

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

#define GAP_INITIAL_SIZE 65536 /* 64 KiB initial buffer */
#define GAP_GROW_SIZE 4096     /* 4 KiB growth increment */

/* Initialize a gap buffer with given size */
static gap_buffer_t *gap_init(size_t initial_size)
{
    gap_buffer_t *gb = malloc(sizeof(gap_buffer_t));
    if (!gb)
        return NULL;

    if (!(gb->buffer = malloc(initial_size))) {
        free(gb);
        return NULL;
    }

    gb->size = initial_size;
    gb->gap = gb->buffer;
    gb->egap = gb->ebuffer = gb->buffer + initial_size;
    gb->modified = false;
    return gb;
}

/* Destroy gap buffer and free memory */
static void gap_destroy(gap_buffer_t *gb)
{
    if (gb) {
        free(gb->buffer);
        free(gb);
    }
}

/* Get total text length (excluding gap) */
static inline size_t gap_length(const gap_buffer_t *gb)
{
    return (gb->gap - gb->buffer) + (gb->ebuffer - gb->egap);
}

/* Convert file position to buffer pointer */
static char *gap_ptr(gap_buffer_t *gb, size_t pos)
{
    size_t front_size = gb->gap - gb->buffer;

    if (pos <= front_size)
        return gb->buffer + pos;
    return gb->egap + (pos - front_size);
}

/* Move gap to position */
static void gap_move(gap_buffer_t *gb, size_t pos)
{
    char *dest = gap_ptr(gb, pos);

    if (dest < gb->gap) {
        /* Move gap backward - shift text forward */
        size_t len = gb->gap - dest;
        gb->egap -= len;
        gb->gap -= len;
        memmove(gb->egap, gb->gap, len);
    } else if (dest > gb->egap) {
        /* Move gap forward - shift text backward */
        size_t len = dest - gb->egap;
        memmove(gb->gap, gb->egap, len);
        gb->gap += len;
        gb->egap += len;
    }
}

/* Grow the gap to ensure minimum size */
static bool gap_grow(gap_buffer_t *gb, size_t min_gap)
{
    size_t gap_size = gb->egap - gb->gap;

    if (gap_size >= min_gap)
        return true; /* Already large enough */

    /* Calculate new size */
    size_t text_size = gap_length(gb);
    size_t new_size = text_size + min_gap + GAP_GROW_SIZE;

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
static bool gap_insert(gap_buffer_t *gb,
                       size_t pos,
                       const char *text,
                       size_t len)
{
    /* Move gap to insertion point */
    gap_move(gb, pos);

    /* Ensure gap is large enough */
    if (!gap_grow(gb, len))
        return false; /* Failed to grow */

    /* Copy text into gap */
    memcpy(gb->gap, text, len);
    gb->gap += len;
    gb->modified = true;

    return true;
}

/* Delete text from position */
static void gap_delete(gap_buffer_t *gb, size_t pos, size_t len)
{
    /* Move gap to deletion point */
    gap_move(gb, pos);

    /* Extend gap to cover deleted text */
    size_t available = gb->ebuffer - gb->egap;
    if (len > available)
        len = available; /* Can't delete more than exists */

    gb->egap += len;
    gb->modified = true;
}

/* Get character at position */
static int gap_get_char(gap_buffer_t *gb, size_t pos)
{
    return pos >= gap_length(gb) ? -1 : (unsigned char) *gap_ptr(gb, pos);
}

/* Load file into gap buffer */
static bool gap_load_file(gap_buffer_t *gb, FILE *fp)
{
    /* Clear existing content */
    gb->gap = gb->buffer;
    gb->egap = gb->ebuffer;

    char buf[4096];
    size_t nread;

    while ((nread = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (!gap_insert(gb, gap_length(gb), buf, nread))
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

    stack->head = stack->tail = NULL;
    stack->current = NULL;
    stack->max_undos = max_levels;
    stack->count = 0;

    return stack;
}

/* Destroy undo stack and free all memory */
static void undo_destroy(undo_stack_t *stack)
{
    if (!stack)
        return;

    /* Free all undo nodes */
    undo_node_t *node = stack->head;
    while (node) {
        undo_node_t *next = node->next;
        free(node->text);
        free(node);
        node = next;
    }

    free(stack);
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
    if ((stack->tail = stack->current))
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

    if (stack->current)
        stack->current->next = node;
    else
        stack->head = node;

    stack->tail = stack->current = node;
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
static void gap_sync_to_rows(gap_buffer_t *gb);

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
        gap_delete(gb, node->pos, node->len);
        break;

    case UNDO_DELETE:
        /* Was a delete, so insert it back */
        gap_insert(gb, node->pos, node->text, node->len);
        break;

    case UNDO_REPLACE:
        /* For replace, we need the old text (stored in next node) */
        /* For now, treat as delete + insert */
        gap_delete(gb, node->pos, node->len);
        if (node->prev && node->prev->type == UNDO_DELETE)
            gap_insert(gb, node->pos, node->prev->text, node->prev->len);
        break;
    }

    /* Move current pointer back */
    stack->current = node->prev;

    /* Sync gap buffer back to rows for display */
    gap_sync_to_rows(gb);

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
    undo_node_t *node = stack->current ? stack->current->next : stack->head;

    if (!node)
        return false; /* Nothing to redo */

    /* Re-apply the operation */
    switch (node->type) {
    case UNDO_INSERT:
        /* Re-insert the text */
        gap_insert(gb, node->pos, node->text, node->len);
        break;

    case UNDO_DELETE:
        /* Re-delete the text */
        gap_delete(gb, node->pos, node->len);
        break;

    case UNDO_REPLACE:
        /* Re-replace the text */
        gap_delete(gb, node->pos, node->len);
        gap_insert(gb, node->pos, node->text, node->len);
        break;
    }

    /* Move current pointer forward */
    stack->current = node;

    /* Sync gap buffer back to rows for display */
    gap_sync_to_rows(gb);

    return true;
}

/* Track insertion for undo (wrapper for gap_insert) */
static bool gap_insert_with_undo(gap_buffer_t *gb,
                                 undo_stack_t *undo,
                                 size_t pos,
                                 const char *text,
                                 size_t len)
{
    if (!gap_insert(gb, pos, text, len))
        return false;

    if (undo)
        undo_push(undo, UNDO_INSERT, pos, text, len);

    return true;
}

/* Track deletion for undo (wrapper for gap_delete) */
static void gap_delete_with_undo(gap_buffer_t *gb,
                                 undo_stack_t *undo,
                                 size_t pos,
                                 size_t len)
{
    if (undo && len > 0 && pos < gap_length(gb)) {
        /* Save the text being deleted */
        char *text = malloc(len + 1);
        if (text) {
            size_t i;
            for (i = 0; i < len && pos + i < gap_length(gb); i++) {
                int ch = gap_get_char(gb, pos + i);
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

    gap_delete(gb, pos, len);
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

/* X-macro for editor modes */
#define EDITOR_MODES                                  \
    _(NORMAL, "NORMAL", "Default editing mode")       \
    _(SEARCH, "SEARCH", "Search mode (Ctrl-F)")       \
    _(PROMPT, "PROMPT", "Generic prompt mode")        \
    _(SELECT, "SELECT", "Text selection mode")        \
    _(CONFIRM, "CONFIRM", "Confirmation dialog mode") \
    _(HELP, "HELP", "Help screen mode")               \
    _(BROWSER, "BROWSER", "File browser mode")

/* X-macro for key bindings - centralizes all shortcuts */
#define KEY_BINDINGS                    \
    _(QUIT, 'q', "Exit editor")         \
    _(SAVE, 's', "Save file")           \
    _(FIND, 'f', "Search text")         \
    _(OPEN, 'o', "Open file browser")   \
    _(MARK, 'x', "Start marking text")  \
    _(COPY, 'c', "Copy marked text")    \
    _(CUT, 'k', "Cut line/marked text") \
    _(PASTE, 'v', "Paste/uncut")        \
    _(UNDO, 'z', "Undo last action")    \
    _(REDO, 'r', "Redo last undo")      \
    _(HELP, '?', "Show help")

/* clang-format off */
typedef enum {
#define _(mode, name, desc) MODE_##mode,
    EDITOR_MODES
#undef _
    MODE_COUNT /* Total number of modes */
} editor_mode_t;
/* clang-format on */

/* Text selection state */
typedef struct {
    int start_x, start_y; /* Selection start position */
    int end_x, end_y;     /* Selection end position */
    bool active;          /* Is selection active? */
} selection_state_t;

/* Mode-specific state data */
typedef union {
    struct {
        char *query;
        int last_match;
        int direction;
    } search;
    struct {
        char *buffer;
    } prompt;
    struct {
        char **entries;    /* Array of file/dir names */
        int num_entries;   /* Number of entries */
        int selected;      /* Currently selected entry */
        int offset;        /* Scroll offset */
        char *current_dir; /* Current directory path */
        bool show_hidden;  /* Show hidden files (toggle with H) */
    } browser;
} mode_data_t;

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
    /* Editor mode state machine */
    editor_mode_t mode;
    editor_mode_t prev_mode;     /* For returning from temporary modes */
    mode_data_t mode_state;      /* Mode-specific state data */
    selection_state_t selection; /* Text selection state */
    bool show_line_numbers;      /* Toggle line numbers display */
#if ENABLE_TIMER
    /* Timer support for time update */
    time_t last_update_time;
#endif
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
    .status_msg = "",
    .status_msg_time = 0,
    .copied_char_buffer = NULL,
    .syntax = NULL,
    .gb = NULL,
    .undo_stack = NULL,
    .mode = MODE_NORMAL,
    .prev_mode = MODE_NORMAL,
    .mode_state = {{0}},
    .selection =
        {
            .start_x = 0,
            .start_y = 0,
            .end_x = 0,
            .end_y = 0,
            .active = false,
        },
    .show_line_numbers = false,
#if ENABLE_TIMER
    .last_update_time = 0,
#endif
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

/* X-macro for syntax highlighting types */
#define HIGHLIGHT_TYPES                         \
    _(NORMAL, 97, "Default text")               \
    _(MATCH, 43, "Search match")                \
    _(SL_COMMENT, 36, "Single-line comment")    \
    _(ML_COMMENT, 36, "Multi-line comment")     \
    _(KEYWORD_1, 93, "Primary keyword")         \
    _(KEYWORD_2, 92, "Secondary keyword")       \
    _(KEYWORD_3, 36, "Preprocessor")            \
    _(STRING, 91, "String literal")             \
    _(NUMBER, 31, "Numeric literal")

/* Generate highlight enum using X-macro */
typedef enum {
#define _(type, color, desc) type,
    HIGHLIGHT_TYPES
#undef _
    HIGHLIGHT_COUNT
} highlight_type_t;
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

static char *ui_prompt(const char *msg, void (*callback)(char *, int));

/* Mode management implementation */
static void mode_set(editor_mode_t new_mode)
{
    /* Save current mode as previous (unless entering temporary mode) */
    if (ec.mode != MODE_PROMPT && ec.mode != MODE_CONFIRM &&
        ec.mode != MODE_HELP)
        ec.prev_mode = ec.mode;

    /* Clean up old mode state */
    switch (ec.mode) {
    case MODE_SEARCH:
        free(ec.mode_state.search.query);
        ec.mode_state.search.query = NULL;
        break;
    case MODE_PROMPT:
        free(ec.mode_state.prompt.buffer);
        ec.mode_state.prompt.buffer = NULL;
        break;
    default:
        break;
    }

    /* Initialize new mode */
    ec.mode = new_mode;
    memset(&ec.mode_state, 0, sizeof(ec.mode_state));

    /* Mode-specific initialization */
    switch (new_mode) {
    case MODE_SELECT:
        /* Ensure cursor is in valid position before starting selection */
        if (ec.cursor_y >= ec.num_rows && ec.num_rows > 0) {
            ec.cursor_y = ec.num_rows - 1;
            ec.cursor_x = ec.row[ec.cursor_y].size;
        }
        ec.selection.start_x = ec.cursor_x;
        ec.selection.start_y = ec.cursor_y;
        ec.selection.end_x = ec.cursor_x;
        ec.selection.end_y = ec.cursor_y;
        ec.selection.active = true;
        ui_set_message("-- SELECT MODE -- Use arrows to extend, ESC to cancel");
        break;
    case MODE_SEARCH:
        ec.mode_state.search.direction = 1;
        ec.mode_state.search.last_match = -1;
        break;
    case MODE_HELP:
        ui_set_message("-- HELP -- Press any key to exit");
        break;
    case MODE_NORMAL:
        ec.selection.active = false;
        ui_set_message("");
        break;
    default:
        break;
    }
}

static void mode_restore(void)
{
    mode_set(ec.prev_mode);
}

static const char *mode_get_name(editor_mode_t mode)
{
    if (mode >= 0 && mode < MODE_COUNT) {
        /* Generate mode names using X-macro */
        static const char *mode_names[] = {
#define _(mode, name, desc) [MODE_##mode] = name,
            EDITOR_MODES
#undef _
        };
        return mode_names[mode];
    }
    return "UNKNOWN";
}

static void help_generate(char *buffer, size_t size)
{
    /* Generate help text from key bindings X-macro */
    int offset = snprintf(buffer, size, "Key Bindings:\n");

#define _(action, key, desc) \
    offset +=                \
        snprintf(buffer + offset, size - offset, "  ^%c - %s\n", key, desc);
    KEY_BINDINGS
#undef _

    offset += snprintf(buffer + offset, size - offset, "\nEditor Modes:\n");

#define _(mode, name, desc) \
    offset +=               \
        snprintf(buffer + offset, size - offset, "  %s - %s\n", name, desc);
    EDITOR_MODES
#undef _
}

static void term_clear(void)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
}

static void panic(const char *s)
{
    term_clear();
    perror(s);
    puts("\r\n");
    exit(1);
}

static void term_open_buffer(void)
{
    if (write(STDOUT_FILENO, "\x1b[?47h", 6) == -1)
        panic("Error changing terminal buffer");
}

static void term_disable_raw(void)
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ec.orig_termios) == -1)
        panic("Failed to disable raw mode");
}

static void term_enable_raw(void)
{
    if (tcgetattr(STDIN_FILENO, &ec.orig_termios) == -1)
        panic("Failed to get current terminal state");
    atexit(term_disable_raw);
    struct termios raw = ec.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    term_open_buffer();
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        panic("Failed to set raw mode");
}

static int term_read_key(void)
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

static int term_get_size(int *screen_rows, int *screen_cols)
{
    struct winsize ws;
    if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) || (ws.ws_col == 0))
        return -1;
    *screen_cols = ws.ws_col;
    *screen_rows = ws.ws_row;
    return 0;
}

static void term_update_size(void)
{
    if (term_get_size(&ec.screen_rows, &ec.screen_cols) == -1) {
        /* Fallback to reasonable defaults for testing */
        ec.screen_rows = 24, ec.screen_cols = 80;
    }
    ec.screen_rows -= 2;
}

static void term_close_buffer(void)
{
    if (write(STDOUT_FILENO, "\x1b[?9l", 5) == -1 ||
        write(STDOUT_FILENO, "\x1b[?47l", 6) == -1)
        panic("Error restoring buffer state");
    term_clear();
}

static bool syntax_is_separator(int c)
{
    return isspace(c) || !c || strchr(",.()+-/*=~%<>[]:;", c);
}

static bool syntax_is_number_part(int c)
{
    return c == '.' || c == 'x' || c == 'a' || c == 'b' || c == 'c' ||
           c == 'd' || c == 'e' || c == 'f' || c == 'A' || c == 'X' ||
           c == 'B' || c == 'C' || c == 'D' || c == 'E' || c == 'F' ||
           c == 'h' || c == 'H';
}

static void syntax_highlight(editor_row_t *row)
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
                (syntax_is_number_part(c) && (prev_highlight == NUMBER))) {
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
                    syntax_is_separator(row->render[i + kw_len])) {
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
        prev_sep = syntax_is_separator(c);
        i++;
    }
    bool changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < ec.num_rows)
        syntax_highlight(&ec.row[row->idx + 1]);
}

/* Reference: https://misc.flogisoft.com/bash/tip_colors_and_formatting */
static int syntax_token_color(int highlight)
{
    /* Generate color mapping using X-macro */
    static const int highlight_colors[] = {
#define _(type, color, desc) [type] = color,
        HIGHLIGHT_TYPES
#undef _
    };

    if (highlight >= 0 && highlight < HIGHLIGHT_COUNT)
        return highlight_colors[highlight];
    return 97; /* Default white */
}

static void syntax_select(void)
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
                    syntax_highlight(&ec.row[file_row]);
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

static void row_update(editor_row_t *row)
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
    syntax_highlight(row);
}

static void row_insert(int at, const char *s, size_t line_len)
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
    row_update(&ec.row[at]);
    ec.num_rows++;
    ec.modified = true;
}

static void editor_copy(int cut)
{
    if (!ec.gb || ec.cursor_y >= ec.num_rows)
        return;

    size_t len = strlen(ec.row[ec.cursor_y].chars) + 1;
    ec.copied_char_buffer = realloc(ec.copied_char_buffer, len);
    if (!ec.copied_char_buffer) {
        ui_set_message("Memory allocation failed");
        return;
    }
    snprintf(ec.copied_char_buffer, len, "%s", ec.row[ec.cursor_y].chars);
    ui_set_message(cut ? "Text cut" : "Text copied");
}

static void editor_cut(void)
{
    if (!ec.gb || ec.cursor_y >= ec.num_rows)
        return;

    editor_copy(-1);

    /* Calculate line position in gap buffer */
    size_t line_start = 0;
    for (int i = 0; i < ec.cursor_y; i++)
        line_start += ec.row[i].size + 1;

    /* Delete entire line including newline */
    size_t line_len = ec.row[ec.cursor_y].size;
    if (ec.cursor_y < ec.num_rows - 1)
        line_len++; /* Include newline */

    gap_delete_with_undo(ec.gb, ec.undo_stack, line_start, line_len);

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
        row_update(&ec.row[0]);
    }

    /* Adjust cursor */
    if (ec.cursor_y >= ec.num_rows && ec.num_rows > 0)
        ec.cursor_y = ec.num_rows - 1;
    ec.cursor_x = 0;
    ec.modified = true;
}

static void editor_paste(void)
{
    if (!ec.copied_char_buffer || !ec.gb)
        return;

    /* Validate cursor position first */
    if (ec.cursor_y >= ec.num_rows) {
        if (ec.num_rows > 0) {
            ec.cursor_y = ec.num_rows - 1;
            ec.cursor_x = ec.row[ec.cursor_y].size;
        } else {
            ec.cursor_y = 0;
            ec.cursor_x = 0;
        }
    }

    /* Ensure cursor_x doesn't exceed current row size */
    if (ec.cursor_y < ec.num_rows) {
        int max_x = ec.row[ec.cursor_y].size;
        if (ec.cursor_x > max_x)
            ec.cursor_x = max_x;
    }

    /* Save validated cursor position for later */
    int paste_start_x = ec.cursor_x, paste_start_y = ec.cursor_y;

    /* Calculate position in gap buffer */
    size_t pos = 0;
    for (int i = 0; i < ec.cursor_y && i < ec.num_rows; i++)
        pos += ec.row[i].size + 1;
    pos += ec.cursor_x;

    /* Insert the copied text */
    size_t paste_len = strlen(ec.copied_char_buffer);

    if (gap_insert_with_undo(ec.gb, ec.undo_stack, pos, ec.copied_char_buffer,
                             paste_len)) {
        /* Sync gap buffer to rows first */
        gap_sync_to_rows(ec.gb);

        /* Now calculate correct cursor position based on what we pasted */
        bool has_newlines = (strchr(ec.copied_char_buffer, '\n') != NULL);

        if (has_newlines) {
            /* For multi-line paste, find where we end up */
            int lines_in_paste = 0;
            int last_line_len = 0;
            int chars_on_first_line = paste_start_x;

            for (size_t i = 0; i < paste_len; i++) {
                if (ec.copied_char_buffer[i] == '\n') {
                    lines_in_paste++;
                    last_line_len = 0;
                } else {
                    if (lines_in_paste == 0) {
                        chars_on_first_line++;
                    } else {
                        last_line_len++;
                    }
                }
            }

            /* Position cursor at the end of pasted content */
            ec.cursor_y = paste_start_y + lines_in_paste;
            if (ec.cursor_y >= ec.num_rows)
                ec.cursor_y = ec.num_rows - 1;

            if (lines_in_paste == 0) {
                /* No newlines actually found, stay on same line */
                ec.cursor_x = chars_on_first_line;
            } else {
                /* Had newlines, cursor is at position on last line */
                ec.cursor_x = last_line_len;
            }

            /* Ensure cursor_x is valid */
            if (ec.cursor_y < ec.num_rows &&
                ec.cursor_x > ec.row[ec.cursor_y].size)
                ec.cursor_x = ec.row[ec.cursor_y].size;
        } else {
            /* Single-line paste - advance cursor on same line */
            ec.cursor_x = paste_start_x + paste_len;
            if (ec.cursor_y < ec.num_rows &&
                ec.cursor_x > ec.row[ec.cursor_y].size)
                ec.cursor_x = ec.row[ec.cursor_y].size;
        }

        ec.modified = true;
        ui_set_message("Pasted %zu bytes", paste_len);
    }
}

/* Check if a position is within the selection */
static bool selection_contains(int x, int y)
{
    if (!ec.selection.active)
        return false;

    int start_y = ec.selection.start_y, start_x = ec.selection.start_x;
    int end_y = ec.selection.end_y, end_x = ec.selection.end_x;

    /* Normalize selection */
    if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
        int tmp_y = start_y;
        start_y = end_y;
        end_y = tmp_y;
        int tmp_x = start_x;
        start_x = end_x;
        end_x = tmp_x;
    }

    if (y < start_y || y > end_y)
        return false;

    if (y == start_y && y == end_y) {
        /* Single line selection */
        return x >= start_x && x < end_x;
    } else if (y == start_y) {
        /* First line of multi-line selection */
        return x >= start_x;
    } else if (y == end_y) {
        /* Last line of multi-line selection */
        return x < end_x;
    } else {
        /* Middle lines of multi-line selection */
        return true;
    }
}

/* Get the selected text as a string */
static char *selection_get_text(void)
{
    if (!ec.selection.active)
        return NULL;

    int start_y = ec.selection.start_y;
    int start_x = ec.selection.start_x;
    int end_y = ec.selection.end_y;
    int end_x = ec.selection.end_x;

    /* Ensure selection is within valid rows */
    if (start_y >= ec.num_rows || end_y >= ec.num_rows)
        return NULL;

    /* Normalize selection (ensure start comes before end) */
    if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
        int tmp_y = start_y;
        start_y = end_y;
        end_y = tmp_y;
        int tmp_x = start_x;
        start_x = end_x;
        end_x = tmp_x;
    }

    /* Calculate total size needed */
    size_t total_size = 0;
    if (start_y == end_y) {
        /* Single line selection */
        if (start_y < ec.num_rows) {
            editor_row_t *row = &ec.row[start_y];
            if (row->chars) { /* Safety check */
                int actual_start = start_x > row->size ? row->size : start_x;
                int actual_end = end_x > row->size ? row->size : end_x;
                if (actual_end > actual_start)
                    total_size = actual_end - actual_start;
            }
        }
    } else {
        /* Multi-line selection - properly count newlines */
        for (int y = start_y; y <= end_y && y < ec.num_rows; y++) {
            editor_row_t *row = &ec.row[y];
            if (!row->chars) {
                /* Empty row, just count newline if not last line */
                if (y < end_y)
                    total_size++;
                continue;
            }

            if (y == start_y) {
                /* First line: from start_x to end of line */
                int actual_start = start_x > row->size ? row->size : start_x;
                int len = row->size - actual_start;
                if (len > 0)
                    total_size += len;
                if (y < end_y)
                    total_size++; /* Add newline if not last line */
            } else if (y == end_y) {
                /* Last line: from beginning to end_x */
                int actual_end = end_x > row->size ? row->size : end_x;
                total_size += actual_end;
            } else {
                /* Middle lines: entire line */
                total_size += row->size;
                total_size++; /* Add newline */
            }
        }
    }

    if (total_size == 0)
        return NULL;

    char *buffer = malloc(total_size + 1);
    if (!buffer)
        return NULL;

    /* Copy selected text */
    char *p = buffer;
    if (start_y == end_y) {
        /* Single line */
        if (start_y < ec.num_rows) {
            editor_row_t *row = &ec.row[start_y];
            if (row->chars) { /* Safety check */
                /* Clamp positions to actual row size */
                int actual_start = start_x > row->size ? row->size : start_x;
                int actual_end = end_x > row->size ? row->size : end_x;
                int len = actual_end - actual_start;
                if (len > 0) {
                    memcpy(p, &row->chars[actual_start], len);
                    p += len;
                }
            }
        }
    } else {
        /* Multi-line - properly include newlines */
        for (int y = start_y; y <= end_y && y < ec.num_rows; y++) {
            editor_row_t *row = &ec.row[y];
            if (!row->chars)
                continue; /* Safety check */

            if (y == start_y) {
                /* First line: from start_x to end of line */
                int actual_start = start_x > row->size ? row->size : start_x;
                int len = row->size - actual_start;
                if (len > 0) {
                    memcpy(p, &row->chars[actual_start], len);
                    p += len;
                }
                /* Always add newline after first line if not the last line */
                if (y < end_y) {
                    *p++ = '\n';
                }
            } else if (y == end_y) {
                /* Last line: from beginning to end_x */
                int actual_end = end_x > row->size ? row->size : end_x;
                if (actual_end > 0) {
                    memcpy(p, row->chars, actual_end);
                    p += actual_end;
                }
            } else {
                /* Middle lines: entire line with newline */
                if (row->size > 0) {
                    memcpy(p, row->chars, row->size);
                    p += row->size;
                }
                *p++ = '\n'; /* Add newline after middle lines */
            }
        }
    }
    *p = '\0';

    return buffer;
}

/* Copy selected text to clipboard */
static void selection_copy(void)
{
    if (!ec.selection.active) {
        ui_set_message("No selection to copy");
        return;
    }

    char *text = selection_get_text();
    if (text) {
        free(ec.copied_char_buffer);
        ec.copied_char_buffer = text;
        ui_set_message("Selection copied (%zu bytes)", strlen(text));
    }
}

/* Delete selected text */
static void selection_delete(void)
{
    if (!ec.selection.active || !ec.gb)
        return;

    int start_y = ec.selection.start_y;
    int start_x = ec.selection.start_x;
    int end_y = ec.selection.end_y;
    int end_x = ec.selection.end_x;

    /* Normalize selection */
    if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
        int tmp_y = start_y;
        start_y = end_y;
        end_y = tmp_y;
        int tmp_x = start_x;
        start_x = end_x;
        end_x = tmp_x;
    }

    /* Calculate position in gap buffer */
    size_t start_pos = 0;
    for (int i = 0; i < start_y && i < ec.num_rows; i++)
        start_pos += ec.row[i].size + 1;
    start_pos += start_x;

    size_t end_pos = 0;
    for (int i = 0; i < end_y && i < ec.num_rows; i++)
        end_pos += ec.row[i].size + 1;
    end_pos += end_x;

    /* Delete the selected range */
    if (end_pos > start_pos) {
        gap_delete_with_undo(ec.gb, ec.undo_stack, start_pos,
                             end_pos - start_pos);
        gap_sync_to_rows(ec.gb);
        ec.cursor_y = start_y;
        ec.cursor_x = start_x;
        ec.modified = true;
    }

    ec.selection.active = false;
    mode_set(MODE_NORMAL);
}

/* Cut selected text (copy then delete) */
static void selection_cut(void)
{
    if (!ec.selection.active) {
        ui_set_message("No selection to cut");
        return;
    }

    selection_copy();
    selection_delete();
    ui_set_message("Selection cut");
}


static void editor_newline(void)
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
    if (gap_insert_with_undo(ec.gb, ec.undo_stack, pos, &nl, 1)) {
        /* Update row structure directly */
        if (ec.cursor_x == 0) {
            row_insert(ec.cursor_y, "", 0);
        } else {
            editor_row_t *row = &ec.row[ec.cursor_y];
            row_insert(ec.cursor_y + 1, &row->chars[ec.cursor_x],
                       row->size - ec.cursor_x);
            row = &ec.row[ec.cursor_y];
            row->size = ec.cursor_x;
            row->chars[row->size] = '\0';
            row_update(row);
        }
        ec.cursor_y++;
        ec.cursor_x = 0;
        ec.modified = true;
    }
}

/* Sync gap buffer to rows for display */
static void gap_sync_to_rows(gap_buffer_t *gb)
{
    if (!gb)
        return;

    /* Save cursor position */
    int saved_y = ec.cursor_y, saved_x = ec.cursor_x;

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
    size_t pos = 0, len = gap_length(gb);

    while (pos < len) {
        size_t line_start = pos, line_end = pos;

        /* Find end of line */
        while (line_end < len && gap_get_char(gb, line_end) != '\n')
            line_end++;

        /* Extract line */
        size_t line_len = line_end - line_start;
        char *line = malloc(line_len + 1);
        if (line) {
            for (size_t i = 0; i < line_len; i++)
                line[i] = gap_get_char(gb, line_start + i);
            line[line_len] = '\0';
            row_insert(ec.num_rows, line, line_len);
            free(line);
        }

        /* Move past newline */
        pos = line_end + (gap_get_char(gb, line_end) == '\n');
    }

    /* Ensure at least one row */
    if (!ec.num_rows)
        row_insert(0, "", 0);

    /* Update modified flag from gap buffer */
    ec.modified = gb->modified;

    /* Restore cursor position within bounds */
    ec.cursor_y = saved_y >= ec.num_rows ? ec.num_rows - 1 : saved_y;
    if (ec.cursor_y >= 0 && ec.cursor_y < ec.num_rows)
        ec.cursor_x = saved_x > ec.row[ec.cursor_y].size
                          ? ec.row[ec.cursor_y].size
                          : saved_x;
}

/* Buffer for accumulating UTF-8 bytes */
static struct {
    char bytes[4];
    int len;
    int expected;
} utf8_buffer = {.len = 0, .expected = 0};

static void editor_insert_char(int c)
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
    if (gap_insert_with_undo(ec.gb, ec.undo_stack, pos, utf8_buffer.bytes,
                             utf8_buffer.len)) {
        /* Update current row directly */
        if (ec.cursor_y == ec.num_rows)
            row_insert(ec.num_rows, "", 0);

        editor_row_t *row = &ec.row[ec.cursor_y];
        row->chars = realloc(row->chars, row->size + utf8_buffer.len + 1);
        memmove(&row->chars[ec.cursor_x + utf8_buffer.len],
                &row->chars[ec.cursor_x], row->size - ec.cursor_x + 1);
        memcpy(&row->chars[ec.cursor_x], utf8_buffer.bytes, utf8_buffer.len);
        row->size += utf8_buffer.len;
        row_update(row);
        ec.cursor_x += utf8_buffer.len;
        ec.modified = true;
    }

    /* Reset UTF-8 buffer */
    utf8_buffer.len = 0;
    utf8_buffer.expected = 0;
}

static void editor_delete_char(void)
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
    for (int i = 0; i < ec.cursor_y && i < ec.num_rows; i++)
        pos += ec.row[i].size + 1; /* +1 for newline */

    if (ec.cursor_x > 0) {
        /* Delete character before cursor */
        const char *prev = utf8_prev_char(row->chars, row->chars + ec.cursor_x);
        int prev_pos = prev - row->chars, char_len = ec.cursor_x - prev_pos;

        gap_delete_with_undo(ec.gb, ec.undo_stack, pos + prev_pos, char_len);

        /* Update row directly */
        memmove(&row->chars[prev_pos], &row->chars[ec.cursor_x],
                row->size - ec.cursor_x + 1);
        row->size -= char_len;
        row_update(row);
        ec.cursor_x = prev_pos;
        ec.modified = true;
    } else {
        /* Delete newline - join with previous line */
        if (ec.cursor_y > 0) {
            pos--; /* Point to previous line's newline */
            gap_delete_with_undo(ec.gb, ec.undo_stack, pos, 1);

            /* Join lines in row structure */
            ec.cursor_x = ec.row[ec.cursor_y - 1].size;
            editor_row_t *prev_row = &ec.row[ec.cursor_y - 1];
            prev_row->chars =
                realloc(prev_row->chars, prev_row->size + row->size + 1);
            memcpy(&prev_row->chars[prev_row->size], row->chars, row->size);
            prev_row->size += row->size;
            prev_row->chars[prev_row->size] = '\0';
            row_update(prev_row);

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

static char *file_rows_to_string(int *buf_len)
{
    int total_len = 0;
    for (int j = 0; j < ec.num_rows; j++)
        total_len += ec.row[j].size + 1;
    *buf_len = total_len;
    char *buf = malloc(total_len), *p = buf;
    for (int j = 0; j < ec.num_rows; j++) {
        memcpy(p, ec.row[j].chars, ec.row[j].size);
        p += ec.row[j].size;
        *p++ = '\n';
    }
    return buf;
}

static void file_open(const char *file_name)
{
    /* Clear existing file content first */
    for (int i = 0; i < ec.num_rows; i++) {
        free(ec.row[i].chars);
        free(ec.row[i].render);
        free(ec.row[i].highlight);
    }
    free(ec.row);
    ec.row = NULL;
    ec.num_rows = 0;

    /* Reset cursor and scroll position */
    ec.cursor_x = 0;
    ec.cursor_y = 0;
    ec.row_offset = 0;
    ec.col_offset = 0;
    ec.render_x = 0;

    free(ec.file_name);
    ec.file_name = strdup(file_name);
    syntax_select();
    FILE *file = fopen(file_name, "r+");
    if (!file)
        panic("Failed to open the file");

    /* Load file into gap buffer if available */
    if (ec.gb) {
        /* Recreate gap buffer for new file */
        gap_destroy(ec.gb);
        ec.gb = gap_init(1024);
        /* Rewind file to beginning */
        fseek(file, 0, SEEK_SET);
        gap_load_file(ec.gb, file);

        /* Rewind for row loading */
        fseek(file, 0, SEEK_SET);
    }

    /* Clear undo/redo stacks for new file */
    if (ec.undo_stack) {
        undo_destroy(ec.undo_stack);
        ec.undo_stack = undo_init(100);
    }

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    while ((line_len = getline(&line, &line_cap, file)) != -1) {
        if (line_len > 0 &&
            (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
            line_len--;
        row_insert(ec.num_rows, line, line_len);
    }
    free(line);
    fclose(file);
    ec.modified = false;
}

static void file_save(void)
{
    if (!ec.file_name) {
        ec.file_name = ui_prompt("Save as: %s (ESC to cancel)", NULL);
        if (!ec.file_name) {
            ui_set_message("Save aborted");
            return;
        }
        syntax_select();
    }
    int len;
    char *buf = file_rows_to_string(&len);
    int fd = open(ec.file_name, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if ((ftruncate(fd, len) != -1) && (write(fd, buf, len) == len)) {
            close(fd);
            free(buf);
            ec.modified = false;
            if (len >= 1024)
                ui_set_message("%d KiB written to disk", len >> 10);
            else
                ui_set_message("%d B written to disk", len);
            return;
        }
        close(fd);
    }
    free(buf);
    ui_set_message("Error: %s", strerror(errno));
}

/* Global variables for search state - needed for prompt display */
static int search_last_match = -1;
static int search_total_matches = 0;
static int search_current_match = 0;

static void search_callback(char *query, int key)
{
    static int direction = 1;
    static int saved_highlight_line;
    static char *saved_hightlight = NULL;

    /* Restore previous highlighting safely */
    if (saved_hightlight && saved_highlight_line >= 0 &&
        saved_highlight_line < ec.num_rows &&
        ec.row[saved_highlight_line].highlight) {
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
        if (query && strlen(query) > 0) {
            for (int i = 0; i < ec.num_rows; i++) {
                if (!ec.row[i].render)
                    continue;
                char *p = ec.row[i].render;
                while ((p = strstr(p, query)) != NULL) {
                    search_total_matches++;
                    p++;
                }
            }
        }
    }
    /* Only search if we have a valid query */
    if (!query || strlen(query) == 0)
        return;

    int current = search_last_match;
    for (int i = 0; i < ec.num_rows; i++) {
        current += direction;
        if (current == -1)
            current = ec.num_rows - 1;
        else if (current == ec.num_rows)
            current = 0;
        editor_row_t *row = &ec.row[current];
        if (!row->render)
            continue;
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

            /* Update match counter safely */
            if (search_total_matches > 0) {
                if (direction == 1)
                    search_current_match =
                        (search_current_match % search_total_matches) + 1;
                else
                    search_current_match = (search_current_match - 1) > 0
                                               ? search_current_match - 1
                                               : search_total_matches;
            }
            break;
        }
    }
}

static void search_find(void)
{
    int saved_x = ec.cursor_x, saved_y = ec.cursor_y;
    int saved_col = ec.col_offset, saved_row = ec.row_offset;

    /* Reset search state for new search */
    search_last_match = -1;
    search_total_matches = 0;
    search_current_match = 0;

    char *query = ui_prompt("Search", search_callback);
    if (query)
        free(query);
    else {
        ec.cursor_x = saved_x;
        ec.cursor_y = saved_y;
        ec.col_offset = saved_col;
        ec.row_offset = saved_row;
    }

    /* Return to normal mode after search completes */
    mode_set(MODE_NORMAL);
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

static void buf_destroy(editor_buf_t *eb)
{
    free(eb->buf);
}

/* Calculate line number display width */
static int get_line_number_width(void)
{
    if (!ec.show_line_numbers || ec.num_rows == 0)
        return 0;

    /* Calculate width needed for line numbers */
    int max_line = ec.num_rows;
    int width = 1;
    while (max_line >= 10) {
        width++;
        max_line /= 10;
    }
    return width + 2; /* Add space for padding and separator */
}

static void editor_scroll(void)
{
    ec.render_x = 0;
    if (ec.cursor_y < ec.num_rows)
        ec.render_x = row_cursorx_to_renderx(&ec.row[ec.cursor_y], ec.cursor_x);
    if (ec.cursor_y < ec.row_offset)
        ec.row_offset = ec.cursor_y;
    if (ec.cursor_y >= ec.row_offset + ec.screen_rows)
        ec.row_offset = ec.cursor_y - ec.screen_rows + 1;

    /* Adjust horizontal scrolling for line numbers */
    int line_num_width = get_line_number_width();
    int available_cols = ec.screen_cols - line_num_width;

    if (ec.render_x < ec.col_offset)
        ec.col_offset = ec.render_x;
    if (ec.render_x >= ec.col_offset + available_cols)
        ec.col_offset = ec.render_x - available_cols + 1;
}

static void ui_draw_statusbar(editor_buf_t *eb)
{
    buf_append(eb, "\x1b[100m", 6); /* Dark gray */
    char status[80], r_status[80];

    /* Include mode in status line */
    const char *mode_name = mode_get_name(ec.mode);
    int len = snprintf(status, sizeof(status), " [%s] File: %.20s %s",
                       mode_name, ec.file_name ? ec.file_name : "< New >",
                       ec.modified ? "(modified)" : "");
    int col_size =
        ec.row && ec.cursor_y <= ec.num_rows - 1 ? ec.row[ec.cursor_y].size : 0;

#if ENABLE_TIMER
    /* Display time when timer is enabled */
    time_t now = time(NULL);
    struct tm currtime_buf;
    struct tm *currtime = localtime_r(&now, &currtime_buf);
    int r_len = snprintf(
        r_status, sizeof(r_status), "%d/%d lines  %d/%d cols [ %2d:%2d:%2d ]",
        (ec.cursor_y + 1 > ec.num_rows) ? ec.num_rows : ec.cursor_y + 1,
        ec.num_rows, ec.cursor_x + 1, col_size, currtime->tm_hour,
        currtime->tm_min, currtime->tm_sec);
#else
    /* No time display when timer is disabled */
    int r_len = snprintf(
        r_status, sizeof(r_status), "%d/%d lines  %d/%d cols",
        (ec.cursor_y + 1 > ec.num_rows) ? ec.num_rows : ec.cursor_y + 1,
        ec.num_rows, ec.cursor_x + 1, col_size);
#endif
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

static void ui_draw_messagebar(editor_buf_t *eb)
{
    buf_append(eb, "\x1b[93m\x1b[44m\x1b[K", 13);
    int msg_len = strlen(ec.status_msg);
    int displayed_len = 0;

    /* For search messages, try to show as much as possible */
    if (strstr(ec.status_msg, "Search:")) {
        /* Always show search messages, even if long */
        if (msg_len > ec.screen_cols) {
            /* Truncate but try to keep the help text visible */
            buf_append(eb, ec.status_msg, ec.screen_cols);
            displayed_len = ec.screen_cols;
        } else {
            buf_append(eb, ec.status_msg, msg_len);
            displayed_len = msg_len;
        }
    } else if (strstr(ec.status_msg, "File Browser:")) {
        /* Always show file browser messages */
        if (msg_len > ec.screen_cols) {
            buf_append(eb, ec.status_msg, ec.screen_cols);
            displayed_len = ec.screen_cols;
        } else {
            buf_append(eb, ec.status_msg, msg_len);
            displayed_len = msg_len;
        }
    } else {
        /* Regular messages: truncate and show for 5 seconds */
        if (msg_len > ec.screen_cols)
            msg_len = ec.screen_cols;
        if (msg_len && time(NULL) - ec.status_msg_time < 5) {
            buf_append(eb, ec.status_msg, msg_len);
            displayed_len = msg_len;
        }
    }

    /* Pad the rest of the line with spaces */
    while (displayed_len < ec.screen_cols) {
        buf_append(eb, " ", 1);
        displayed_len++;
    }

    buf_append(eb, "\x1b[0m", 4);
}

static void ui_set_message(const char *msg, ...)
{
    va_list args;
    va_start(args, msg);
    vsnprintf(ec.status_msg, sizeof(ec.status_msg), msg, args);
    va_end(args);
    ec.status_msg_time = time(NULL);
}

static void ui_draw_rows(editor_buf_t *eb)
{
    /* Calculate line number width if enabled */
    int line_num_width = get_line_number_width();

    for (int y = 0; y < ec.screen_rows; y++) {
        int file_row = y + ec.row_offset;

        /* Draw line number if enabled */
        if (ec.show_line_numbers) {
            if (file_row < ec.num_rows) {
                /* Draw line number */
                char line_num[32];
                int num_len = snprintf(line_num, sizeof(line_num), "%*d ",
                                       line_num_width - 1, file_row + 1);
                buf_append(eb, "\x1b[90m", 5); /* Dark gray color */
                buf_append(eb, line_num, num_len);
                buf_append(eb, "\x1b[0m", 4); /* Reset color */
            } else {
                /* Draw empty space for consistency */
                for (int i = 0; i < line_num_width; i++)
                    buf_append(eb, " ", 1);
            }
        }

        if (file_row >= ec.num_rows) {
            buf_append(eb, "~", 1);
        } else {
            int available_cols = ec.screen_cols - line_num_width;
            int len = ec.row[file_row].render_size - ec.col_offset;
            if (len < 0)
                len = 0;
            if (len > available_cols)
                len = available_cols;
            char *c = &ec.row[file_row].render[ec.col_offset];
            unsigned char *hl = &ec.row[file_row].highlight[ec.col_offset];
            int current_color = -1;
            bool in_selection = false;

            for (int j = 0; j < len; j++) {
                /* Check if this character is in selection */
                int cursor_x = row_renderx_to_cursorx(&ec.row[file_row],
                                                      ec.col_offset + j);
                bool is_selected = selection_contains(cursor_x, file_row);

                /* Handle selection highlighting transitions */
                if (is_selected && !in_selection) {
                    /* Enter selection - use inverse video */
                    buf_append(eb, "\x1b[7m", 4);
                    in_selection = true;
                } else if (!is_selected && in_selection) {
                    /* Exit selection */
                    buf_append(eb, "\x1b[27m", 5);
                    in_selection = false;
                }

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
                    int color = syntax_token_color(hl[j]);
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
            /* Ensure selection highlighting is turned off at end of line */
            if (in_selection)
                buf_append(eb, "\x1b[27m", 5);
            buf_append(eb, "\x1b[39m", 5);
        }
        buf_append(eb, "\x1b[K", 3);
        buf_append(eb, "\r\n", 2);
    }
}

static void editor_refresh(void)
{
    editor_scroll();
    editor_buf_t eb = {NULL, 0};
    buf_append(&eb, "\x1b[?25l", 6);
    buf_append(&eb, "\x1b[H", 3);
    ui_draw_rows(&eb);
    ui_draw_statusbar(&eb);
    ui_draw_messagebar(&eb);

    /* Adjust cursor position for line numbers */
    int line_num_width = get_line_number_width();
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (ec.cursor_y - ec.row_offset) + 1,
             (ec.render_x - ec.col_offset) + 1 + line_num_width);
    buf_append(&eb, buf, strlen(buf));
    buf_append(&eb, "\x1b[?25h", 6);
    write(STDOUT_FILENO, eb.buf, eb.len);
    buf_destroy(&eb);
}

/* Force full screen refresh by clearing first */
static void editor_refresh_full(void)
{
    editor_scroll();
    editor_buf_t eb = {NULL, 0};
    buf_append(&eb, "\x1b[?25l", 6);
    buf_append(&eb, "\x1b[2J", 4); /* Clear entire screen */
    buf_append(&eb, "\x1b[H", 3);
    ui_draw_rows(&eb);
    ui_draw_statusbar(&eb);
    ui_draw_messagebar(&eb);

    /* Adjust cursor position for line numbers */
    int line_num_width = get_line_number_width();
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (ec.cursor_y - ec.row_offset) + 1,
             (ec.render_x - ec.col_offset) + 1 + line_num_width);
    buf_append(&eb, buf, strlen(buf));
    buf_append(&eb, "\x1b[?25h", 6);
    write(STDOUT_FILENO, eb.buf, eb.len);
    buf_destroy(&eb);
}

static void sig_winch_handler(int sig)
{
    (void) sig; /* Unused parameter */
    term_update_size();
    if (ec.cursor_y > ec.screen_rows)
        ec.cursor_y = ec.screen_rows - 1;
    if (ec.cursor_x > ec.screen_cols)
        ec.cursor_x = ec.screen_cols - 1;
    editor_refresh();
}

static void sig_cont_handler(int sig)
{
    (void) sig; /* Unused parameter */
    term_disable_raw();
    term_open_buffer();
    term_enable_raw();
    editor_refresh();
}

static bool ui_confirm(const char *msg)
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

        ui_set_message("%s", status_msg);
        editor_refresh();

        int c = term_read_key();
        switch (c) {
        case '\r': /* Enter key */
            ui_set_message("");
            return choice;
        case '\x1b': /* ESC key */
        case CTRL_('q'):
            ui_set_message("");
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

static char *ui_prompt(const char *msg, void (*callback)(char *, int))
{
    size_t buf_size = 128;
    char *buf = malloc(buf_size);
    if (!buf)
        return NULL;
    size_t buf_len = 0;
    buf[0] = '\0';

    /* Check if this is a search prompt */
    bool is_search = (callback == search_callback);

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

            ui_set_message("%s", display_msg);
        } else {
            /* For non-search prompts, use the format string */
            char formatted_msg[256];
            snprintf(formatted_msg, sizeof(formatted_msg), msg, buf);
            ui_set_message("%s", formatted_msg);
        }
        editor_refresh();
        int c = term_read_key();
        if ((c == DEL_KEY) || (c == CTRL_('h')) || (c == BACKSPACE)) {
            if (buf_len != 0)
                buf[--buf_len] = '\0';
        } else if (c == '\x1b') {
            ui_set_message("");
            if (callback)
                callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buf_len != 0) {
                ui_set_message("");
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

static void editor_move_cursor(int key)
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

/* File browser implementation */

/* Get file extension */
static const char *get_file_extension(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename)
        return "";
    return dot + 1;
}

/* Get file type indicator and color */
static const char *get_file_type_info(const char *filename, int *color)
{
    if (filename[0] == '/') {
        *color = 34; /* Blue for directories */
        return "[DIR]  ";
    }

    const char *ext = get_file_extension(filename);

    /* Source and script files */
    if (!strcasecmp(ext, "c") || !strcasecmp(ext, "h") ||
        !strcasecmp(ext, "cpp") || !strcasecmp(ext, "cxx") ||
        !strcasecmp(ext, "hpp") || !strcasecmp(ext, "cc") ||
        !strcasecmp(ext, "sh") || !strcasecmp(ext, "py") ||
        !strcasecmp(ext, "rb") || !strcasecmp(ext, "js") ||
        !strcasecmp(ext, "rs") || !strcasecmp(ext, "go") ||
        !strcasecmp(ext, "java") || !strcasecmp(ext, "php") ||
        !strcasecmp(ext, "pl") || !strcasecmp(ext, "lua") ||
        !strcasecmp(ext, "vim") || !strcasecmp(ext, "asm") ||
        !strcasecmp(ext, "s")) {
        *color = 32; /* Green for source */
        return "[SRC]  ";
    }

    /* All other files */
    *color = 37; /* White for others */
    return "[FILE] ";
}

static void browser_free_entries(void)
{
    if (ec.mode_state.browser.entries) {
        for (int i = 0; i < ec.mode_state.browser.num_entries; i++)
            free(ec.mode_state.browser.entries[i]);
        free(ec.mode_state.browser.entries);
        ec.mode_state.browser.entries = NULL;
        ec.mode_state.browser.num_entries = 0;
    }
    free(ec.mode_state.browser.current_dir);
    ec.mode_state.browser.current_dir = NULL;
}

static int browser_compare_entries(const void *a, const void *b)
{
    const char *name_a = *(const char **) a, *name_b = *(const char **) b;

    /* Directories first (start with '/'), then files */
    bool is_dir_a = (name_a[0] == '/');
    bool is_dir_b = (name_b[0] == '/');

    if (is_dir_a && !is_dir_b)
        return -1;
    if (!is_dir_a && is_dir_b)
        return 1;

    /* Compare names, ignoring the '/' prefix for directories */
    const char *cmp_a = is_dir_a ? name_a + 1 : name_a;
    const char *cmp_b = is_dir_b ? name_b + 1 : name_b;

    return strcasecmp(cmp_a, cmp_b);
}

static void browser_load_directory(const char *path)
{
    browser_free_entries();

    DIR *dir = opendir(path ? path : ".");
    if (!dir) {
        ui_set_message("Cannot open directory: %s", strerror(errno));
        mode_set(MODE_NORMAL);
        return;
    }

    /* Store current directory */
    ec.mode_state.browser.current_dir = strdup(path ? path : ".");

    /* Count entries first */
    int capacity = 32;
    ec.mode_state.browser.entries = malloc(sizeof(char *) * capacity);
    ec.mode_state.browser.num_entries = 0;

    /* Add parent directory if not root */
    if (strcmp(ec.mode_state.browser.current_dir, "/")) {
        ec.mode_state.browser.entries[ec.mode_state.browser.num_entries++] =
            strdup("/..");
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        /* Skip current and parent directory entries */
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        /* Skip hidden files if show_hidden is false */
        if (!ec.mode_state.browser.show_hidden && de->d_name[0] == '.')
            continue;

        /* Check if we need to resize array */
        if (ec.mode_state.browser.num_entries >= capacity - 1) {
            capacity *= 2;
            ec.mode_state.browser.entries = realloc(
                ec.mode_state.browser.entries, sizeof(char *) * capacity);
        }

        /* Get file info to determine if it's a directory */
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s",
                 ec.mode_state.browser.current_dir, de->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                /* Directory - prefix with '/' */
                char *entry = malloc(strlen(de->d_name) + 2);
                sprintf(entry, "/%s", de->d_name);
                ec.mode_state.browser
                    .entries[ec.mode_state.browser.num_entries++] = entry;
            } else if (S_ISREG(st.st_mode)) {
                /* Regular file */
                ec.mode_state.browser
                    .entries[ec.mode_state.browser.num_entries++] =
                    strdup(de->d_name);
            }
        }
    }

    closedir(dir);

    /* Sort entries: directories first, then files, both alphabetically */
    if (ec.mode_state.browser.num_entries > 0) {
        qsort(ec.mode_state.browser.entries, ec.mode_state.browser.num_entries,
              sizeof(char *), browser_compare_entries);
    }

    ec.mode_state.browser.selected = 0;
    ec.mode_state.browser.offset = 0;
}

static void browser_open_selected(void)
{
    if (ec.mode_state.browser.selected >= ec.mode_state.browser.num_entries)
        return;

    char *entry = ec.mode_state.browser.entries[ec.mode_state.browser.selected];
    if (!entry)
        return;

    if (entry[0] == '/') {
        /* Directory */
        if (!strcmp(entry, "/..")) {
            /* Go to parent directory */
            char *last_slash = strrchr(ec.mode_state.browser.current_dir, '/');
            if (last_slash && last_slash != ec.mode_state.browser.current_dir) {
                *last_slash = '\0';
                browser_load_directory(ec.mode_state.browser.current_dir);
            } else {
                browser_load_directory("/");
            }
        } else {
            /* Enter subdirectory */
            char new_path[PATH_MAX];
            snprintf(new_path, sizeof(new_path), "%s%s",
                     ec.mode_state.browser.current_dir, entry);
            browser_load_directory(new_path);
        }
    } else {
        /* File - open it */
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s",
                 ec.mode_state.browser.current_dir, entry);

        /* Save current file if modified */
        if (ec.modified) {
            if (!ui_confirm("Current file has been modified. Save before "
                            "opening new file?")) {
                return;
            }
            file_save();
        }

        /* Open the new file */
        file_open(full_path);
        browser_free_entries();
        mode_set(MODE_NORMAL);
        ui_set_message("Opened: %s", full_path);
        editor_refresh_full(); /* Full redraw the editor with new file */
    }
}

static void browser_render(void)
{
    editor_buf_t eb = {NULL, 0};

    /* Clear entire screen */
    buf_append(&eb, "\x1b[?25l", 6); /* Hide cursor */
    buf_append(&eb, "\x1b[2J", 4);   /* Clear entire screen */
    buf_append(&eb, "\x1b[H", 3);    /* Move cursor home */

    /* Title bar */
    char title[256];
    snprintf(title, sizeof(title), "=== File Browser: %s ===\r\n",
             ec.mode_state.browser.current_dir);
    buf_append(&eb, "\x1b[7m", 4); /* Inverse video */
    buf_append(&eb, title, strlen(title));
    buf_append(&eb, "\x1b[0m", 4); /* Reset */

    /* Calculate visible entries */
    int visible_lines = ec.screen_rows - 1; /* Subtract title line */

    /* Adjust offset if needed */
    if (ec.mode_state.browser.selected < ec.mode_state.browser.offset) {
        ec.mode_state.browser.offset = ec.mode_state.browser.selected;
    }
    if (ec.mode_state.browser.selected >=
        ec.mode_state.browser.offset + visible_lines) {
        ec.mode_state.browser.offset =
            ec.mode_state.browser.selected - visible_lines + 1;
    }

    /* Display entries */
    int lines_printed = 0;
    for (int i = 0; i < visible_lines && i + ec.mode_state.browser.offset <
                                             ec.mode_state.browser.num_entries;
         i++) {
        int idx = i + ec.mode_state.browser.offset;
        char *entry = ec.mode_state.browser.entries[idx];

        /* Highlight selected entry */
        if (idx == ec.mode_state.browser.selected)
            buf_append(&eb, "\x1b[7m", 4); /* Inverse video */

        /* Display entry with colored icon */
        int color;
        const char *type_str = get_file_type_info(entry, &color);

        /* Add color for file type */
        char color_buf[16];
        snprintf(color_buf, sizeof(color_buf), "\x1b[%dm", color);
        buf_append(&eb, color_buf, strlen(color_buf));

        buf_append(&eb, "  ", 2);
        buf_append(&eb, type_str, strlen(type_str));

        /* Display name */
        if (entry[0] == '/') {
            /* Directory name without leading slash */
            buf_append(&eb, entry + 1, strlen(entry + 1));
        } else {
            /* File name */
            buf_append(&eb, entry, strlen(entry));
        }

        /* Reset color */
        buf_append(&eb, "\x1b[0m", 4);

        if (idx == ec.mode_state.browser.selected)
            buf_append(&eb, "\x1b[0m", 4); /* Reset */

        buf_append(&eb, "\x1b[K", 3); /* Clear to end of line */

        lines_printed++;
        /* Only add newline if not the last line before status */
        if (lines_printed < visible_lines)
            buf_append(&eb, "\r\n", 2);
    }

    /* Fill remaining lines */
    for (int i = lines_printed; i < visible_lines; i++) {
        if (i > lines_printed || lines_printed > 0)
            buf_append(&eb, "\r\n", 2);
        buf_append(&eb, "~\x1b[K", 4);
    }

    /* Always add final newline before status bar */
    if (visible_lines > 0)
        buf_append(&eb, "\r\n", 2);

    /* Draw status bar similar to ui_draw_statusbar */
    buf_append(&eb, "\x1b[100m", 6); /* Dark gray background */
    char status[80], r_status[80];

    /* Left side: mode and path */
    int len = snprintf(status, sizeof(status), " [BROWSER] %s",
                       ec.mode_state.browser.current_dir);

    /* Right side: file count and time */
#if ENABLE_TIMER
    time_t now = time(NULL);
    struct tm currtime_buf;
    struct tm *currtime = localtime_r(&now, &currtime_buf);
    int r_len = snprintf(
        r_status, sizeof(r_status), "%d/%d files [ %2d:%2d:%2d ]",
        ec.mode_state.browser.selected + 1, ec.mode_state.browser.num_entries,
        currtime->tm_hour, currtime->tm_min, currtime->tm_sec);
#else
    int r_len = snprintf(r_status, sizeof(r_status), "%d/%d files",
                         ec.mode_state.browser.selected + 1,
                         ec.mode_state.browser.num_entries);
#endif

    if (len > ec.screen_cols)
        len = ec.screen_cols;
    buf_append(&eb, status, len);

    while (len < ec.screen_cols) {
        if (ec.screen_cols - len == r_len) {
            buf_append(&eb, r_status, r_len);
            break;
        }
        buf_append(&eb, " ", 1);
        len++;
    }
    buf_append(&eb, "\x1b[m", 3); /* Reset */
    buf_append(&eb, "\r\n", 2);   /* Move to message bar line */

    /* Use ui_draw_messagebar for the bottom message bar */
    ui_draw_messagebar(&eb);

    write(STDOUT_FILENO, eb.buf, eb.len);
    buf_destroy(&eb);
}

/* Clean up all allocated memory before exit */
static void editor_cleanup(void)
{
    /* Free all rows */
    for (int i = 0; i < ec.num_rows; i++) {
        free(ec.row[i].chars);
        free(ec.row[i].render);
        free(ec.row[i].highlight);
    }
    free(ec.row);
    ec.row = NULL;
    ec.num_rows = 0;

    /* Free file name */
    free(ec.file_name);
    ec.file_name = NULL;

    /* Free copied buffer */
    free(ec.copied_char_buffer);
    ec.copied_char_buffer = NULL;

    /* Free gap buffer */
    if (ec.gb) {
        gap_destroy(ec.gb);
        ec.gb = NULL;
    }

    /* Free undo stack */
    if (ec.undo_stack) {
        undo_destroy(ec.undo_stack);
        ec.undo_stack = NULL;
    }

    /* Free mode state */
    free(ec.mode_state.search.query);
    ec.mode_state.search.query = NULL;
    free(ec.mode_state.prompt.buffer);
    ec.mode_state.prompt.buffer = NULL;
    browser_free_entries();
}

static void editor_process_key(void)
{
    static int indent_level = 0;
    int c = term_read_key();

    /* Handle mode-specific keys first */
    switch (ec.mode) {
    case MODE_BROWSER:
        /* File browser mode */
        switch (c) {
        case '\x1b': /* ESC - cancel */
        case CTRL_('q'):
            browser_free_entries();
            mode_set(MODE_NORMAL);
            editor_refresh_full(); /* Full redraw the editor */
            return;
        case '\r': /* Enter - open file/directory */
            browser_open_selected();
            return;
        case ARROW_UP:
            if (ec.mode_state.browser.selected > 0) {
                ec.mode_state.browser.selected--;
            }
            browser_render();
            return;
        case ARROW_DOWN:
            if (ec.mode_state.browser.selected <
                ec.mode_state.browser.num_entries - 1) {
                ec.mode_state.browser.selected++;
            }
            browser_render();
            return;
        case PAGE_UP:
            ec.mode_state.browser.selected -= ec.screen_rows - 3;
            if (ec.mode_state.browser.selected < 0)
                ec.mode_state.browser.selected = 0;
            browser_render();
            return;
        case PAGE_DOWN:
            ec.mode_state.browser.selected += ec.screen_rows - 3;
            if (ec.mode_state.browser.selected >=
                ec.mode_state.browser.num_entries)
                ec.mode_state.browser.selected =
                    ec.mode_state.browser.num_entries - 1;
            browser_render();
            return;
        case HOME_KEY:
            ec.mode_state.browser.selected = 0;
            browser_render();
            return;
        case END_KEY:
            ec.mode_state.browser.selected =
                ec.mode_state.browser.num_entries - 1;
            browser_render();
            return;
        case 'h':
        case 'H':
            /* Toggle hidden files */
            ec.mode_state.browser.show_hidden =
                !ec.mode_state.browser.show_hidden;
            browser_load_directory(ec.mode_state.browser.current_dir);
            browser_render();
            return;
        default:
            /* Ignore other keys */
            return;
        }
        break;

    case MODE_SELECT:
        switch (c) {
        case '\x1b': /* ESC - abort selection */
            ec.selection.active = false;
            mode_set(MODE_NORMAL);
            ui_set_message("Mark cancelled");
            return;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            /* Update selection while moving */
            editor_move_cursor(c);
            /* Ensure selection doesn't go past last row */
            if (ec.cursor_y >= ec.num_rows && ec.num_rows > 0) {
                ec.cursor_y = ec.num_rows - 1;
                ec.cursor_x = ec.row[ec.cursor_y].size;
            }
            ec.selection.end_x = ec.cursor_x;
            ec.selection.end_y = ec.cursor_y;
            return;
        case HOME_KEY:
            ec.cursor_x = 0;
            ec.selection.end_x = ec.cursor_x;
            ec.selection.end_y = ec.cursor_y;
            return;
        case END_KEY:
            if (ec.cursor_y < ec.num_rows)
                ec.cursor_x = ec.row[ec.cursor_y].size;
            ec.selection.end_x = ec.cursor_x;
            ec.selection.end_y = ec.cursor_y;
            return;
        case PAGE_UP:
        case PAGE_DOWN: {
            if (c == PAGE_UP)
                ec.cursor_y = ec.row_offset;
            else
                ec.cursor_y = ec.row_offset + ec.screen_rows - 1;
            int times = ec.screen_rows;
            while (times--)
                editor_move_cursor((c == PAGE_UP) ? ARROW_UP : ARROW_DOWN);
            ec.selection.end_x = ec.cursor_x;
            ec.selection.end_y = ec.cursor_y;
            return;
        }
        case CTRL_('c'): /* Copy marked text and exit marking */
            selection_copy();
            mode_set(MODE_NORMAL);
            ui_set_message("Copied marked text");
            return;
        case CTRL_('k'): /* Cut marked text and exit marking */
            selection_cut();
            mode_set(MODE_NORMAL);
            ui_set_message("Cut marked text");
            return;
        case CTRL_('v'): /* Paste over selection */
            selection_delete();
            editor_paste();
            mode_set(MODE_NORMAL);
            return;
        case DEL_KEY:
        case BACKSPACE:
            selection_delete();
            return;
        default:
            /* Exit selection mode for other keys */
            mode_set(MODE_NORMAL);
            break;
        }
        break;

    case MODE_HELP:
        /* Any key exits help mode */
        mode_restore();
        editor_refresh_full(); /* Full redraw after exiting help */
        return;

    case MODE_SEARCH:
    case MODE_PROMPT:
    case MODE_CONFIRM:
        /* These modes handle their own input */
        return;

    case MODE_NORMAL:
    default:
        break;
    }

    /* Normal mode key handling */
    switch (c) {
    case '\r':
        editor_newline();
        for (int i = 0; i < indent_level; i++)
            editor_insert_char('\t');
        break;
    case CTRL_('q'):
        if (ec.modified) {
            if (!ui_confirm("File has been modified. Quit without saving?"))
                return;
        }
        term_clear();
        term_close_buffer();
        editor_cleanup();
        exit(0);
        break;
    case CTRL_('s'):
        file_save();
        break;
    case CTRL_('x'): /* Start text marking */
        if (ec.mode != MODE_SELECT) {
            mode_set(MODE_SELECT);
            ui_set_message(
                "Mark set - Move cursor to select, ^C=Copy, ^K=Cut, "
                "ESC=Cancel");
        }
        break;
    case CTRL_('c'): /* Copy current line (no marking in normal mode) */
        if (ec.cursor_y < ec.num_rows)
            editor_copy(0);
        break;
    case CTRL_('k'): /* Cut from cursor to end of line */
        if (ec.cursor_y < ec.num_rows) {
            editor_row_t *row = &ec.row[ec.cursor_y];
            if (ec.cursor_x < row->size) {
                /* Cut from cursor to end of line */
                int len = row->size - ec.cursor_x;
                char *text = malloc(len + 1);
                if (text) {
                    memcpy(text, &row->chars[ec.cursor_x], len);
                    text[len] = '\0';

                    /* Store in copied_char_buffer */
                    free(ec.copied_char_buffer);
                    ec.copied_char_buffer = text;

                    /* Delete from cursor to end using gap buffer */
                    size_t pos = 0;
                    for (int i = 0; i < ec.cursor_y; i++)
                        pos += ec.row[i].size + 1;
                    pos += ec.cursor_x;

                    if (ec.gb)
                        gap_delete_with_undo(ec.gb, ec.undo_stack, pos, len);

                    /* Update row */
                    row->size = ec.cursor_x;
                    row->chars[row->size] = '\0';
                    row_update(row);
                    ec.modified = true;
                    ui_set_message("Cut to end of line");
                }
            } else if (ec.cursor_x == row->size &&
                       ec.cursor_y < ec.num_rows - 1) {
                /* At end of line, join with next line by deleting newline */
                size_t pos = 0;
                for (int i = 0; i < ec.cursor_y; i++)
                    pos += ec.row[i].size + 1;
                pos += row->size;

                if (ec.gb) {
                    /* Delete the newline character */
                    gap_delete_with_undo(ec.gb, ec.undo_stack, pos, 1);
                }

                /* Join rows */
                int next_len = ec.row[ec.cursor_y + 1].size;
                row->chars = realloc(row->chars, row->size + next_len + 1);
                memcpy(&row->chars[row->size], ec.row[ec.cursor_y + 1].chars,
                       next_len);
                row->size += next_len;
                row->chars[row->size] = '\0';
                row_update(row);

                /* Delete the next row using memmove */
                free(ec.row[ec.cursor_y + 1].chars);
                free(ec.row[ec.cursor_y + 1].render);
                free(ec.row[ec.cursor_y + 1].highlight);
                if (ec.cursor_y + 1 < ec.num_rows - 1) {
                    memmove(
                        &ec.row[ec.cursor_y + 1], &ec.row[ec.cursor_y + 2],
                        sizeof(editor_row_t) * (ec.num_rows - ec.cursor_y - 2));
                }
                ec.num_rows--;
                ec.modified = true;
                ui_set_message("Lines joined");
            } else {
                /* Empty line - cut whole line */
                editor_cut();
            }
        }
        break;
    case CTRL_('v'): /* Paste/uncut */
        editor_paste();
        break;
    case CTRL_('z'):
        /* Undo last operation */
        if (ec.gb && ec.undo_stack) {
            if (undo_perform(ec.gb, ec.undo_stack)) {
                ec.modified = ec.gb->modified;
                ui_set_message("Undo performed");
            } else {
                ui_set_message("Nothing to undo");
            }
        } else {
            ui_set_message("Undo system not initialized");
        }
        break;
    case CTRL_('r'):
        /* Redo last undone operation */
        if (ec.gb && ec.undo_stack) {
            if (undo_redo(ec.gb, ec.undo_stack)) {
                ec.modified = ec.gb->modified;
                ui_set_message("Redo performed");
            } else {
                ui_set_message("Nothing to redo");
            }
        } else {
            ui_set_message("Undo system not initialized");
        }
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editor_move_cursor(c);
        break;
    case PAGE_UP:
    case PAGE_DOWN: {
        if (c == PAGE_UP)
            ec.cursor_y = ec.row_offset;
        else if (c == PAGE_DOWN)
            ec.cursor_y = ec.row_offset + ec.screen_rows - 1;
        int times = ec.screen_rows;
        while (times--)
            editor_move_cursor((c == PAGE_UP) ? ARROW_UP : ARROW_DOWN);
    } break;
    case HOME_KEY:
        ec.cursor_x = 0;
        break;
    case END_KEY:
        if (ec.cursor_y < ec.num_rows)
            ec.cursor_x = ec.row[ec.cursor_y].size;
        break;
    case CTRL_('f'):
        search_find();
        break;
    case CTRL_('n'): /* Toggle line numbers */
        ec.show_line_numbers = !ec.show_line_numbers;
        ui_set_message("Line numbers %s",
                       ec.show_line_numbers ? "enabled" : "disabled");
        break;
    case CTRL_('o'): /* Open file browser */
        mode_set(MODE_BROWSER);
        browser_load_directory(".");
        ui_set_message("File Browser: Enter to open, ESC to cancel");
        browser_render();
        return;      /* Don't continue to normal refresh */
    case CTRL_('?'): /* Show help */
        mode_set(MODE_HELP);
        /* Generate comprehensive help */
        {
            static char help_buffer[256];
            help_generate(help_buffer, sizeof(help_buffer));
            ui_set_message(
                "Press any key to close. Key bindings: ^Q=Quit ^S=Save ^F=Find "
                "^X=Mark ^C=Copy ^K=Cut ^V=Paste ^Z=Undo");
        }
        break;
    case BACKSPACE:
    case CTRL_('h'):
    case DEL_KEY:
        if (c == DEL_KEY)
            editor_move_cursor(ARROW_RIGHT);
        editor_delete_char();
        break;
    case CTRL_('l'):
    case '\x1b':
        break;
    case '{':
        editor_insert_char(c);
        indent_level++;
        break;
    case '}':
        if (ec.cursor_y == ec.num_rows)
            goto none;
        if ((ec.cursor_x == 0) && (ec.cursor_y == 0))
            goto none;
        editor_row_t *row = &ec.row[ec.cursor_y];
        if ((ec.cursor_x > 0) && (row->chars[ec.cursor_x - 1] == '\t'))
            editor_delete_char();
    none:
        editor_insert_char(c);
        indent_level--;
        break;
    default:
        editor_insert_char(c);
    }
}

#if ENABLE_TIMER
static bool timer_check_update(void)
{
    time_t current_time = time(NULL);
    if (current_time != ec.last_update_time) {
        ec.last_update_time = current_time;
        return true; /* Need refresh */
    }
    return false;
}
#endif

static void editor_init(void)
{
    term_update_size();
    signal(SIGWINCH, sig_winch_handler);
    signal(SIGCONT, sig_cont_handler);

    /* Initialize gap buffer and undo/redo - always enabled */
    ec.gb = gap_init(GAP_INITIAL_SIZE);
    if (ec.gb)
        ec.undo_stack = undo_init(MAX_UNDO_LEVELS);

#if ENABLE_TIMER
    /* Initialize time tracking */
    ec.last_update_time = time(NULL);
#endif
}

int main(int argc, char *argv[])
{
    editor_init();
    if (argc >= 2)
        file_open(argv[1]);
    term_enable_raw();
    ui_set_message("Mazu Editor | Ctrl-? Help");
    editor_refresh();

    /* Main event loop */
    while (1) {
#if ENABLE_TIMER
        /* Check if timer needs refresh */
        if (timer_check_update() && ec.mode != MODE_BROWSER)
            editor_refresh();

        /* Use poll for non-blocking keyboard input */
        struct pollfd fds[1];
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;

        /* Poll with 100ms timeout */
        int ret = poll(fds, 1, 100);

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            editor_process_key();
            if (ec.mode != MODE_BROWSER)
                editor_refresh();
        }
#else
        /* Simple blocking read when timer is disabled */
        editor_process_key();
        if (ec.mode != MODE_BROWSER)
            editor_refresh();
#endif
    }
    /* not reachable */
    return 0;
}
