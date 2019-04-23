// Mazu Editor: minimalist editor with syntax highlight, copy/paste, and search

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
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

typedef struct {
    int idx;
    int size;
    int render_size;
    char *chars;
    char *render;
    unsigned char *highlight;
    int hl_open_comment;
} editor_row;

typedef struct {
    char *file_type;
    char **file_match;
    char **keywords;
    char *sl_comment_start;                  /* single line */
    char *ml_comment_start, *ml_comment_end; /* multiple lines */
    int flags;
} editor_syntax;

struct {
    int cursor_x, cursor_y, render_x;
    int row_offset, col_offset;
    int screen_rows, screen_cols;
    int num_rows;
    editor_row *row;
    int modified;
    char *file_name;
    char status_msg[80];
    time_t status_msg_time;
    char *copied_char_buffer;
    editor_syntax *syntax;
    struct termios orig_termios;
} ec = {
    /* editor config */
    .cursor_x = 0,         .cursor_y = 0,        .render_x = 0,
    .row_offset = 0,       .col_offset = 0,      .num_rows = 0,
    .row = NULL,           .modified = 0,        .file_name = NULL,
    .status_msg[0] = '\0', .status_msg_time = 0, .copied_char_buffer = NULL,
    .syntax = NULL,
};

typedef struct {
    char *buf;
    int len;
} editor_buf;

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

char *C_keywords[] = {"switch",    "if",       "while",   "for",     "break",
                      "continue",  "return",   "else",    "struct",  "union",
                      "typedef",   "static",   "enum",    "class",   "case",
                      "volatile",  "register", "sizeof",  "typedef", "union",
                      "goto",      "const",    "auto",    "#if",     "#endif",
                      "#error",    "#ifdef",   "#ifndef", "#elif",   "#define",
                      "#undef",    "#include",

                      "int|",      "long|",    "double|", "float|",  "char|",
                      "unsigned|", "signed|",  "void|",   "bool|",   NULL};

editor_syntax DB[] = {
    {"c", C_extensions, C_keywords, "//", "/*", "*/",
     HIGHLIGHT_NUMBERS | HIGHLIGHT_STRINGS},
};

#define DB_ENTRIES (sizeof(DB) / sizeof(DB[0]))

void set_status_message(const char *msg, ...);
char *prompt(char *msg, void (*callback)(char *, int));

void clear_screen()
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
}

void panic(const char *s)
{
    clear_screen();
    perror(s);
    puts("\r\n");
    exit(1);
}

void open_buffer()
{
    if (write(STDOUT_FILENO, "\x1b[?47h", 6) == -1)
        panic("Error changing terminal buffer");
}

void disable_raw_mode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ec.orig_termios) == -1)
        panic("Failed to disable raw mode");
}

void enable_raw_mode()
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

int read_key()
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

int get_window_size(int *screen_rows, int *screen_cols)
{
    struct winsize ws;
    if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) || (ws.ws_col == 0))
        return -1;
    *screen_cols = ws.ws_col;
    *screen_rows = ws.ws_row;
    return 0;
}

void update_window_size()
{
    if (get_window_size(&ec.screen_rows, &ec.screen_cols) == -1)
        panic("Failed to get window size");
    ec.screen_rows -= 2;
}

void close_buffer()
{
    if (write(STDOUT_FILENO, "\x1b[?9l", 5) == -1 ||
        write(STDOUT_FILENO, "\x1b[?47l", 6) == -1)
        panic("Error restoring buffer state");
    clear_screen();
}

bool is_token_separator(int c)
{
    return isspace(c) || (c == '\0') || strchr(",.()+-/*=~%<>[]:;", c);
}

bool is_part_of_number(int c)
{
    return c == '.' || c == 'x' || c == 'a' || c == 'b' || c == 'c' ||
           c == 'd' || c == 'e' || c == 'f' || c == 'A' || c == 'X' ||
           c == 'B' || c == 'C' || c == 'D' || c == 'E' || c == 'F' ||
           c == 'h' || c == 'H';
}

void highlight(editor_row *row)
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
    int in_comment = (row->idx > 0 && ec.row[row->idx - 1].hl_open_comment);
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
int token_to_color(int highlight)
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
        return 35; /* Magenta */
    default:
        return 97;
    }
}

