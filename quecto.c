/*** quecto.c - main editor file ***/

#include "quecto.h"

/*** global state ***/
Editor Q;

/*** prototypes ***/
static void screenRefresh(void);
static void statusSet(const char *fmt, ...);
static char *inputPrompt(const char *prompt);

/*** editor operations ***/

static void editorAddLine(int pos, char *s, int len) {
    if (pos < 0 || pos > Q.linecount) return;

    Q.lines = realloc(Q.lines, sizeof(Line) * (Q.linecount + 1));
    memmove(&Q.lines[pos + 1], &Q.lines[pos], sizeof(Line) * (Q.linecount - pos));

    Q.lines[pos].len = len;
    Q.lines[pos].text = malloc(len + 1);
    memcpy(Q.lines[pos].text, s, len);
    Q.lines[pos].text[len] = '\0';

    Q.linecount++;
    Q.modified = 1;
}

static void editorRemoveLine(int pos) {
    if (pos < 0 || pos >= Q.linecount) return;
    lineDestroy(&Q.lines[pos]);
    memmove(&Q.lines[pos], &Q.lines[pos + 1], sizeof(Line) * (Q.linecount - pos - 1));
    Q.linecount--;
    Q.modified = 1;
}

static void editorPutChar(int ch) {
    if (Q.cury == Q.linecount) {
        editorAddLine(Q.linecount, "", 0);
    }
    lineInsertCh(&Q.lines[Q.cury], Q.curx, ch);
    Q.curx++;
    Q.modified = 1;
}

static void editorNewLine(void) {
    if (Q.curx == 0) {
        editorAddLine(Q.cury, "", 0);
    } else {
        Line *ln = &Q.lines[Q.cury];
        editorAddLine(Q.cury + 1, &ln->text[Q.curx], ln->len - Q.curx);
        ln = &Q.lines[Q.cury];
        ln->len = Q.curx;
        ln->text[ln->len] = '\0';
    }
    Q.cury++;
    Q.curx = 0;
    Q.modified = 1;
}

static void editorBackspace(void) {
    if (Q.cury == Q.linecount) return;
    if (Q.curx == 0 && Q.cury == 0) return;

    Line *ln = &Q.lines[Q.cury];
    if (Q.curx > 0) {
        lineDeleteCh(ln, Q.curx - 1);
        Q.curx--;
        Q.modified = 1;
    } else {
        Q.curx = Q.lines[Q.cury - 1].len;
        lineJoin(&Q.lines[Q.cury - 1], ln->text, ln->len);
        editorRemoveLine(Q.cury);
        Q.cury--;
        Q.modified = 1;
    }
}

static void editorDelete(void) {
    if (Q.cury == Q.linecount) return;

    Line *ln = &Q.lines[Q.cury];
    if (Q.curx < ln->len) {
        lineDeleteCh(ln, Q.curx);
        Q.modified = 1;
    } else if (Q.cury + 1 < Q.linecount) {
        lineJoin(ln, Q.lines[Q.cury + 1].text, Q.lines[Q.cury + 1].len);
        editorRemoveLine(Q.cury + 1);
        Q.modified = 1;
    }
}

/*** file i/o ***/

static char *fileToString(int *outlen) {
    int total = 0;
    for (int i = 0; i < Q.linecount; i++) {
        total += Q.lines[i].len + 1;
    }
    *outlen = total;

    char *buf = malloc(total);
    if (buf == NULL) return NULL;
    
    char *p = buf;
    for (int i = 0; i < Q.linecount; i++) {
        memcpy(p, Q.lines[i].text, Q.lines[i].len);
        p += Q.lines[i].len;
        *p = '\n';
        p++;
    }

    return buf;
}

static void fileLoad(char *path) {
    free(Q.filepath);
    Q.filepath = strdup(path);

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char *buf = NULL;
    size_t bufcap = 0;
    ssize_t len;
    while ((len = getline(&buf, &bufcap, fp)) != -1) {
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            len--;
        editorAddLine(Q.linecount, buf, len);
    }
    free(buf);
    fclose(fp);
    Q.modified = 0;
}

static int fileSave(void) {
    if (Q.filepath == NULL) {
        Q.filepath = inputPrompt("Save as: %s");
        if (Q.filepath == NULL) {
            statusSet("");
            return 0;
        }
    }

    int len;
    char *buf = fileToString(&len);
    if (buf == NULL) {
        statusSet("Save failed: memory error");
        return 0;
    }

    int fd = open(Q.filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd != -1) {
        if (write(fd, buf, len) == len) {
            close(fd);
            free(buf);
            Q.modified = 0;
            statusSet("Saved %d bytes", len);
            return 1;
        }
        close(fd);
    }
    free(buf);
    statusSet("Save failed: %s", strerror(errno));
    return 0;
}

/*** output ***/

static void screenScroll(void) {
    int editrows = Q.termrows - 1;
    int currow = cursorScreenRow();
    
    if (currow < Q.offsety) {
        Q.offsety = currow;
    }
    if (currow >= Q.offsety + editrows) {
        Q.offsety = currow - editrows + 1;
    }
}

