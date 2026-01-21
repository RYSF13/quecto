/*
 * Quecto - A minimalist, modal text editor for Linux
 *
 * Author: Robert Ryan
 * License: MIT
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <regex.h>
#include <stdarg.h>
#include <fcntl.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define Q_VERSION "1.1"
#define TAB_STOP 4

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000, ARROW_RIGHT, ARROW_UP, ARROW_DOWN,
  DEL_KEY, HOME_KEY, END_KEY, PAGE_UP, PAGE_DOWN
};

/* Check if byte is a UTF-8 continuation byte (0b10xxxxxx) */
int is_utf8_continuation(char c) {
    return (c & 0xC0) == 0x80;
}

typedef struct erow {
  int size;
  char *chars;
} erow;

struct editorConfig {
  int cx, cy;
  int rx;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  int dirty;
  char *filename;
  char statusmsg[80];
  struct termios orig_termios;
};

struct editorConfig E;

/* Terminal handling */

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[0m", 4); /* Reset colors first */
  write(STDOUT_FILENO, "\x1b[2J", 4); /* Then clear screen */
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);
  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
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
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    }
    return '\x1b';
  }
  return c;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) return -1;
  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
}

/* Row operations */

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->chars);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  E.dirty++;
}

void editorRowDelBytes(erow *row, int at, int len) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + len], row->size - at - len);
  row->size -= len;
  E.dirty++;
}

/* Editor operations */

void editorInsertChar(int c) {
  if (E.cy == E.numrows) editorInsertRow(E.numrows, "", 0);
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline() {
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
  }
  E.cy++;
  E.cx = 0;
}

void editorDelChar() {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;
  if (E.cx > 0) {
    int delete_len = 1;
    /* Backtrack to find the start of the UTF-8 character */
    while (E.cx - delete_len > 0 && is_utf8_continuation(E.row[E.cy].chars[E.cx - delete_len])) {
        delete_len++;
    }
    editorRowDelBytes(&E.row[E.cy], E.cx - delete_len, delete_len);
    E.cx -= delete_len;
  } else {
    /* Merge with previous line */
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], E.row[E.cy].chars, E.row[E.cy].size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/* File I/O */

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++) totlen += E.row[j].size + 1;
  *buflen = totlen;
  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);
  FILE *fp = fopen(filename, "r");
  if (!fp) return; 
  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL) return; 
  int len;
  char *buf = editorRowsToString(&len);
  FILE *fp = fopen(E.filename, "w");
  if (fp) {
    if (ftruncate(fileno(fp), len) != -1) {
      if (fwrite(buf, len, 1, fp) == 1) {
        fclose(fp);
        free(buf);
        E.dirty = 0;
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Saved to %s", E.filename);
        return;
      }
    }
    fclose(fp);
  }
  free(buf);
  snprintf(E.statusmsg, sizeof(E.statusmsg), "Error: I/O error");
}

/* Regex and Command Logic */

void editorRegexReplace(char *pattern, char *repl, int global) {
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Error: Invalid Regex");
        return;
    }
    int count = 0;
    int i;
    for(i=0; i<E.numrows; i++) {
        erow *row = &E.row[i];
        regmatch_t match;
        int offset = 0;
        while(regexec(&regex, row->chars + offset, 1, &match, 0) == 0) {
            int start = offset + match.rm_so;
            int end = offset + match.rm_eo;
            int len_match = end - start;
            int len_repl = strlen(repl);
            int len_diff = len_repl - len_match;
            
            row->chars = realloc(row->chars, row->size + len_diff + 1);
            memmove(row->chars + end + len_diff, row->chars + end, row->size - end + 1);
            memcpy(row->chars + start, repl, len_repl);
            row->size += len_diff;
            
            offset = start + len_repl;
            count++;
            E.dirty++;
            if (!global) break; 
        }
        if(!global && count > 0) break; 
    }
    regfree(&regex);
    snprintf(E.statusmsg, sizeof(E.statusmsg), "Replaced %d occurrences", count);
}

void editorExit() {
    write(STDOUT_FILENO, "\x1b[0m", 4); /* Reset colors first */
    write(STDOUT_FILENO, "\x1b[2J", 4); /* Then clear screen */
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
}