void select_highlight()
{
    ec.syntax = NULL;
    if (!ec.file_name)
        return;
    for (size_t j = 0; j < DB_ENTRIES; j++) {
        editor_syntax *es = &DB[j];
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

int row_cursorx_to_renderx(editor_row *row, int cursor_x)
{
    int render_x = 0;
    for (int j = 0; j < cursor_x; j++) {
        if (row->chars[j] == '\t')
            render_x += (TAB_STOP - 1) - (render_x % TAB_STOP);
        render_x++;
    }
    return render_x;
}

int row_renderx_to_cursorx(editor_row *row, int render_x)
{
    int cur_render_x = 0;
    int cursor_x;
    for (cursor_x = 0; cursor_x < row->size; cursor_x++) {
        if (row->chars[cursor_x] == '\t')
            cur_render_x += (TAB_STOP - 1) - (cur_render_x % TAB_STOP);
        cur_render_x++;
        if (cur_render_x > render_x)
            return cursor_x;
    }
    return cursor_x;
}

void update_row(editor_row *row)
{
    int tabs = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t')
            tabs++;
    }
    free(row->render);
    row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);
    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0)
                row->render[idx++] = ' ';
        } else
            row->render[idx++] = row->chars[j];
    }
    row->render[idx] = '\0';
    row->render_size = idx;
    highlight(row);
}

void insert_row(int at, char *s, size_t line_len)
{
    if ((at < 0) || (at > ec.num_rows))
        return;
    ec.row = realloc(ec.row, sizeof(editor_row) * (ec.num_rows + 1));
    memmove(&ec.row[at + 1], &ec.row[at],
            sizeof(editor_row) * (ec.num_rows - at));
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
    ec.row[at].hl_open_comment = 0;
    update_row(&ec.row[at]);
    ec.num_rows++;
    ec.modified++;
}

void free_row(editor_row *row)
{
    free(row->render);
    free(row->chars);
    free(row->highlight);
}

void delete_row(int at)
{
    if (at < 0 || at >= ec.num_rows)
        return;
    free_row(&ec.row[at]);
    memmove(&ec.row[at], &ec.row[at + 1],
            sizeof(editor_row) * (ec.num_rows - at - 1));
    for (int j = at; j < ec.num_rows - 1; j++)
        ec.row[j].idx--;
    ec.num_rows--;
    ec.modified++;
}

void row_append(editor_row *row, char *s, size_t len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    update_row(row);
    ec.modified++;
}

void copy(int cut)
{
    ec.copied_char_buffer =
        realloc(ec.copied_char_buffer, strlen(ec.row[ec.cursor_y].chars) + 1);
    strcpy(ec.copied_char_buffer, ec.row[ec.cursor_y].chars);
    set_status_message(cut ? "Text cut" : "Text copied");
}

void cut()
{
    copy(-1);
    delete_row(ec.cursor_y);
    if (ec.num_rows - ec.cursor_y > 0)
        highlight(&ec.row[ec.cursor_y]);
    if (ec.num_rows - ec.cursor_y > 1)
        highlight(&ec.row[ec.cursor_y + 1]);
    ec.cursor_x = ec.cursor_y == ec.num_rows ? 0 : ec.row[ec.cursor_y].size;
}

void paste()
{
    if (!ec.copied_char_buffer)
        return;
    if (ec.cursor_y == ec.num_rows)
        insert_row(ec.cursor_y, ec.copied_char_buffer,
                   strlen(ec.copied_char_buffer));
    else
        row_append(&ec.row[ec.cursor_y], ec.copied_char_buffer,
                   strlen(ec.copied_char_buffer));
    ec.cursor_x += strlen(ec.copied_char_buffer);
}

void row_insert_char(editor_row *row, int at, int c)
{
    if ((at < 0) || (at > row->size))
        at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    update_row(row);
    ec.modified++;
}

void newline()
{
    if (ec.cursor_x == 0)
        insert_row(ec.cursor_y, "", 0);
    else {
        editor_row *row = &ec.row[ec.cursor_y];
        insert_row(ec.cursor_y + 1, &row->chars[ec.cursor_x],
                   row->size - ec.cursor_x);
        row = &ec.row[ec.cursor_y];
        row->size = ec.cursor_x;
        row->chars[row->size] = '\0';
        update_row(row);
    }
    ec.cursor_y++;
    ec.cursor_x = 0;
}

