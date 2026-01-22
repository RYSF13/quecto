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
#define Q_VERSION "1.2"

enum {
  BACKSPACE = 127,
  ARROW_LEFT = 1000, ARROW_RIGHT, ARROW_UP, ARROW_DOWN,
  DEL_KEY, HOME_KEY, END_KEY, PAGE_UP, PAGE_DOWN
};

typedef struct erow {
  int size;
  char *chars;
} erow;

struct editorConfig {
  int cx, cy;
  int rowoff;
  int screenrows, screencols;
  int numrows;
  erow *row;
  int dirty;
  int quit_times;
  char *filename;
  char statusmsg[80];
  struct termios orig_termios;
} E;

/* --- Terminal & raw mode --- */

void disableRawMode() {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

void die(const char *s) {
  disableRawMode(); /* Restore TTY state to avoid staircase effect */
  write(STDOUT_FILENO, "\x1b[0m\x1b[2J\x1b[H", 11);
  perror(s);
  exit(1);
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

/* --- Row operations --- */

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

void editorFreeRow(erow *row) { free(row->chars); }

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

/* --- Editor logic --- */

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

void editorInsertChar(int c) {
  if (E.cy == E.numrows) editorInsertRow(E.numrows, "", 0);
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorDelChar() {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;
  if (E.cx > 0) {
    editorRowDelBytes(&E.row[E.cy], E.cx - 1, 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], E.row[E.cy].chars, E.row[E.cy].size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

char *editorRowsToString(int *buflen) {
  int totlen = 0, j;
  for (j = 0; j < E.numrows; j++) totlen += E.row[j].size + 1;
  *buflen = totlen;
  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n'; p++;
  }
  return buf;
}

/* --- File I/O --- */

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
        fclose(fp); free(buf); E.dirty = 0;
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Saved");
        return;
      }
    }
    fclose(fp);
  }
  free(buf);
  snprintf(E.statusmsg, sizeof(E.statusmsg), "I/O Error");
}

/* --- Regex & Commands --- */

void editorRegexReplace(char *pattern, char *repl, int global) {
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) return;
    int count = 0, i;
    for(i=0; i<E.numrows; i++) {
        erow *row = &E.row[i];
        regmatch_t match;
        int offset = 0;
        while(regexec(&regex, row->chars + offset, 1, &match, 0) == 0) {
            int start = offset + match.rm_so;
            int end = offset + match.rm_eo;
            int ld = (strlen(repl)) - (end - start);
            row->chars = realloc(row->chars, row->size + ld + 1);
            memmove(row->chars + end + ld, row->chars + end, row->size - end + 1);
            memcpy(row->chars + start, repl, strlen(repl));
            row->size += ld;
            offset = start + strlen(repl);
            count++; E.dirty++;
            if (!global) break; 
        }
        if(!global && count > 0) break; 
    }
    regfree(&regex);
    snprintf(E.statusmsg, sizeof(E.statusmsg), "Replaced %d", count);
}

void editorExit() {
    write(STDOUT_FILENO, "\x1b[0m\x1b[2J\x1b[H", 11);
    disableRawMode(); 
    exit(0);
}

void editorProcessCommand(char *cmd) {
    if (isdigit(cmd[0])) { /* Jump */
        int l = atoi(cmd);
        E.cy = (l > 0 && l <= E.numrows) ? l - 1 : E.numrows - 1;
        if(E.cy < 0) E.cy = 0;
        return;
    }
    if (strcmp(cmd, "q") == 0) {
         if (E.dirty) { snprintf(E.statusmsg, sizeof(E.statusmsg), "Unsaved! (q!)"); return; }
         editorExit();
    }
    if (strcmp(cmd, "q!") == 0) editorExit();
    if (strcmp(cmd, "w") == 0) editorSave();
    if (strcmp(cmd, "wq") == 0) { editorSave(); editorExit(); }
    if (cmd[0] == 'r' && cmd[1] == '/') {
        char *p = cmd + 2, *r = NULL, *f = NULL, *e = strchr(p, '/');
        if(e) { *e = 0; r = e + 1; e = strchr(r, '/'); if(e) { *e = 0; f = e + 1; } }
        if(p && r) editorRegexReplace(p, r, (f && strchr(f, 'G')));
    }
}

char *editorPrompt(char *prompt) {
  size_t bufsize = 128; char *buf = malloc(bufsize); size_t buflen = 0;
  buf[0] = '\0';
  while(1) {
      char s[32]; snprintf(s, sizeof(s), "\x1b[%d;1H", E.screenrows + 1);
      write(STDOUT_FILENO, s, strlen(s));
      write(STDOUT_FILENO, "\x1b[2K\x1b[7m", 8); /* Clear + Invert */
      write(STDOUT_FILENO, prompt, strlen(prompt));
      write(STDOUT_FILENO, buf, buflen);
      int total = strlen(prompt) + buflen;
      while (total++ < E.screencols) write(STDOUT_FILENO, " ", 1); /* Fill bar */
      write(STDOUT_FILENO, "\x1b[m", 3);
      snprintf(s, sizeof(s), "\x1b[%d;%ldH", E.screenrows + 1, strlen(prompt) + buflen + 1);
      write(STDOUT_FILENO, s, strlen(s));

      int c = editorReadKey();
      if (c == BACKSPACE || c == 127) { if (buflen != 0) buf[--buflen] = '\0'; }
      else if (c == '\x1b') { free(buf); return NULL; }
      else if (c == '\r') return buf;
      else if (!iscntrl(c) && c < 128) { buf[buflen++] = c; buf[buflen] = '\0'; }
  }
}

