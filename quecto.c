/* quecto.c -- a very small terminal text editor.
 *
 * Text is stored as an array of lines. Every line is a plain byte buffer
 * holding UTF-8; the cursor is a byte offset into that buffer. All the
 * editing and movement code works on whole code points, never on single
 * bytes, so a Chinese character behaves like one character: it is drawn
 * two columns wide, the cursor skips over it in one step, and one press
 * of Backspace removes it.
 *
 * The screen is redrawn from scratch on every keystroke. That sounds
 * wasteful, but a full frame is only a few kilobytes and one write()
 * call, and it keeps the drawing code free of cleverness.
 */

#include "quecto.h"

Editor Q;

/* Declared up here because the file handling below needs the prompt and
   the prompt needs the drawing code, and C reads top to bottom. */
static void statusSet(const char *fmt, ...);
static char *inputPrompt(const char *prompt, const char *initial);
static void screenRefresh(void);

/* ------------------------------------------------------------------ */
/* memory                                                             */
/* ------------------------------------------------------------------ */

/* Allocation failures in an editor this small are fatal anyway; wrapping
   them here keeps the call sites free of error handling that could only
   ever print a message and exit. */
static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (p == NULL) {
        fprintf(stderr, "quecto: out of memory\n");
        exit(1);
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *np = realloc(p, n ? n : 1);
    if (np == NULL) {
        fprintf(stderr, "quecto: out of memory\n");
        exit(1);
    }
    return np;
}

/* ------------------------------------------------------------------ */
/* terminal                                                           */
/* ------------------------------------------------------------------ */

/* Put the terminal back the way we found it: attributes off, cursor
   visible again in case we died mid-frame, screen cleared. */
static void termLeave(void)
{
    static const char seq[] = "\x1b[0m\x1b[?25h\x1b[2J\x1b[H";

    (void)!write(STDOUT_FILENO, seq, sizeof seq - 1);
}

static void fatal(const char *msg)
{
    termLeave();
    perror(msg);
    exit(1);
}

static void termRestore(void)
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &Q.savedterm) == -1)
        fatal("tcsetattr");
}

/* Raw mode: no line buffering, no echo, no signals from the keyboard.
   Turning off ICRNL matters for correctness, not just taste -- otherwise
   Enter arrives as \n and every other control key is remapped. */
static void termSetupRaw(void)
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &Q.savedterm) == -1) fatal("tcgetattr");
    atexit(termRestore);

    raw = Q.savedterm;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) fatal("tcsetattr");
}

/* Decode the tail of a UTF-8 sequence whose first byte was already read.
   Returns the code point, or KEY_NONE if the sequence was cut short --
   a lone lead byte is not worth inserting into someone's file. */
static int termReadUtf8(unsigned char lead)
{
    unsigned char tail[3];
    int want, i, cp;

    if ((lead & 0xE0) == 0xC0) {
        want = 1;
        cp = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        want = 2;
        cp = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        want = 3;
        cp = lead & 0x07;
    } else {
        return KEY_NONE; /* stray continuation byte, drop it */
    }

    for (i = 0; i < want; i++) {
        if (read(STDIN_FILENO, &tail[i], 1) != 1) return KEY_NONE;
        if ((tail[i] & 0xC0) != 0x80) return KEY_NONE;
        cp = (cp << 6) | (tail[i] & 0x3F);
    }
    return cp;
}

/* Set by the SIGWINCH handler. The handler itself must not touch the
   screen: reallocating the frame buffer from inside a signal that may
   have interrupted a malloc() is a good way to corrupt the heap, so it
   raises a flag and the main loop does the work. */
static volatile sig_atomic_t gotResize = 0;

/* Read one key press and return either a code point or one of the
   KEY_* values above. */