void row_delete_char(editor_row *row, int at)
{
    if ((at < 0) || (at >= row->size))
        return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    update_row(row);
    ec.modified++;
}

void insert_char(int c)
{
    if (ec.cursor_y == ec.num_rows)
        insert_row(ec.num_rows, "", 0);
    row_insert_char(&ec.row[ec.cursor_y], ec.cursor_x, c);
    ec.cursor_x++;
}

void delete_char()
{
    if (ec.cursor_y == ec.num_rows)
        return;
    if (ec.cursor_x == 0 && ec.cursor_y == 0)
        return;
    editor_row *row = &ec.row[ec.cursor_y];
    if (ec.cursor_x > 0) {
        row_delete_char(row, ec.cursor_x - 1);
        ec.cursor_x--;
    } else {
        ec.cursor_x = ec.row[ec.cursor_y - 1].size;
        row_append(&ec.row[ec.cursor_y - 1], row->chars, row->size);
        delete_row(ec.cursor_y);
        ec.cursor_y--;
    }
}

char *rows_tostring(int *buf_len)
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

void open_file(char *file_name)
{
    free(ec.file_name);
    ec.file_name = strdup(file_name);
    select_highlight();
    FILE *file = fopen(file_name, "r+");
    if (!file)
        panic("Failed to open the file");
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
    ec.modified = 0;
}

void save_file()
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
            ec.modified = 0;
            if (len > 1000)
                set_status_message("%d KB written to disk", len / 1000);
            else
                set_status_message("%d B written to disk", len);
            return;
        }
        close(fd);
    }
    free(buf);
    set_status_message("Error: %s", strerror(errno));
}

void search_cb(char *query, int key)
{
    static int last_match = -1;
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
        last_match = -1;
        direction = 1;
        return;
    } else if ((key == ARROW_RIGHT) || (key == ARROW_DOWN))
        direction = 1;
    else if ((key == ARROW_LEFT) || (key == ARROW_UP)) {
        if (last_match == -1)
            return;
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }
    int current = last_match;
    for (int i = 0; i < ec.num_rows; i++) {
        current += direction;
        if (current == -1)
            current = ec.num_rows - 1;
        else if (current == ec.num_rows)
            current = 0;
        editor_row *row = &ec.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            ec.cursor_y = current;
            ec.cursor_x = row_renderx_to_cursorx(row, match - row->render);
            ec.row_offset = ec.num_rows;
            saved_highlight_line = current;
            saved_hightlight = malloc(row->render_size);
            memcpy(saved_hightlight, row->highlight, row->render_size);
            memset(&row->highlight[match - row->render], MATCH, strlen(query));
            break;
        }
    }
}

void search()
{
    int saved_cursor_x = ec.cursor_x;
    int saved_cursor_y = ec.cursor_y;
    int saved_col_offset = ec.col_offset;
    int saved_row_offset = ec.row_offset;
    char *query = prompt("Search: %s (ESC / Enter / Arrows)", search_cb);
    if (query)
        free(query);
    else {
        ec.cursor_x = saved_cursor_x;
        ec.cursor_y = saved_cursor_y;
        ec.col_offset = saved_col_offset;
        ec.row_offset = saved_row_offset;
    }
}

void buf_append(editor_buf *eb, const char *s, int len)
{
    char *new = realloc(eb->buf, eb->len + len);
    if (!new)
        return;
    memcpy(&new[eb->len], s, len);
    eb->buf = new;
    eb->len += len;
}

void buf_free(editor_buf *eb)
{
    free(eb->buf);
}

void scroll()
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