/* --- Rendering --- */

struct abuf { char *b; int len; };
#define ABUF_INIT {NULL, 0}
void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);
  if (new) { memcpy(&new[ab->len], s, len); ab->b = new; ab->len += len; }
}
void abFree(struct abuf *ab) { free(ab->b); }

void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;
  /* Hide cursor + Go Home + Reset Color */
  abAppend(&ab, "\x1b[?25l\x1b[H\x1b[0m", 13);
  
  int width = E.screencols - 1; /* Soft wrap width (minus padding) */
  int visual_r = 0, i;
  int cursor_vy = -1, cursor_vx = -1;

  for (i = E.rowoff; i < E.numrows; i++) {
      int len = E.row[i].size;
      int chunks = (len / width) + 1;
      if (len == 0) chunks = 1;
      
      if (i == E.cy) {
          cursor_vy = visual_r + (E.cx / width);
          cursor_vx = (E.cx % width) + 2; 
      }

      int c = 0, chunk_idx;
      for (chunk_idx = 0; chunk_idx < chunks; chunk_idx++) {
          if (visual_r >= E.screenrows) break;
          abAppend(&ab, " ", 1); /* Left padding */
          
          int clen = width;
          if (c + clen > len) clen = len - c;
          
          if (clen > 0) abAppend(&ab, &E.row[i].chars[c], clen);
          abAppend(&ab, "\x1b[K\r\n", 5);
          c += clen; visual_r++;
      }
      if (visual_r >= E.screenrows) break;
  }
  
  while (visual_r < E.screenrows) {
      abAppend(&ab, "~", 1);
      abAppend(&ab, "\x1b[K\r\n", 5);
      visual_r++;
  }
  
  /* Status Bar */
  char status[80], rstatus[80];
  if (E.statusmsg[0]) snprintf(status, sizeof(status), "%s", E.statusmsg);
  else snprintf(status, sizeof(status), "%.20s %dL %s", E.filename?E.filename:"[N]", E.numrows, E.dirty?"*":"");
  int len = strlen(status);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d,%d", E.cy + 1, E.cx + 1);
  if (len > E.screencols) len = E.screencols;
  abAppend(&ab, "\x1b[7m", 4);
  abAppend(&ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) { abAppend(&ab, rstatus, rlen); break; }
    else { abAppend(&ab, " ", 1); len++; }
  }
  abAppend(&ab, "\x1b[m", 3);
  
  if (cursor_vy != -1 && cursor_vy < E.screenrows) {
      char pos[32];
      snprintf(pos, sizeof(pos), "\x1b[%d;%dH", cursor_vy + 1, cursor_vx);
      abAppend(&ab, pos, strlen(pos));
  }
  
  abAppend(&ab, "\x1b[?25h", 6);
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/* --- Input --- */

void editorProcessKeypress() {
  int c = editorReadKey();
  if (c == CTRL_KEY('q')) {
      if(E.dirty && E.quit_times > 0) { 
          snprintf(E.statusmsg, sizeof(E.statusmsg), "Unsaved! Press Ctrl+Q again.");
          E.quit_times--; return; 
      }
      editorExit();
  }
  E.quit_times = 1;
  if (c == CTRL_KEY('s')) { editorSave(); return; }
  if (c == CTRL_KEY('x')) {
      char *cmd = editorPrompt(">");
      if (cmd) { editorProcessCommand(cmd); free(cmd); }
      return;
  }
  switch (c) {
  case '\r': editorInsertNewline(); break;
  case HOME_KEY: E.cx = 0; break;
  case END_KEY: if (E.cy < E.numrows) E.cx = E.row[E.cy].size; break;
  case BACKSPACE: case DEL_KEY: case CTRL_KEY('h'):
    if (c == DEL_KEY) { if (E.cx < E.row[E.cy].size) { editorRowDelBytes(&E.row[E.cy], E.cx, 1); } }
    else editorDelChar();
    break;
  case ARROW_UP:    if (E.cy != 0) E.cy--; break;
  case ARROW_DOWN:  if (E.cy < E.numrows - 1) E.cy++; break;
  case ARROW_LEFT:  if (E.cx != 0) E.cx--; else if (E.cy>0) { E.cy--; E.cx=E.row[E.cy].size; } break;
  case ARROW_RIGHT: if (E.cx < E.row[E.cy].size) E.cx++; else if (E.cy<E.numrows-1) { E.cy++; E.cx=0; } break;
  case PAGE_UP: E.cy = E.rowoff; break;
  case PAGE_DOWN: E.cy = E.rowoff + E.screenrows - 1; if(E.cy > E.numrows) E.cy = E.numrows; break;
  default: if (!iscntrl(c) || c == '\t') editorInsertChar(c); break;
  }
  if (E.cy < E.numrows && E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  E.cx = 0; E.cy = 0; E.rowoff = 0; E.numrows = 0; E.row = NULL; E.dirty = 0; E.filename = NULL; 
  E.statusmsg[0] = 0; E.quit_times = 1;
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("ws");
  E.screenrows -= 1; 
  if (argc >= 2) editorOpen(argv[1]);
  while (1) {
    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}