static int termReadKey(void)
{
    unsigned char c;
    char seq[3];
    int n;

    /* VMIN=0 makes read() return on timeout; keep asking until a real
       byte shows up, or until a resize asks to be dealt with. */
    while ((n = read(STDIN_FILENO, &c, 1)) != 1) {
        if (gotResize) return KEY_NONE;
        if (n == -1 && errno != EAGAIN) fatal("read");
    }

    if (c == '\x1b') {
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

    if (c >= 0x80) return termReadUtf8(c);
    return c;
}

/* Ask the terminal where the cursor is. Only used as a fallback, when
   the window size ioctl is not available. */
static int termGetCursor(int *rows, int *cols)
{
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

static int termGetSize(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return termGetCursor(rows, cols);
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* ------------------------------------------------------------------ */
/* frame buffer                                                       */
/* ------------------------------------------------------------------ */

static void qbufInit(QBuf *qb)
{
    qb->data = NULL;
    qb->len = 0;
    qb->cap = 0;
}

static void qbufPush(QBuf *qb, const char *s, int len)
{
    if (qb->len + len + 1 > qb->cap) {
        int newcap = qb->cap ? qb->cap : 1024;
        while (newcap < qb->len + len + 1) newcap *= 2;
        qb->data = xrealloc(qb->data, (size_t)newcap);
        qb->cap = newcap;
    }
    memcpy(&qb->data[qb->len], s, (size_t)len);
    qb->len += len;
    qb->data[qb->len] = '\0';
}

static void qbufClear(QBuf *qb)
{
    free(qb->data);
    qb->data = NULL;
    qb->len = 0;
    qb->cap = 0;
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                              */
/* ------------------------------------------------------------------ */

/* Decode the sequence starting at s[i] (i < len). Returns its length in
   bytes and stores the code point in *cp.

   Anything that is not a well formed sequence is reported as a single
   byte -- the raw byte value -- rather than an error. Text files get
   truncated, copied and pasted together in all sorts of ways; showing a
   broken byte as one odd character is better than refusing to open the
   file or, worse, than skipping bytes and silently losing the user's
   data when the buffer is written back. */
static int utf8Decode(const char *s, int len, int i, int *cp)
{
    unsigned char c = (unsigned char)s[i];
    unsigned char cc;
    int n, k, code;

    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        n = 2;
        code = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
        code = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
        code = c & 0x07;
    } else {
        *cp = c; /* continuation byte or 0xFE/0xFF on its own */
        return 1;
    }

    if (i + n > len) {
        *cp = c;
        return 1;
    }
    for (k = 1; k < n; k++) {
        cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) {
            *cp = c;
            return 1;
        }
        code = (code << 6) | (cc & 0x3F);
    }
    *cp = code;
    return n;
}

/* Write a code point as UTF-8. Returns the number of bytes used, 1 to 4.
   Code points outside Unicode are clamped rather than rejected. */
static int utf8Encode(int cp, char *out)
{
    if (cp < 0) cp = 0;
    if (cp > 0x10FFFF) cp = 0xFFFD;

    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Walk back to the first byte of the character before `pos`.
   Callers keep the cursor on character boundaries, so this only has to
   skip the continuation bytes. */
static int utf8PrevPos(const char *s, int pos)
{
    int p = pos - 1;

    while (p > 0 && ((unsigned char)s[p] & 0xC0) == 0x80) p--;
    return p;
}

/* Characters that take up no room of their own: combining accents,
   zero width joiners, variation selectors and emoji skin tone
   modifiers. The list covers the ranges people actually run into; it is
   not the whole of Unicode's table, and adding to it is harmless. */
static int charIsZeroWidth(int cp)
{
    return (cp >= 0x0300 && cp <= 0x036F) ||   /* combining diacritics  */
           (cp >= 0x0483 && cp <= 0x0489) ||
           (cp >= 0x0591 && cp <= 0x05BD) ||   /* Hebrew points         */
           (cp == 0x05BF) || (cp == 0x05C1) || (cp == 0x05C2) ||
           (cp == 0x05C4) || (cp == 0x05C5) || (cp == 0x05C7) ||
           (cp >= 0x0610 && cp <= 0x061A) ||   /* Arabic marks          */
           (cp >= 0x064B && cp <= 0x065F) ||
           (cp >= 0x06D6 && cp <= 0x06DC) ||
           (cp >= 0x0900 && cp <= 0x0902) ||   /* Devanagari            */
           (cp == 0x093C) || (cp == 0x094D) ||
           (cp >= 0x0E31 && cp <= 0x0E3A) ||   /* Thai                  */
           (cp >= 0x0E47 && cp <= 0x0E4E) ||
           (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp == 0x200B) || (cp == 0x200C) || (cp == 0x200D) ||
           (cp >= 0x20D0 && cp <= 0x20FF) ||   /* combining for symbols */
           (cp >= 0xFE00 && cp <= 0xFE0F) ||   /* variation selectors   */
           (cp >= 0xFE20 && cp <= 0xFE2F) ||
           (cp >= 0x1F3FB && cp <= 0x1F3FF);   /* emoji skin tones      */
}

/* Characters drawn two columns wide: CJK ideographs and kana, Hangul,
   the fullwidth forms, and most emoji. Derived from the East Asian Width
   property; the ranges below are the ones that matter in practice. */
static int charIsWide(int cp)
{
    return (cp >= 0x1100 && cp <= 0x115F) ||   /* Hangul Jamo            */
           (cp >= 0x2E80 && cp <= 0x303E) ||   /* CJK radicals, punctuation */
           (cp >= 0x3041 && cp <= 0x33FF) ||   /* kana, CJK compatibility   */
           (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK extension A        */
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   /* CJK unified ideographs */
           (cp >= 0xA000 && cp <= 0xA4CF) ||   /* Yi                     */
           (cp >= 0xA960 && cp <= 0xA97F) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) ||   /* Hangul syllables       */
           (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK compatibility ideographs */
           (cp >= 0xFE10 && cp <= 0xFE19) ||   /* vertical forms         */
           (cp >= 0xFE30 && cp <= 0xFE6F) ||   /* CJK compatibility forms*/
           (cp >= 0xFF00 && cp <= 0xFF60) ||   /* fullwidth forms        */
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||
           (cp >= 0x1F300 && cp <= 0x1F64F) || /* emoji                  */
           (cp >= 0x1F900 && cp <= 0x1F9FF) ||
           (cp >= 0x20000 && cp <= 0x3FFFD);   /* CJK extensions B and up */
}

/* How many columns a character occupies on screen. Control characters
   are drawn as a single stand-in symbol, so they count as one. */
static int charWidth(int cp)
{
    if (cp < 32 || cp == 127) return 1;
    if (charIsZeroWidth(cp)) return 0;
    if (charIsWide(cp)) return 2;
    return 1;
}

/* Width of a whole byte string, counting characters rather than bytes. */
static int textCols(const char *s, int len)
{
    int i = 0, cols = 0, cp;

    while (i < len) {
        i += utf8Decode(s, len, i, &cp);
        cols += charWidth(cp);
    }
    return cols;
}

/* Copy as much of s into dst as fits in maxcols display columns without
   cutting a character in half. Returns the columns used. Used for the
   status bar and the prompt, where the text is user supplied and may be
   Chinese, Arabic or anything else. */
static int textFit(char *dst, int dstsize, const char *s, int maxcols)
{
    int len = (int)strlen(s);
    int i = 0, out = 0, used = 0, cp, n, w;

    if (dstsize < 1) return 0;
    while (i < len && out < dstsize - 1) {
        n = utf8Decode(s, len, i, &cp);
        w = charWidth(cp);
        if (used + w > maxcols) break;
        if (out + n > dstsize - 1) break;
        memcpy(&dst[out], &s[i], (size_t)n);
        out += n;
        used += w;
        i += n;
    }
    dst[out] = '\0';
    return used;
}

/* ------------------------------------------------------------------ */
/* layout                                                             */
/* ------------------------------------------------------------------ */

/* Width of the text area: the terminal less one column of left margin,
   which is what separates the text from the edge of the window. */
static int contentWidth(void)
{
    int w = Q.termcols - 1;
    return (w < 1) ? 1 : w;
}

/* Number of rows left for text once the status line is taken out. */
static int editRows(void)
{
    int r = Q.termrows - 1;
    return (r < 1) ? 1 : r;
}

/* Measure the character at s[i] as it would be placed at column `col` of
   a row `width` columns wide.

   Tabs are expanded to the next multiple of TAB_WIDTH, counted from the
   start of the row, which keeps columns lined up vertically whatever the
   text before the tab looks like. A character that does not fit in the
   space left on the row is reported with wrap set, meaning it belongs to
   the next row of the same line. Keeping this rule in one place is what
   makes the drawing code and the cursor code agree; when they disagree
   the cursor lands in the wrong cell, which is exactly the bug this file
   used to have with wide characters. */
static void glyphAt(const char *s, int len, int i, int width, int col,
                    int *cp, int *bytes, int *cols, int *wrap)
{
    *bytes = utf8Decode(s, len, i, cp);

    if (*cp == '\t') {
        *cols = TAB_WIDTH - (col % TAB_WIDTH);
        *wrap = (col + *cols > width);
        if (*wrap) *cols = TAB_WIDTH > width ? width : TAB_WIDTH;
    } else {
        *cols = charWidth(*cp);
        *wrap = (col + *cols > width);
        if (*wrap && *cols > width) *cols = width;
    }
}

/* Where the cursor sits when it is at byte offset `pos` of a line:
   *row is the wrapped row index from the start of the line, *col the
   display column inside that row.

   A position that lands exactly at the right edge moves to the start of
   the next row, the way a text cursor is expected to behave. */
static void lineCursorPos(const char *s, int len, int pos, int width,
                          int *row, int *col)
{
    int i = 0, r = 0, c = 0, cp, bytes, cols, wrap;

    while (i < pos) {
        glyphAt(s, len, i, width, c, &cp, &bytes, &cols, &wrap);
        if (wrap) {
            r++;
            c = 0;
        }
        c += cols;
        i += bytes;
    }
    if (c >= width) {
        r++;
        c = 0;
    }
    *row = r;
    *col = c;
}

/* How many screen rows a line occupies, cursor position included. */
static int lineScreenRows(const char *s, int len, int width)
{
    int row, col;

    lineCursorPos(s, len, len, width, &row, &col);
    return row + 1;
}

/* The byte offset whose character starts closest to display column
   `target`, without running past it, on the first row of the line.
   Used to keep the cursor in the same column while moving up and down
   through lines of different lengths. */
static int linePosAtCol(const char *s, int len, int target, int width)
{
    int i = 0, c = 0, cp, bytes, cols, wrap;

    if (target < 0) target = 0;
    while (i < len) {
        glyphAt(s, len, i, width, c, &cp, &bytes, &cols, &wrap);
        if (wrap || c + cols > target) break;
        c += cols;
        i += bytes;
    }
    return i;
}

/* Display column of the cursor, which is what the status bar reports and
   what a vertical move tries to preserve. */
static int cursorColumn(void)
{
    int row, col;
    Line *ln;

    if (Q.cury >= Q.linecount) return 0;
    ln = &Q.lines[Q.cury];
    lineCursorPos(ln->text, ln->len, Q.curx, contentWidth(), &row, &col);
    return col;
}

/* ------------------------------------------------------------------ */
/* lines                                                              */
/* ------------------------------------------------------------------ */

/* Refresh the cached row count of a line. Every function that changes a
   line has to call this, and so does anything that changes the window
   width. Forgetting it shows up as wrong scrolling rather than as a
   crash, so it is worth keeping the calls next to the memcpy. */
static void lineFix(Line *ln)
{
    ln->rows = lineScreenRows(ln->text, ln->len, contentWidth());
}

static void lineInsertBytes(Line *ln, int pos, const char *bytes, int n)
{
    if (pos < 0) pos = 0;
    if (pos > ln->len) pos = ln->len;

    ln->text = xrealloc(ln->text, (size_t)ln->len + n + 1);
    memmove(&ln->text[pos + n], &ln->text[pos], (size_t)(ln->len - pos + 1));
    memcpy(&ln->text[pos], bytes, (size_t)n);
    ln->len += n;
}

static void lineDeleteBytes(Line *ln, int pos, int n)
{
    if (pos < 0 || n <= 0 || pos + n > ln->len) return;
    memmove(&ln->text[pos], &ln->text[pos + n],
            (size_t)(ln->len - pos - n + 1));
    ln->len -= n;
}

static void lineAppend(Line *ln, const char *s, int slen)
{
    ln->text = xrealloc(ln->text, (size_t)ln->len + slen + 1);
    memcpy(&ln->text[ln->len], s, (size_t)slen);
    ln->len += slen;
    ln->text[ln->len] = '\0';
}

static void lineDestroy(Line *ln)
{
    free(ln->text);
    ln->text = NULL;
    ln->len = 0;
    ln->rows = 0;
}

/* ------------------------------------------------------------------ */
/* editing                                                            */
/* ------------------------------------------------------------------ */

static void editorAddLine(int pos, const char *s, int len)
{
    if (pos < 0 || pos > Q.linecount) return;

    Q.lines = xrealloc(Q.lines, sizeof(Line) * (size_t)(Q.linecount + 1));
    memmove(&Q.lines[pos + 1], &Q.lines[pos],
            sizeof(Line) * (size_t)(Q.linecount - pos));

    Q.lines[pos].len = len;
    Q.lines[pos].text = xmalloc((size_t)len + 1);
    memcpy(Q.lines[pos].text, s, (size_t)len);
    Q.lines[pos].text[len] = '\0';
    Q.lines[pos].rows = 1;

    Q.linecount++;
    lineFix(&Q.lines[pos]);
    Q.modified = 1;
}

static void editorRemoveLine(int pos)
{
    if (pos < 0 || pos >= Q.linecount) return;

    lineDestroy(&Q.lines[pos]);
    memmove(&Q.lines[pos], &Q.lines[pos + 1],
            sizeof(Line) * (size_t)(Q.linecount - pos - 1));
    Q.linecount--;
    Q.modified = 1;
}

/* Insert raw bytes at the cursor. The caller is responsible for passing
   whole characters. */
static void editorInsert(const char *bytes, int n)
{
    Line *ln;

    if (Q.cury >= Q.linecount) editorAddLine(Q.linecount, "", 0);
    if (Q.cury >= Q.linecount) return;

    ln = &Q.lines[Q.cury];
    lineInsertBytes(ln, Q.curx, bytes, n);
    Q.curx += n;
    lineFix(ln);
    Q.modified = 1;
}

static void editorPutChar(int cp)
{
    char buf[4];
    int n = utf8Encode(cp, buf);

    editorInsert(buf, n);
}

/* Split the current line at the cursor. */
static void editorNewLine(void)
{
    Line *ln;

    if (Q.cury >= Q.linecount) {
        editorAddLine(Q.linecount, "", 0);
        Q.curx = 0;
        return;
    }

    ln = &Q.lines[Q.cury];
    editorAddLine(Q.cury + 1, &ln->text[Q.curx], ln->len - Q.curx);
    ln = &Q.lines[Q.cury];
    ln->len = Q.curx;
    ln->text[ln->len] = '\0';
    lineFix(ln);

    Q.cury++;
    Q.curx = 0;
    Q.modified = 1;
}

/* Remove the character before the cursor. Multi-byte characters go as a
   whole, which is the whole point of tracking byte offsets carefully. */
static void editorBackspace(void)
{
    Line *ln;
    int start;

    if (Q.cury >= Q.linecount) return;
    if (Q.curx == 0 && Q.cury == 0) return;

    ln = &Q.lines[Q.cury];
    if (Q.curx > 0) {
        start = utf8PrevPos(ln->text, Q.curx);
        lineDeleteBytes(ln, start, Q.curx - start);
        Q.curx = start;
        lineFix(ln);
        Q.modified = 1;
    } else {
        int prevlen = Q.lines[Q.cury - 1].len;
        lineAppend(&Q.lines[Q.cury - 1], ln->text, ln->len);
        editorRemoveLine(Q.cury);
        Q.cury--;
        Q.curx = prevlen;
        lineFix(&Q.lines[Q.cury]);
    }
}

/* Remove the character under the cursor. */
static void editorDelete(void)
{
    Line *ln;
    int n, cp;

    if (Q.cury >= Q.linecount) return;
    ln = &Q.lines[Q.cury];

    if (Q.curx < ln->len) {
        n = utf8Decode(ln->text, ln->len, Q.curx, &cp);
        lineDeleteBytes(ln, Q.curx, n);
        lineFix(ln);
        Q.modified = 1;
    } else if (Q.cury + 1 < Q.linecount) {
        lineAppend(ln, Q.lines[Q.cury + 1].text, Q.lines[Q.cury + 1].len);
        editorRemoveLine(Q.cury + 1);
        lineFix(ln);
    }
}

/* Re-measure every line. Needed after the window width changes, since
   the cached row counts depend on it. */
static void editorFixAllRows(void)
{
    int i;

    for (i = 0; i < Q.linecount; i++) lineFix(&Q.lines[i]);
}

/* Put the cursor at the start of line `line`, counting from one. A line
   number past the end of the file lands on the last line rather than
   being refused: when you are guessing at a number, the nearest thing you
   can get is more useful than an error. */
static void editorGotoLine(long line)
{
    if (Q.linecount == 0) {
        Q.cury = 0;
        Q.curx = 0;
        return;
    }
    if (line < 1) line = 1;
    if (line > Q.linecount) line = Q.linecount;

    Q.cury = (int)line - 1;
    Q.curx = 0;
    Q.goalx = -1;
}

/* ------------------------------------------------------------------ */
/* search                                                             */
/* ------------------------------------------------------------------ */

/* A search is a plain substring match, done on bytes. UTF-8 makes that
   safe: a valid sequence can only occur on a character boundary, so a
   pattern never matches half a character by accident. */

static int matchAt(const char *hay, int i, const char *needle, int nlen,
                   int icase)
{
    int k;
    unsigned char a, b;

    for (k = 0; k < nlen; k++) {
        a = (unsigned char)hay[i + k];
        b = (unsigned char)needle[k];
        if (icase) {
            /* ASCII only, deliberately: tolower() on the bytes of a UTF-8
               sequence would corrupt them. */
            if (a < 128) a = (unsigned char)tolower(a);
            if (b < 128) b = (unsigned char)tolower(b);
        }
        if (a != b) return 0;
    }
    return 1;
}

/* Case matters only when the pattern says it does: "TODO" finds exactly
   "TODO", while "todo" finds "TODO" as well. Halfway between the two
   sensible defaults, and it costs four lines. */
static int hasUpper(const char *s)
{
    for (; *s != '\0'; s++)
        if (*s >= 'A' && *s <= 'Z') return 1;
    return 0;
}

/* Look for the pattern in lines first..last, starting at byte `from` in
   the first of them. Returns the line index, or -1. */
static int findMatch(int first, int last, int from, const char *needle,
                     int nlen, int icase, int *outx)
{
    int i, k;

    if (first < 0) first = 0;
    if (last >= Q.linecount) last = Q.linecount - 1;

    for (i = first; i <= last; i++) {
        Line *ln = &Q.lines[i];
        int start = (i == first && from > 0) ? from : 0;

        for (k = start; k + nlen <= ln->len; k++) {
            if (matchAt(ln->text, k, needle, nlen, icase)) {
                *outx = k;
                return i;
            }
        }
    }
    return -1;
}

/* Search forward from the cursor, wrapping round once at the end of the
   file. The cursor lands on the start of the match; nothing is changed in
   the buffer, so a search can be repeated or abandoned at no cost. */
static int editorFind(const char *needle)
{
    int nlen = (int)strlen(needle);
    int icase, line, x = 0, wrapped = 0;

    if (nlen == 0) {
        statusSet("Nothing to search for");
        return 0;
    }
    icase = !hasUpper(needle);

    line = findMatch(Q.cury, Q.linecount - 1, Q.curx + 1, needle, nlen,
                     icase, &x);
    if (line < 0) {
        line = findMatch(0, Q.cury, 0, needle, nlen, icase, &x);
        wrapped = 1;
    }
    if (line < 0) {
        statusSet("Not found: %s", needle);
        return 0;
    }

    Q.cury = line;
    Q.curx = x;
    Q.goalx = -1;
    statusSet(wrapped ? "Found line %d (wrapped)" : "Found line %d",
              line + 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/* files                                                              */
/* ------------------------------------------------------------------ */

/* Write until everything is written or the disk says no. A single write()
   is allowed to come up short, and ignoring that is how files end up
   truncated. */
static int writeAll(int fd, const char *buf, int len)
{
    int done = 0;
    ssize_t n;

    while (done < len) {
        n = write(fd, buf + done, (size_t)(len - done));
        if (n <= 0) return -1;
        done += (int)n;
    }
    return 0;
}

/* Load a file into the line array. A missing file is not an error; it is
   how you start a new document. */
static void fileLoad(const char *path)
{
    FILE *fp;
    char *buf = NULL;
    size_t bufcap = 0;
    ssize_t len;

    free(Q.filepath);
    Q.filepath = strdup(path);
    if (Q.filepath == NULL) Q.filepath = NULL;

    fp = fopen(path, "r");
    if (fp == NULL) {
        /* A missing file is simply a new file; anything else is worth
           saying out loud, because the save would fail later anyway. */
        if (errno != ENOENT)
            statusSet("Could not open %s: %s", path, strerror(errno));
        return;
    }

    while ((len = getline(&buf, &bufcap, fp)) != -1) {
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            len--;
        editorAddLine(Q.linecount, buf, (int)len);
    }
    free(buf);
    fclose(fp);

    Q.modified = 0;
}

/* Copy the buffer into an open descriptor, one line at a time. Writing
   the lines straight out means a large buffer never has to be copied into
   a second large buffer first. */
static int writeLines(int fd, int *total)
{
    int i;

    *total = 0;
    for (i = 0; i < Q.linecount; i++) {
        if (writeAll(fd, Q.lines[i].text, Q.lines[i].len) == -1 ||
            writeAll(fd, "\n", 1) == -1)
            return -1;
        *total += Q.lines[i].len + 1;
    }
    return 0;
}

/* Last resort: write over the file in place. This is not crash safe, but
   it is the only thing that works for a symlink (where a rename would
   replace the link instead of the file it points at) and for device
   nodes, pipes and the like. */
static int writeInPlace(const char *path)
{
    int fd, total = 0;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        statusSet("Save failed: %s", strerror(errno));
        return 0;
    }
    if (writeLines(fd, &total) == -1) {
        statusSet("Save failed: %s", strerror(errno));
        close(fd);
        return 0;
    }
    close(fd);

    Q.modified = 0;
    statusSet("Saved %d bytes", total);
    return 1;
}

/* Build the "<dir>/.<name>.XXXXXX" template mkstemp() wants. The
   temporary file has to sit in the same directory as the target, because
   rename() is only atomic within a single filesystem. Returns the length
   of the directory part, or -1 if the name does not fit the buffer. */
static int tempPath(char *buf, int size, const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash != NULL ? slash + 1 : path;
    int dirlen = slash != NULL ? (int)(slash - path) + 1 : 0;
    int n;

    n = snprintf(buf, (size_t)size, "%.*s.%s.XXXXXX", dirlen, path, base);
    if (n < 0 || n >= size) return -1;
    return dirlen;
}

/* Push the directory entry itself to disk. Without this the rename can
   still be lost by a crash a moment after a successful save: the file
   contents would be durable and the name pointing at them would not. */
static void syncDir(const char *tmp, int dirlen)
{
    char dir[4096];
    int fd;

    if (dirlen <= 0) {
        strcpy(dir, ".");
    } else {
        if (dirlen >= (int)sizeof dir) return;
        memcpy(dir, tmp, (size_t)dirlen);
        dir[dirlen] = '\0';
    }
    fd = open(dir, O_RDONLY);
    if (fd == -1) return;
    (void)!fsync(fd);
    close(fd);
}

/* The real save: write a temporary file beside the target, flush it,
   then rename it over the original.

   This is what makes a save atomic. Either the new file is complete and
   in place, or the old file is untouched -- there is no moment where the
   name points at a half-written file, which is what a plain truncate and
   write leaves behind if the power goes out. It is also why a swap file
   is not the answer: a swap file is a crash *recovery* journal (vim keeps
   one so a killed session can be resumed), not a way of writing safely.
   Quecto writes and renames, so it has nothing to recover. */
static int writeAtomic(const char *path, const struct stat *st)
{
    char tmp[4096];
    int dirlen, fd, total = 0;

    dirlen = tempPath(tmp, (int)sizeof tmp, path);
    if (dirlen < 0) {
        statusSet("Save failed: name too long");
        return 0;
    }

    fd = mkstemp(tmp);
    if (fd == -1) {
        statusSet("Save failed: %s", strerror(errno));
        return 0;
    }

    /* Give the new file the permissions, and if we are allowed the owner,
       of the file it replaces, so that saving does not quietly change
       either. A brand new file gets the usual 0666 less the umask,
       because mkstemp() is deliberately private. Both calls can fail for
       an ordinary user, and a wrong owner is better than no file. */
    if (st != NULL) {
        (void)!fchmod(fd, st->st_mode & 07777);
        (void)!fchown(fd, st->st_uid, st->st_gid);
    } else {
        mode_t mask = umask(0);
        umask(mask);
        (void)!fchmod(fd, 0666 & ~mask);
    }

    if (writeLines(fd, &total) == -1 || fsync(fd) == -1) {
        statusSet("Save failed: %s", strerror(errno));
        close(fd);
        unlink(tmp);
        return 0;
    }
    if (close(fd) == -1) {
        statusSet("Save failed: %s", strerror(errno));
        unlink(tmp);
        return 0;
    }

    if (rename(tmp, path) == -1) {
        statusSet("Save failed: %s", strerror(errno));
        unlink(tmp);
        return 0;
    }
    syncDir(tmp, dirlen);

    Q.modified = 0;
    statusSet("Saved %d bytes", total);
    return 1;
}

/* Save the buffer. Anything that is not a plain file is written through
   in place; anything else is replaced atomically. */
static int fileSave(void)
{
    struct stat st;
    int have;

    if (Q.filepath == NULL) {
        char *path = inputPrompt("Save as: %s", "");
        if (path == NULL || path[0] == '\0') {
            free(path);
            statusSet("Save cancelled");
            return 0;
        }
        Q.filepath = path;
    }

    have = (lstat(Q.filepath, &st) == 0);
    /* A symlink, a hardlink, a device node or a pipe is written through
       rather than replaced: renaming over any of them would quietly break
       the link. Only a plain, singly linked file gets the atomic path. */
    if (have && (!S_ISREG(st.st_mode) || st.st_nlink > 1))
        return writeInPlace(Q.filepath);
    return writeAtomic(Q.filepath, have ? &st : NULL);
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */

static void statusSet(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(Q.message, sizeof Q.message, fmt, ap);
    va_end(ap);
}

/* Scroll just enough that the cursor row stays visible. */
static void screenScroll(int currow)
{
    int rows = editRows();

    if (currow < Q.offsety) Q.offsety = currow;
    if (currow >= Q.offsety + rows) Q.offsety = currow - rows + 1;
    if (Q.offsety < 0) Q.offsety = 0;
}

/* Find the file line and the wrapped row inside it that belong at the
   top of the screen. */
static void screenTopLine(int *filerow, int *segment)
{
    int rows = Q.offsety;
    int i = 0;

    while (i < Q.linecount && rows >= Q.lines[i].rows) {
        rows -= Q.lines[i].rows;
        i++;
    }
    if (i >= Q.linecount) {
        rows = 0;
    }
    *filerow = i;
    *segment = rows;
}

/* Append one character to the frame. Tabs become spaces up to the next
   tab stop -- a tab is a horizontal jump, not a printable symbol, so
   showing it as ^I would be a lie about the layout.

   Control characters are drawn as a single stand-in symbol. Passing them
   through unchanged would let a file containing a raw escape sequence
   paint the terminal, which is both ugly and a security problem. */
static void screenDrawGlyph(QBuf *qb, const char *s, int i, int cp, int bytes,
                            int cols)
{
    char sym;

    if (cp == '\t') {
        while (cols-- > 0) qbufPush(qb, " ", 1);
    } else if (cp < 32 || cp == 127) {
        sym = (cp == 127) ? '?' : (char)(cp + '@');
        qbufPush(qb, &sym, 1);
    } else {
        qbufPush(qb, &s[i], bytes);
    }
}

/* Draw the text area. The visible rows are produced in a single pass
   over the file: the byte offset into the current line is carried from
   one screen row to the next, so a long line is only ever measured once
   even when it fills the whole window. */
static void screenDrawContent(QBuf *qb)
{
    int rows = editRows();
    int width = contentWidth();
    int filerow, segment, screenrow = 0;
    int i = 0, row = 0, col = 0, cp, bytes, cols, wrap;

    screenTopLine(&filerow, &segment);

    /* Walk past the wrapped rows scrolled off the top of the window. */
    if (filerow < Q.linecount) {
        Line *ln = &Q.lines[filerow];
        while (i < ln->len && row < segment) {
            glyphAt(ln->text, ln->len, i, width, col, &cp, &bytes, &cols,
                    &wrap);
            if (wrap) {
                row++;
                col = 0;
                if (row >= segment) break;
            }
            col += cols;
            i += bytes;
        }
    }

    while (screenrow < rows) {
        if (filerow >= Q.linecount) {
            /* Past the end of the file: blank window, or a column of
               tildes like every other terminal editor. */
            if (Q.linecount == 0 && screenrow == 0)
                qbufPush(qb, " ", 1);
            else
                qbufPush(qb, "~", 1);
        } else {
            Line *ln = &Q.lines[filerow];

            qbufPush(qb, " ", 1); /* left margin */
            col = 0;
            while (i < ln->len) {
                glyphAt(ln->text, ln->len, i, width, col, &cp, &bytes, &cols,
                        &wrap);
                if (wrap) break; /* continues on the next screen row */
                screenDrawGlyph(qb, ln->text, i, cp, bytes, cols);
                col += cols;
                i += bytes;
            }
            /* The line is finished and its last row was not full: the
               next screen row starts the next line. If the last row was
               full, the next screen row is the empty tail of this line,
               which is where the cursor goes when it sits at the end. */
            if (i >= ln->len && col < width) {
                filerow++;
                i = 0;
            }
        }
        qbufPush(qb, "\x1b[K\r\n", 5);
        screenrow++;
    }
}

/* The status bar: file name on the left, cursor position on the right,
   or a message in place of both. */
static void screenDrawStatus(QBuf *qb)
{
    char left[256], right[64];
    char name[300];
    int lcols = 0, rcols = 0, pad;

    qbufPush(qb, "\x1b[7m", 4);

    if (Q.message[0] != '\0') {
        lcols = textFit(left, (int)sizeof left, Q.message, Q.termcols);
    } else {
        snprintf(name, sizeof name, "%s%s",
                 Q.filepath ? Q.filepath : "[Untitled]",
                 Q.modified ? " *" : "");
        lcols = textFit(left, (int)sizeof left, name, Q.termcols);
        rcols = snprintf(right, sizeof right, "%d,%d", Q.cury + 1,
                         cursorColumn() + 1);
        if (rcols < 0) rcols = 0;
        if (lcols + rcols > Q.termcols) rcols = 0;
    }

    qbufPush(qb, left, (int)strlen(left));
    for (pad = Q.termcols - lcols - rcols; pad > 0; pad--)
        qbufPush(qb, " ", 1);
    if (rcols > 0) qbufPush(qb, right, rcols);

    qbufPush(qb, "\x1b[m", 3);
}

/* Screen position of the cursor: absolute row from the top of the file,
   and the display column. */
static void cursorScreenPos(int *outrow, int *outcol)
{
    int i, r = 0, c = 0;

    for (i = 0; i < Q.cury && i < Q.linecount; i++) r += Q.lines[i].rows;

    if (Q.cury < Q.linecount) {
        Line *ln = &Q.lines[Q.cury];
        int row, col;
        lineCursorPos(ln->text, ln->len, Q.curx, contentWidth(), &row, &col);
        r += row;
        c = col;
    }
    *outrow = r;
    *outcol = c;
}

static void screenRefresh(void)
{
    QBuf qb;
    char pos[32];
    int currow, curcol, screeny, screenx, n;

    cursorScreenPos(&currow, &curcol);
    screenScroll(currow);

    qbufInit(&qb);

    qbufPush(&qb, "\x1b[?25l", 6); /* hide the cursor while drawing */
    qbufPush(&qb, "\x1b[H", 3);

    screenDrawContent(&qb);
    screenDrawStatus(&qb);

    screeny = currow - Q.offsety + 1;
    screenx = curcol + 2; /* one margin column, one for 1-based counting */
    n = snprintf(pos, sizeof pos, "\x1b[%d;%dH", screeny, screenx);
    qbufPush(&qb, pos, n);

    qbufPush(&qb, "\x1b[?25h", 6);

    if (write(STDOUT_FILENO, qb.data, (size_t)qb.len) == -1) {
        qbufClear(&qb);
        fatal("write");
    }
    qbufClear(&qb);
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

/* Read a line of text in the status bar. Returns a malloc'd string, or
   NULL if the user changed their mind. Backspace removes a whole
   character, so a Chinese file name can be typed and corrected.

   `initial` pre-fills the line with the cursor at the end, which is what
   makes repeating a search a two-key operation. */
static char *inputPrompt(const char *prompt, const char *initial)
{
    char *input = xmalloc(128);
    int cap = 128, len;
    char head[64];
    const char *p = prompt;
    int headlen = 0, headcols, i, key, n;

    len = (int)strlen(initial);
    if (len + 1 > cap) {
        cap = len + 1;
        input = xrealloc(input, (size_t)cap);
    }
    memcpy(input, initial, (size_t)len + 1);

    while (*p != '\0' && *p != '%' && headlen < (int)sizeof head - 1)
        head[headlen++] = *p++;
    head[headlen] = '\0';
    headcols = textCols(head, headlen);

    while (1) {
        QBuf qb;
        char line[300], fit[300];
        int cols;

        qbufInit(&qb);
        qbufPush(&qb, "\x1b[?25l", 6);
        qbufPush(&qb, "\x1b[H", 3);
        screenDrawContent(&qb);

        qbufPush(&qb, "\x1b[7m", 4);
        snprintf(line, sizeof line, prompt, input);
        cols = textFit(fit, (int)sizeof fit, line, Q.termcols);
        qbufPush(&qb, fit, (int)strlen(fit));
        for (i = cols; i < Q.termcols; i++) qbufPush(&qb, " ", 1);
        qbufPush(&qb, "\x1b[m", 3);

        n = snprintf(line, sizeof line, "\x1b[%d;%dH", Q.termrows,
                     headcols + textCols(input, len) + 1);
        qbufPush(&qb, line, n);
        qbufPush(&qb, "\x1b[?25h", 6);

        if (write(STDOUT_FILENO, qb.data, (size_t)qb.len) == -1) {
            qbufClear(&qb);
            fatal("write");
        }
        qbufClear(&qb);

        key = termReadKey();

        if (key == KEY_BACKSPACE || key == CTRL_KEY('h') || key == KEY_DELETE) {
            if (len > 0) {
                len = utf8PrevPos(input, len);
                input[len] = '\0';
            }
        } else if (key == '\x1b') {
            free(input);
            return NULL;
        } else if (key == '\r' || key == '\n') {
            /* An empty answer is still an answer -- repeating a search
               needs one. Callers that cannot use it treat "" as a
               cancel. */
            return input;
        } else if (key > 32 && key != 127) {
            char buf[4];
            int blen = utf8Encode(key, buf);
            if (len + blen + 1 > cap) {
                cap *= 2;
                input = xrealloc(input, (size_t)cap);
            }
            memcpy(&input[len], buf, (size_t)blen);
            len += blen;
            input[len] = '\0';
        }
    }
}

/* Ask for a code point and insert it. Ctrl+K followed by 0x4e2d puts a
   character into the file without needing an input method. */
static void inputCodePoint(void)
{
    char *text = inputPrompt("Code point: %s", "");
    char *end;
    long cp;

    if (text == NULL || text[0] == '\0') {
        free(text);
        return;
    }

    errno = 0;
    cp = strtol(text, &end, 0);
    free(text);

    if (errno != 0 || end == NULL || cp < 0 || cp > 0x10FFFF) {
        statusSet("Not a valid code point");
        return;
    }
    editorPutChar((int)cp);
}

/* Move the cursor, working in whole characters.
 *
 * Vertical moves remember the column they started from in Q.goalx, so
 * passing through a short line on the way does not drag the cursor to
 * the left for good. */
/* Ask which line to go to. A leading + or - counts from the current
   line, so "+10" is ten lines down, which is the one relative form people
   actually use. */
static void inputGoto(void)
{
    char *text = inputPrompt("Go to line: %s", "");
    char *end;
    long line;
    int relative = 0;

    if (text == NULL || text[0] == '\0') {
        free(text);
        return;
    }

    if (text[0] == '+' || text[0] == '-') relative = 1;

    errno = 0;
    line = strtol(text, &end, 10);
    if (end == text || errno != 0) {
        statusSet("Not a line number: %s", text);
        free(text);
        return;
    }
    free(text);

    if (relative) line += Q.cury + 1;
    editorGotoLine(line);
}

/* Ask for a search string and jump to the first match after the cursor.
   An empty answer repeats the previous search, so Ctrl+F followed by
   Enter is "find next" and typing something new costs nothing extra. */
static void inputFind(void)
{
    char *text = inputPrompt("Find: %s", "");
    const char *needle;

    if (text == NULL) return;

    if (text[0] == '\0') {
        if (Q.findtext[0] == '\0') {
            free(text);
            statusSet("Nothing to search for");
            return;
        }
        needle = Q.findtext;
    } else {
        snprintf(Q.findtext, sizeof Q.findtext, "%s", text);
        needle = Q.findtext;
    }

    editorFind(needle);
    free(text);
}

static void cursorMove(int key)
{
    Line *ln = (Q.cury < Q.linecount) ? &Q.lines[Q.cury] : NULL;
    int goal, cp;

    switch (key) {
    case KEY_LEFT:
        if (ln != NULL && Q.curx > 0) {
            Q.curx = utf8PrevPos(ln->text, Q.curx);
        } else if (Q.cury > 0) {
            Q.cury--;
            Q.curx = Q.lines[Q.cury].len;
        }
        Q.goalx = -1;
        break;

    case KEY_RIGHT:
        if (ln != NULL && Q.curx < ln->len) {
            Q.curx += utf8Decode(ln->text, ln->len, Q.curx, &cp);
        } else if (ln != NULL && Q.cury + 1 < Q.linecount) {
            Q.cury++;
            Q.curx = 0;
        }
        Q.goalx = -1;
        break;

    case KEY_UP:
    case KEY_DOWN:
        goal = (Q.goalx >= 0) ? Q.goalx : cursorColumn();
        if (key == KEY_UP) {
            if (Q.cury > 0) Q.cury--;
        } else {
            if (Q.cury < Q.linecount) Q.cury++;
        }
        Q.curx = 0;
        if (Q.cury < Q.linecount) {
            Line *t = &Q.lines[Q.cury];
            Q.curx = linePosAtCol(t->text, t->len, goal, contentWidth());
        }
        Q.goalx = goal;
        break;
    }
}

static void handleKey(void)
{
    int key;

    Q.message[0] = '\0';
    key = termReadKey();

    if (key == KEY_NONE) return;

    switch (key) {
    case '\r':
    case '\n':
        editorNewLine();
        break;

    case CTRL_KEY('q'):
        if (Q.modified && !Q.confirmquit) {
            Q.confirmquit = 1;
            statusSet("Unsaved changes. Press ^Q again to discard them.");
            return;
        }
        termLeave();
        exit(0);
        break;

    case CTRL_KEY('w'):
        fileSave();
        break;

    case CTRL_KEY('n'): {
        char *path = inputPrompt("New filename: %s", "");
        if (path != NULL && path[0] == '\0') {
            free(path);
            path = NULL;
        }
        if (path != NULL) {
            free(Q.filepath);
            Q.filepath = path;
            fileSave();
        }
        break;
    }

    case CTRL_KEY('k'):
        inputCodePoint();
        break;

    case CTRL_KEY('g'):
        inputGoto();
        break;

    case CTRL_KEY('f'):
        inputFind();
        break;

    case CTRL_KEY('v'):
        statusSet("quecto %s", Q_VERSION);
        break;

    case KEY_BACKSPACE:
    case CTRL_KEY('h'):
        editorBackspace();
        break;

    case KEY_DELETE:
        editorDelete();
        break;

    case KEY_HOME:
        Q.curx = 0;
        Q.goalx = -1;
        break;

    case KEY_END:
        if (Q.cury < Q.linecount) Q.curx = Q.lines[Q.cury].len;
        Q.goalx = -1;
        break;

    case KEY_PGUP:
    case KEY_PGDOWN: {
        int times = editRows();
        while (times-- > 0) cursorMove(key == KEY_PGUP ? KEY_UP : KEY_DOWN);
        break;
    }

    case KEY_UP:
    case KEY_DOWN:
    case KEY_LEFT:
    case KEY_RIGHT:
        cursorMove(key);
        break;

    case CTRL_KEY('l'): /* redraw, which is what every frame does */
    case '\x1b':
        break;

    case '\t':
        editorPutChar('\t');
        break;

    default:
        /* Everything else that is printable is text, including every
           kind of non-ASCII character the terminal hands over. */
        if (key >= 32 && key != 127) editorPutChar(key);
        break;
    }

    /* Anything but a second Ctrl+Q means the user is still working. */
    Q.confirmquit = 0;
}

/* ------------------------------------------------------------------ */
/* startup                                                            */
/* ------------------------------------------------------------------ */

static void editorSetup(void)
{
    Q.curx = 0;
    Q.cury = 0;
    Q.goalx = -1;
    Q.offsety = 0;
    Q.linecount = 0;
    Q.lines = NULL;
    Q.modified = 0;
    Q.filepath = NULL;
    Q.message[0] = '\0';
    Q.findtext[0] = '\0';
    Q.confirmquit = 0;

    if (termGetSize(&Q.termrows, &Q.termcols) == -1) fatal("termGetSize");
}

static void onResize(int sig)
{
    (void)sig;
    gotResize = 1;
}

/* A new window width changes how every line wraps, so the cached row
   counts have to be rebuilt before the next frame is drawn. */
static void handleResize(void)
{
    gotResize = 0;
    if (termGetSize(&Q.termrows, &Q.termcols) == -1) fatal("termGetSize");
    editorFixAllRows();
}

static void usage(FILE *out)
{
    fprintf(out,
            "usage: quecto [options] [file]\n"
            "\n"
            "  -v, --version   print version and exit\n"
            "  -h, --help      print this help and exit\n"
            "\n"
            "Keys: ^W save, ^N save as, ^Q quit, ^F find, ^G go to line,\n"
            "      ^K insert code point, ^V version,\n"
            "      arrows/Home/End/PageUp/PageDown to move.\n"
            "See quecto(1) for the full manual.\n");
}

int main(int argc, char *argv[])
{
    const char *file = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("quecto %s\n", Q_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (file != NULL) {
            usage(stderr);
            return 1;
        }
        file = argv[i];
    }

    termSetupRaw();
    editorSetup();
    signal(SIGWINCH, onResize);

    if (file != NULL) fileLoad(file);

    for (;;) {
        if (gotResize) handleResize();
        screenRefresh();
        handleKey();
    }
    return 0;
}
