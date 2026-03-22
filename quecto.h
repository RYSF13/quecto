#ifndef QUECTO_H
#define QUECTO_H

/* Ask the C library for the POSIX pieces we rely on: getline() and
   strdup(). Both macros have to be defined before the first #include. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#define Q_VERSION "1.0.0"

/* Turns a letter into the code a terminal sends for Ctrl+letter. */
#define CTRL_KEY(k) ((k) & 0x1f)

/* How many columns a hard tab advances the text by. Every tab stop in
   the display is derived from this one number. */
#define TAB_WIDTH 3

/* Keys that are not characters are numbered above the Unicode range, so
   a decoded code point can never be mistaken for a function key. The
   usual trick of numbering them from 1000 upwards collides with Greek,
   Cyrillic and CJK characters, which really do arrive from the keyboard
   as code points 1000 and up. */
enum KeyCode {
    KEY_NONE = 0,
    KEY_BACKSPACE = 127,
    KEY_SPECIAL = 0x110000,
    KEY_LEFT = KEY_SPECIAL,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_DELETE,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDOWN
};

/* A growable byte buffer, used to assemble one screen frame before it is
   handed to write(). Drawing character by character would mean one
   syscall per cell. */
typedef struct {
    char *data;
    int len;
    int cap;
} QBuf;

/* One line of the file. `text` holds UTF-8 bytes and is NUL terminated,
   but `len` is what counts: a text file may legitimately contain NUL
   bytes, and the editor must not choke on them.

   `rows` caches how many screen rows the line needs once wrapped. Without
   it every frame would re-measure every line up to the cursor, which is
   the difference between instant and sluggish on a large file. */
typedef struct {
    int len;
    char *text;
    int rows;
} Line;

/* The editor state. A single global instance; editing two files at once
   is not something a program this size needs to do. */
typedef struct {
    int curx;                 /* byte offset of the cursor in the line  */
    int cury;                 /* current line, may equal linecount      */
    int goalx;                /* column to aim for on a vertical move   */
    int offsety;              /* first screen row currently shown       */
    int termrows;             /* terminal height                        */
    int termcols;             /* terminal width                         */
    int linecount;
    Line *lines;
    int modified;             /* set once the buffer differs from disk  */
    char *filepath;           /* NULL until the file has a name         */
    char message[256];        /* one line of feedback, empty when idle  */
    int confirmquit;          /* user has already been warned once      */
    struct termios savedterm; /* terminal state to restore on exit      */
} Editor;

extern Editor Q;

#endif /* QUECTO_H */