static void screenDrawContent(QBuf *qb) {
    int editrows = Q.termrows - 1;
    int contentwidth = Q.termcols - 1;
    if (contentwidth < 1) contentwidth = 1;
    
    int screenrow = 0;
    int filerow = 0;
    int wrapoffset = 0;
    
    /* Skip to scroll offset */
    int skiprows = Q.offsety;
    while (skiprows > 0 && filerow < Q.linecount) {
        int rows = lineScreenRows(&Q.lines[filerow], Q.termcols);
        if (skiprows < rows) {
            wrapoffset = skiprows;
            break;
        }
        skiprows -= rows;
        filerow++;
    }
    
    while (screenrow < editrows) {
        if (filerow >= Q.linecount) {
            /* Past EOF */
            if (Q.linecount == 0 && screenrow == 0) {
                qbufPush(qb, " ", 1);
            } else {
                qbufPush(qb, "~", 1);
            }
        } else {
            qbufPush(qb, " ", 1);
            
            Line *ln = &Q.lines[filerow];
            int startchar = wrapoffset * contentwidth;
            int col = 0;
            
            for (int i = startchar; i < ln->len && col < contentwidth; i++) {
                int c = ln->text[i];
                
                if (isCtrlChar(c)) {
                    qbufPush(qb, "\x1b[7m", 4);
                    char sym = ctrlSymbol(c);
                    if(sym == 'I') sym = ' ';
                    qbufPush(qb, &sym, 1);
                    qbufPush(qb, "\x1b[m", 3);
                } else {
                    qbufPush(qb, &ln->text[i], 1);
                }
                col++;
            }
            
            wrapoffset++;
            if (wrapoffset >= lineScreenRows(ln, Q.termcols)) {
                wrapoffset = 0;
                filerow++;
            }
        }
        
        qbufPush(qb, "\x1b[K", 3);
        qbufPush(qb, "\r\n", 2);
        screenrow++;
    }
}

static void screenDrawStatus(QBuf *qb) {
    qbufPush(qb, "\x1b[7m", 4);
    
    char left[256], right[64];
    int leftlen, rightlen;
    
    if (Q.message[0] != '\0') {
        /* Display message when set */
        leftlen = snprintf(left, sizeof(left), "%s", Q.message);
        rightlen = 0;
    } else {
        /* Default: filename and cursor position */
        leftlen = snprintf(left, sizeof(left), "%s%s",
                           Q.filepath ? Q.filepath : "[?]",
                           Q.modified ? "*" : "");
        rightlen = snprintf(right, sizeof(right), "%d,%d", Q.cury + 1, Q.curx + 1);
    }
    
    if (leftlen > Q.termcols) leftlen = Q.termcols;
    qbufPush(qb, left, leftlen);
    
    while (leftlen < Q.termcols) {
        if (rightlen > 0 && Q.termcols - leftlen == rightlen) {
            qbufPush(qb, right, rightlen);
            break;
        } else {
            qbufPush(qb, " ", 1);
            leftlen++;
        }
    }
    
    qbufPush(qb, "\x1b[m", 3);
}

static void screenRefresh(void) {
    screenScroll();

    QBuf qb;
    qbufInit(&qb);

    qbufPush(&qb, "\x1b[?25l", 6);
    qbufPush(&qb, "\x1b[H", 3);

    screenDrawContent(&qb);
    screenDrawStatus(&qb);

    int screeny = cursorScreenRow() - Q.offsety + 1;
    int screenx = cursorScreenCol() + 1;
    
    char pos[32];
    snprintf(pos, sizeof(pos), "\x1b[%d;%dH", screeny, screenx);
    qbufPush(&qb, pos, strlen(pos));

    qbufPush(&qb, "\x1b[?25h", 6);

    write(STDOUT_FILENO, qb.data, qb.len);
    qbufClear(&qb);
}

static void statusSet(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(Q.message, sizeof(Q.message), fmt, ap);
    va_end(ap);
}

/*** input ***/

static char *inputPrompt(const char *prompt) {
    int bufcap = 128;
    char *input = malloc(bufcap);
    if (input == NULL) return NULL;
    
    int len = 0;
    input[0] = '\0';

    while (1) {
        QBuf qb;
        qbufInit(&qb);
        
        qbufPush(&qb, "\x1b[?25l", 6);
        qbufPush(&qb, "\x1b[H", 3);
        
        screenDrawContent(&qb);
        
        qbufPush(&qb, "\x1b[7m", 4);
        
        char line[256];
        int linelen = snprintf(line, sizeof(line), prompt, input);
        if (linelen > Q.termcols) linelen = Q.termcols;
        qbufPush(&qb, line, linelen);
        
        while (linelen < Q.termcols) {
            qbufPush(&qb, " ", 1);
            linelen++;
        }
        qbufPush(&qb, "\x1b[m", 3);
        
        int promptbase = 0;
        const char *p = prompt;
        while (*p && *p != '%') { promptbase++; p++; }
        int cursorx = promptbase + len + 1;
        
        char pos[32];
        snprintf(pos, sizeof(pos), "\x1b[%d;%dH", Q.termrows, cursorx);
        qbufPush(&qb, pos, strlen(pos));
        
        qbufPush(&qb, "\x1b[?25h", 6);
        
        write(STDOUT_FILENO, qb.data, qb.len);
        qbufClear(&qb);

        int key = termReadKey();
        
        if (key == KEY_DELETE || key == CTRL_KEY('h') || key == KEY_BACKSPACE) {
            if (len > 0) input[--len] = '\0';
        } else if (key == '\x1b') {
            free(input);
            return NULL;
        } else if (key == '\r') {
            if (len > 0) return input;
            free(input);
            return NULL;
        } else if (!iscntrl(key) && key < 128) {
            if (len == bufcap - 1) {
                bufcap *= 2;
                char *newinput = realloc(input, bufcap);
                if (newinput == NULL) {
                    free(input);
                    return NULL;
                }
                input = newinput;
            }
            input[len++] = key;
            input[len] = '\0';
        }
    }
}

