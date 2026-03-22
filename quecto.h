#ifndef QUECTO_H
#define QUECTO_H

/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>

/*** defines ***/
#define Q_VERSION "1.00"
#define CTRL_KEY(k) ((k) & 0x1f)

enum KeyCode {
    KEY_BACKSPACE = 127,
    KEY_LEFT = 1000,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_DELETE,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDOWN
};

/*** data structures ***/

/* Dynamic string buffer */
typedef struct {
    char *data;
    int len;
    int cap;
} QBuf;

/* Text line */
typedef struct {
    int len;
    char *text;
} Line;

/* Editor context */
typedef struct {
    int curx, cury;          /* Cursor position in file */
    int offsety;             /* Vertical scroll offset */
    int termrows;            /* Terminal height */
    int termcols;            /* Terminal width */
    int linecount;           /* Number of lines */
    Line *lines;             /* Array of lines */
    int modified;            /* Modified flag */
    char *filepath;          /* Current file path */
    char message[256];       /* Status message */
    int confirmquit;         /* Quit confirmation counter */
    struct termios savedterm; /* Saved terminal state */
} Editor;

/*** global editor ***/
extern Editor Q;

/*** terminal functions ***/
static void fatal(const char *msg) {
    write(STDOUT_FILENO, "\x1b[0m", 4);
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(msg);
    exit(1);
}

static void termRestore(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &Q.savedterm) == -1)
        fatal("tcsetattr");
}

static void termSetupRaw(void) {
    if (tcgetattr(STDIN_FILENO, &Q.savedterm) == -1) fatal("tcgetattr");
    atexit(termRestore);

    struct termios raw = Q.savedterm;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) fatal("tcsetattr");
}

static int termReadKey(void) {
    int n;
    char c;
    while ((n = read(STDIN_FILENO, &c, 1)) != 1) {
        if (n == -1 && errno != EAGAIN) fatal("read");
    }

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return KEY_HOME;
                        case '3': return KEY_DELETE;
                        case '4': return KEY_END;
                        case '5': return KEY_PGUP;
                        case '6': return KEY_PGDOWN;
                        case '7': return KEY_HOME;
                        case '8': return KEY_END;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return KEY_UP;
                    case 'B': return KEY_DOWN;
                    case 'C': return KEY_RIGHT;
                    case 'D': return KEY_LEFT;
                    case 'H': return KEY_HOME;
                    case 'F': return KEY_END;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
        }
        return '\x1b';
    }
    return c;
}

static int termGetCursor(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

static int termGetSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return termGetCursor(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** buffer operations ***/
static void qbufInit(QBuf *qb) {
    qb->data = NULL;
    qb->len = 0;
    qb->cap = 0;
}

static void qbufPush(QBuf *qb, const char *s, int len) {
    if (qb->len + len >= qb->cap) {
        int newcap = (qb->cap == 0) ? 256 : qb->cap * 2;
        while (newcap < qb->len + len + 1) newcap *= 2;
        char *newptr = realloc(qb->data, newcap);
        if (newptr == NULL) return;
        qb->data = newptr;
        qb->cap = newcap;
    }
    memcpy(&qb->data[qb->len], s, len);
    qb->len += len;
    qb->data[qb->len] = '\0';
}

static void qbufClear(QBuf *qb) {
    free(qb->data);
    qb->data = NULL;
    qb->len = 0;
    qb->cap = 0;
}

/*** line operations ***/
static void lineInsertCh(Line *ln, int pos, int ch) {
    if (pos < 0 || pos > ln->len) pos = ln->len;
    ln->text = realloc(ln->text, ln->len + 2);
    memmove(&ln->text[pos + 1], &ln->text[pos], ln->len - pos + 1);
    ln->len++;
    ln->text[pos] = ch;
}

static void lineDeleteCh(Line *ln, int pos) {
    if (pos < 0 || pos >= ln->len) return;
    memmove(&ln->text[pos], &ln->text[pos + 1], ln->len - pos);
    ln->len--;
}

static void lineJoin(Line *ln, char *s, int slen) {
    ln->text = realloc(ln->text, ln->len + slen + 1);
    memcpy(&ln->text[ln->len], s, slen);
    ln->len += slen;
    ln->text[ln->len] = '\0';
}

static void lineDestroy(Line *ln) {
    free(ln->text);
}

/*** utility ***/
static int isCtrlChar(int c) {
    return (c >= 0 && c < 32) || c == 127;
}

static char ctrlSymbol(int c) {
    if (c == 127) return '?';
    return c + '@';
}

/* Calculate how many screen rows a file line takes with soft wrap */
static int lineScreenRows(Line *ln, int termcols) {
    if (termcols <= 1) return 1;
    int contentwidth = termcols - 1;  /* Subtract leading space */
    if (ln->len == 0) return 1;
    return (ln->len + contentwidth - 1) / contentwidth;
}

/* Calculate total screen rows needed for all lines up to (not including) a line */
static int totalScreenRowsBefore(int fileline) {
    int total = 0;
    for (int i = 0; i < fileline && i < Q.linecount; i++) {
        total += lineScreenRows(&Q.lines[i], Q.termcols);
    }
    return total;
}

/* Get the screen row where the cursor should be displayed */
static int cursorScreenRow(void) {
    int row = totalScreenRowsBefore(Q.cury);
    
    if (Q.cury < Q.linecount) {
        int contentwidth = Q.termcols - 1;
        if (contentwidth < 1) contentwidth = 1;
        row += Q.curx / contentwidth;
    }
    
    return row;
}

/* Get the screen column where the cursor should be displayed */
static int cursorScreenCol(void) {
    int contentwidth = Q.termcols - 1;
    if (contentwidth < 1) contentwidth = 1;
    return (Q.curx % contentwidth) + 1;  /* +1 for leading space */
}

#endif /* QUECTO_H */