void editorProcessCommand(char *cmd) {
    if (cmd[0] == '\0') return;
    if (strcmp(cmd, "q") == 0) {
        if (E.dirty) { snprintf(E.statusmsg, sizeof(E.statusmsg), "Unsaved changes!"); return; }
        editorExit();
    }
    if (strcmp(cmd, "q!") == 0) editorExit();
    if (strcmp(cmd, "w") == 0) { editorSave(); return; }
    if (strcmp(cmd, "wq") == 0) { editorSave(); editorExit(); }
    
    if (cmd[0] == 'r' && cmd[1] == '/') {
        char *pat = cmd + 2;
        char *repl = NULL;
        char *flags = NULL;
        char *end_pat = strchr(pat, '/');
        if(end_pat) {
            *end_pat = '\0'; 
            repl = end_pat + 1;
            char *end_repl = strchr(repl, '/');
            if(end_repl) {
                *end_repl = '\0';
                flags = end_repl + 1;
            }
        }
        if(pat && repl) {
            int g = (flags && strchr(flags, 'g')) ? 1 : 0;
            editorRegexReplace(pat, repl, g);
        }
    }
}

char *editorPrompt(char *prompt) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';
  while(1) {
      /* We just reset to ensure clean slate */
      write(STDOUT_FILENO, "\x1b[0m", 4); 
      
      char pos[32];
      snprintf(pos, sizeof(pos), "\x1b[%d;1H", E.screenrows + 1);
      write(STDOUT_FILENO, pos, strlen(pos));
      write(STDOUT_FILENO, "\x1b[2K", 4);
      write(STDOUT_FILENO, prompt, strlen(prompt));
      write(STDOUT_FILENO, buf, buflen);
      
      /* No color code needed, use terminal default */
      
      int c = editorReadKey();
      if (c == BACKSPACE || c == 127 || c == CTRL_KEY('h')) {
          if (buflen != 0) buf[--buflen] = '\0';
      } else if (c == '\x1b') {
          free(buf); 
          snprintf(E.statusmsg, sizeof(E.statusmsg), "");
          return NULL;
      } else if (c == '\r') {
          if (buflen != 0) {
             snprintf(E.statusmsg, sizeof(E.statusmsg), "");
             return buf;
          }
      } else if (!iscntrl(c) && c < 128) {
          if (buflen == bufsize - 1) { bufsize *= 2; buf = realloc(buf, bufsize); }
          buf[buflen++] = c;
          buf[buflen] = '\0';
      }
  }
}

/* Rendering */

struct abuf { char *b; int len; };
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}
void abFree(struct abuf *ab) { free(ab->b); }

/* Convert Byte Index (cx) to Render Index (rx) */
int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; ) {
    unsigned char c = row->chars[j];
    if (c == '\t') {
      rx += (TAB_STOP - (rx % TAB_STOP));
      j++;
    } else if ((c & 0xE0) == 0xC0) { 
        /* 2-byte char (e.g. Cyrillic), usually width 1 */
        rx += 1; j += 2;
    } else if ((c & 0xF0) == 0xE0) { 
        /* 3-byte char (CJK), width 2 */
        rx += 2; j += 3;
    } else if ((c & 0xF8) == 0xF0) { 
        /* 4-byte char (Emoji), width 2 */
        rx += 2; j += 4;
    } else {
        /* ASCII or others */
        rx += 1; j++;
    }
    /* Safety break if malformed utf-8 causes j to jump past cx */
    if (j > row->size) break; 
  }
  return rx;
}

void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;
  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);
  
  /* Reset color to default */
  abAppend(&ab, "\x1b[0m", 4);

  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    
    if (filerow < E.numrows) {
      /* CHANGE: Padding space ONLY added for actual text rows */
      abAppend(&ab, " ", 1); 

      int len = E.row[filerow].size - E.coloff;
      if (len < 0) len = 0;
      /* Max width adjustment due to the leading space */
      if (len > E.screencols - 1) len = E.screencols - 1; 
      
      /* Render loop handling control chars */
      char *c = &E.row[filerow].chars[E.coloff];
      int j;
      for (j = 0; j < len; j++) {
          if (iscntrl(c[j]) && c[j] != '\t') { 
              char sym = (c[j] <= 26) ? '@' + c[j] : '?';
              abAppend(&ab, "\x1b[7m", 4);
              abAppend(&ab, &sym, 1);
              abAppend(&ab, "\x1b[m", 3);
          } else {
              abAppend(&ab, &c[j], 1);
          }
      }
    } else {
      /* CHANGE: No padding for tilde lines */
      abAppend(&ab, "~", 1); 
    }
    
    abAppend(&ab, "\x1b[K", 3);
    abAppend(&ab, "\r\n", 2);
  }

  /* Status Bar rendering (No color, just text) */
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s%s",
    E.filename ? E.filename : "[New]", E.dirty ? "*" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d,%d", E.cy + 1, E.cx + 1);
  if (len > E.screencols) len = E.screencols;
  abAppend(&ab, status, len);
  
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(&ab, rstatus, rlen);
      break;
    } else {
      abAppend(&ab, " ", 1);
      len++;
    }
  }
  
  if (E.statusmsg[0] != '\0') {
      char buf[32];
      snprintf(buf, sizeof(buf), "\x1b[%d;1H", E.screenrows + 1);
      abAppend(&ab, buf, strlen(buf));
      abAppend(&ab, "\x1b[2K", 4);     
      abAppend(&ab, E.statusmsg, strlen(E.statusmsg));
  }

  /* Restore cursor position */
  char buf[32];
  /* Cursor calculation remains +2 because text area is indented */
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.cx - E.coloff) + 2);
  
  abAppend(&ab, buf, strlen(buf));
  
  abAppend(&ab, "\x1b[?25h", 6);
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/* Input */

