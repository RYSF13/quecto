# Quecto Editor Makefile
#
# Plain rules, no generated files, no dependency tracking beyond the one
# header: the whole program is a single translation unit.

CC ?= gcc
CFLAGS = -std=c99 -Os -Wall -Wextra -pedantic
LDFLAGS =

# Shrink the binary a little further. None of these change behaviour.
CFLAGS += -fno-asynchronous-unwind-tables -fno-unwind-tables
CFLAGS += -fdata-sections -ffunction-sections -fno-stack-protector
CFLAGS += -fno-ident -fomit-frame-pointer -s

# Platform-specific dead code elimination.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    LDFLAGS += -Wl,-dead_strip
else
    LDFLAGS += -Wl,--gc-sections -Wl,--strip-all
endif

# Static linking, for musl based systems and rescue images.
ifdef STATIC
    LDFLAGS += -static
endif

# Installation paths. DESTDIR is honoured so packagers can stage installs.
PREFIX ?= /usr/local
BINDIR  = $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man

# Files
SRC = quecto.c
HDR = quecto.h
MAN = quecto.1
BIN = quecto
LINK = q

.PHONY: all build check install uninstall clean purge help

# Default target
all: build install clean

# Compile
build: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

# Warnings are the point of this target, so promote them to errors.
check: $(SRC) $(HDR)
	$(CC) -std=c99 -Wall -Wextra -Werror -pedantic -fsyntax-only $(SRC)

# Install the binary, the short name and the manual page
install: $(BIN)
	@mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)/man1
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	ln -sf $(BIN) $(DESTDIR)$(BINDIR)/$(LINK)
	install -m 644 $(MAN) $(DESTDIR)$(MANDIR)/man1/$(MAN)
	@echo "Installed $(BIN) to $(DESTDIR)$(BINDIR)"
	@echo "Created symlink $(LINK) -> $(BIN)"
	@echo "Installed manual page to $(DESTDIR)$(MANDIR)/man1/$(MAN)"

# Remove the local build
clean:
	@rm -f $(BIN)
	@echo "Cleaned local build files"

# Remove the installed files
uninstall:
	@rm -f $(DESTDIR)$(BINDIR)/$(LINK)
	@rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	@rm -f $(DESTDIR)$(MANDIR)/man1/$(MAN)
	@echo "Uninstalled $(BIN), $(LINK) and $(MAN)"

# Both of the above
purge: clean uninstall
	@echo "Purged all files"

# Help
help:
	@echo "Quecto Editor - Makefile targets:"
	@echo ""
	@echo "  make all       - Build, install, and clean local files"
	@echo "  make build     - Compile to $(BIN)"
	@echo "  make check     - Compile with warnings as errors, build nothing"
	@echo "  make install   - Install binary, symlink and man page"
	@echo "  make clean     - Remove the local $(BIN)"
	@echo "  make uninstall - Remove installed files"
	@echo "  make purge     - Clean local + uninstall (remove all)"
	@echo ""
	@echo "Options:"
	@echo "  PREFIX=/path   - Installation prefix (default: /usr/local)"
	@echo "  DESTDIR=/path  - Staging directory for packaging"
	@echo "  STATIC=1       - Build a static binary (for musl)"
	@echo ""
	@echo "Examples:"
	@echo "  make all"
	@echo "  make STATIC=1 build"
	@echo "  sudo make install"
	@echo "  PREFIX=~/.local make install"