void draw_statusbar(editor_buf *eb)
{
    time_t now = time(NULL);
    struct tm *currtime;
    buf_append(eb, "\x1b[100m", 6); /* Dark gray */
    char status[80], r_status[80];
    currtime = localtime(&now);
    int len = snprintf(status, sizeof(status), "  File: %.20s %s",
                       ec.file_name ? ec.file_name : "< New >",
                       ec.modified ? "(modified)" : "");
    int col_size = ec.row &&ec.cursor_y <= ec.num_rows - 1
                       ? col_size = ec.row[ec.cursor_y].size
                       : 0;
    int r_len = snprintf(
        r_status, sizeof(r_status), "%d/%d lines  %d/%d cols [ %2d:%2d:%2d ]",
        (ec.cursor_y + 1 > ec.num_rows) ? ec.num_rows : ec.cursor_y + 1,
        ec.num_rows, (ec.cursor_x + 1 > col_size) ? col_size : ec.cursor_x + 1,
        col_size, currtime->tm_hour, currtime->tm_min, currtime->tm_sec);
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

void draw_messagebar(editor_buf *eb)
{
    buf_append(eb, "\x1b[93m\x1b[44m\x1b[K", 13);
    int msg_len = strlen(ec.status_msg);
    if (msg_len > ec.screen_cols)
        msg_len = ec.screen_cols;
    /* display for 5 seconds and then hide */
    if (msg_len && time(NULL) - ec.status_msg_time < 5)
        buf_append(eb, ec.status_msg, msg_len);
    buf_append(eb, "\x1b[0m", 4);
}

void set_status_message(const char *msg, ...)
{
    va_list args;
    va_start(args, msg);
    vsnprintf(ec.status_msg, sizeof(ec.status_msg), msg, args);
    va_end(args);
    ec.status_msg_time = time(NULL);
}

void draw_rows(editor_buf *eb)
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
            unsigned char *highlight =
                &ec.row[file_row].highlight[ec.col_offset];
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
                } else if (highlight[j] == NORMAL) {
                    if (current_color != -1) {
                        buf_append(eb, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    buf_append(eb, &c[j], 1);
                } else {
                    int color = token_to_color(highlight[j]);
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
            buf_append(eb, "\x1b[39m", 5);
        }
        buf_append(eb, "\x1b[K", 3);
        buf_append(eb, "\r\n", 2);
    }
}

void refresh_screen()
{
    scroll();
    editor_buf eb = {NULL, 0};
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

void handle_sigwinch()
{
    update_window_size();
    if (ec.cursor_y > ec.screen_rows)
        ec.cursor_y = ec.screen_rows - 1;
    if (ec.cursor_x > ec.screen_cols)
        ec.cursor_x = ec.screen_cols - 1;
    refresh_screen();
}

void handle_sigcont()
{
    disable_raw_mode();
    open_buffer();
    enable_raw_mode();
    refresh_screen();
}

char *prompt(char *msg, void (*callback)(char *, int))
{
    size_t buf_size = 128;
    char *buf = malloc(buf_size);
    size_t buf_len = 0;
    buf[0] = '\0';
    while (1) {
        set_status_message(msg, buf);
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
                buf = realloc(buf, buf_size);
            }
            buf[buf_len++] = c;
            buf[buf_len] = '\0';
        }
        if (callback)
            callback(buf, c);
    }
}

void move_cursor(int key)
{
    editor_row *row =
        (ec.cursor_y >= ec.num_rows) ? NULL : &ec.row[ec.cursor_y];
    switch (key) {
    case ARROW_LEFT:
        if (ec.cursor_x != 0)
            ec.cursor_x--;
        else if (ec.cursor_y > 0) {
            ec.cursor_y--;
            ec.cursor_x = ec.row[ec.cursor_y].size;
        }
        break;
    case ARROW_RIGHT:
        if (row && ec.cursor_x < row->size)
            ec.cursor_x++;
        else if (row && ec.cursor_x == row->size) {
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

void process_key()
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
        if (ec.modified &&
            !prompt("File has been modified. Type 'yes' and enter "
                    "to force quit (ESC to cancel)",
                    NULL))
            return;
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
        editor_row *row = &ec.row[ec.cursor_y];
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

void *refresh_thread(void *dummy)
{
    while (1) {
        refresh_screen();
        usleep(1000);
    }
    return NULL;
}

void init_editor()
{
    update_window_size();
    signal(SIGWINCH, handle_sigwinch);
    signal(SIGCONT, handle_sigcont);
}

int main(int argc, char *argv[])
{
    init_editor();
    if (argc >= 2)
        open_file(argv[1]);
    enable_raw_mode();
    set_status_message(
        "Mazu Editor | ^Q Exit | ^S Save | ^F Search | "
        "^C Copy | ^X Cut | ^V Paste");
    if (pthread_create(&(pthread_t){0}, NULL, &refresh_thread, NULL)) {
        perror("pthread_create");
        return 1;
    }
    while (1)
        process_key();
    /* not reachable */
    return 0;
}