static int inputAsciiCode(void) {
    char *input = inputPrompt("ASCII: %s");
    if (input == NULL) return -1;
    
    int code = atoi(input);
    free(input);
    
    if (code < 0 || code > 127) return -1;
    return code;
}

static void cursorMove(int key) {
    Line *ln = (Q.cury < Q.linecount) ? &Q.lines[Q.cury] : NULL;

    switch (key) {
        case KEY_LEFT:
            if (Q.curx > 0) {
                Q.curx--;
            } else if (Q.cury > 0) {
                Q.cury--;
                Q.curx = Q.lines[Q.cury].len;
            }
            break;
        case KEY_RIGHT:
            if (ln && Q.curx < ln->len) {
                Q.curx++;
            } else if (ln && Q.curx == ln->len && Q.cury + 1 < Q.linecount) {
                Q.cury++;
                Q.curx = 0;
            }
            break;
        case KEY_UP:
            if (Q.cury > 0) {
                Q.cury--;
            }
            break;
        case KEY_DOWN:
            if (Q.cury < Q.linecount) {
                Q.cury++;
            }
            break;
    }

    ln = (Q.cury < Q.linecount) ? &Q.lines[Q.cury] : NULL;
    int maxcol = ln ? ln->len : 0;
    if (Q.curx > maxcol) {
        Q.curx = maxcol;
    }
}

static void handleKey(void) {
    /* Clear message from previous action */
    Q.message[0] = '\0';
    
    int key = termReadKey();

    switch (key) {
        case '\r':
            editorNewLine();
            break;

        case CTRL_KEY('q'):
            if (Q.modified && Q.confirmquit > 0) {
                statusSet("Unsaved changes! Press ^Q again to quit.");
                Q.confirmquit--;
                screenRefresh();
                return;
            }
            write(STDOUT_FILENO, "\x1b[0m", 4);
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('w'):
            fileSave();
            break;

        case CTRL_KEY('n'):
            {
                char *newpath = inputPrompt("New filename: %s");
                if (newpath) {
                    free(Q.filepath);
                    Q.filepath = newpath;
                    fileSave();
                }
            }
            break;

        case CTRL_KEY('k'):
            {
                int code = inputAsciiCode();
                if (code >= 0) {
                    editorPutChar(code);
                }
            }
            break;

        case CTRL_KEY('v'):
            statusSet("QUECTO v%s", Q_VERSION);
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
            break;

        case KEY_END:
            if (Q.cury < Q.linecount) {
                Q.curx = Q.lines[Q.cury].len;
            }
            break;

        case KEY_PGUP:
        case KEY_PGDOWN:
            {
                int times = Q.termrows - 1;
                while (times--) {
                    cursorMove(key == KEY_PGUP ? KEY_UP : KEY_DOWN);
                }
            }
            break;

        case KEY_UP:
        case KEY_DOWN:
        case KEY_LEFT:
        case KEY_RIGHT:
            cursorMove(key);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            if (!iscntrl(key) || key == '\t') {
                editorPutChar(key);
            }
            break;
    }

    Q.confirmquit = 1;
}

/*** init ***/

static void editorSetup(void) {
    Q.curx = 0;
    Q.cury = 0;
    Q.offsety = 0;
    Q.linecount = 0;
    Q.lines = NULL;
    Q.modified = 0;
    Q.filepath = NULL;
    Q.message[0] = '\0';
    Q.confirmquit = 1;

    if (termGetSize(&Q.termrows, &Q.termcols) == -1) {
        fatal("termGetSize");
    }
}

static void onResize(int sig) {
    (void)sig;
    if (termGetSize(&Q.termrows, &Q.termcols) == -1) {
        fatal("termGetSize");
    }
    screenRefresh();
}

int main(int argc, char *argv[]) {
    termSetupRaw();
    editorSetup();
    
    signal(SIGWINCH, onResize);

    if (argc >= 2) {
        fileLoad(argv[1]);
    }

    while (1) {
        screenRefresh();
        handleKey();
    }

    return 0;
}