void editorProcessKeypress() {
  int c = editorReadKey();
  
  if (c == CTRL_KEY('q')) {
    if (E.dirty) {
       snprintf(E.statusmsg, sizeof(E.statusmsg), "Unsaved changes! Press Ctrl+Q again.");
       E.dirty = 0; 
       return;
    }
    editorExit();
  }
  
  if (c == CTRL_KEY('s')) {
      editorSave();
      return;
  }
  
  if (c == CTRL_KEY('x')) {
      char *cmd = editorPrompt(">");
      if (cmd) {
          editorProcessCommand(cmd);
          free(cmd);
      }
      return;
  }

  switch (c) {
  case '\r':
    editorInsertNewline();
    break;
  case HOME_KEY: E.cx = 0; break;
  case END_KEY: if (E.cy < E.numrows) E.cx = E.row[E.cy].size; break;
  case BACKSPACE:
  case CTRL_KEY('h'):
  case DEL_KEY:
    if (c == DEL_KEY) {
        if (E.cx < E.row[E.cy].size) {
            int len = 1;
            while (E.cx + len < E.row[E.cy].size && is_utf8_continuation(E.row[E.cy].chars[E.cx + len])) len++;
            editorRowDelBytes(&E.row[E.cy], E.cx, len);
        } else if (E.cy < E.numrows - 1) {
             E.cx = E.row[E.cy].size;
             editorRowAppendString(&E.row[E.cy], E.row[E.cy+1].chars, E.row[E.cy+1].size);
             editorDelRow(E.cy + 1);
        }
    } else {
        editorDelChar();
    }
    break;
  case ARROW_UP:    if (E.cy != 0) E.cy--; break;
  case ARROW_DOWN:  if (E.cy < E.numrows - 1) E.cy++; break;
  case ARROW_LEFT:  if (E.cx != 0) E.cx--; break;
  case ARROW_RIGHT: 
    if (E.cy < E.numrows && E.cx < E.row[E.cy].size) {
        E.cx++;
        while (E.cx < E.row[E.cy].size && is_utf8_continuation(E.row[E.cy].chars[E.cx])) E.cx++;
    } else if (E.cy < E.numrows - 1) {
        E.cy++; E.cx = 0;
    }
    break;
  case PAGE_UP: case PAGE_DOWN:
    {
        int times = E.screenrows;
        while(times--) {
             if (c == PAGE_UP) { if (E.cy != 0) E.cy--; }
             else { if (E.cy < E.numrows - 1) E.cy++; }
        }
    }
    break;
  case '\x1b': break;
  
  default:
    /* CHANGE: Filter junk control characters */
    if (!iscntrl(c) || c == '\t') {
        editorInsertChar(c);
    }
    break;
  }

  /* Cursor Clamping */
  int rowlen = (E.cy < E.numrows) ? E.row[E.cy].size : 0;
  if (E.cx > rowlen) E.cx = rowlen;
}

void initEditor() {
  E.cx = 0; E.cy = 0;
  E.rowoff = 0; E.coloff = 0;
  E.numrows = 0; E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 1;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) editorOpen(argv[1]);
    while (1) {
    /* Calculate RX (Visual Position) before scrolling */
    E.rx = 0;
    if (E.cy < E.numrows) {
      E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    /* Scroll Logic based on RX (Visual), not CX (Bytes) */
    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    
    /* Horizontal scrolling uses RX now */
    if (E.rx < E.coloff) E.coloff = E.rx;
    if (E.rx >= E.coloff + E.screencols) E.coloff = E.rx - E.screencols + 1;
    
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}