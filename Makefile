# Quecto Editor Makefile

CC ?= gcc
CFLAGS = -std=c99 -Os -s -Wall -Wextra -pedantic
LDFLAGS = -s

# Additional flags for minimal binary size
CFLAGS += -fno-asynchronous-unwind-tables -fno-unwind-tables
CFLAGS += -fdata-sections -ffunction-sections -fno-stack-protector
CFLAGS += -fno-ident -fomit-frame-pointer

# Platform-specific linker flags for dead code elimination
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    LDFLAGS += -Wl,-dead_strip
else
    LDFLAGS += -Wl,--gc-sections -Wl,--strip-all
endif

# musl/Alpine compatibility (no special flags needed, but ensure static linking option)
ifdef STATIC
    LDFLAGS += -static
endif

# Installation paths
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

# Source files
SRC = quecto.c
HDR = quecto.h
OBJ = quecto.o
BIN = quecto
LINK = q

.PHONY: all build install clean uninstall purge help

# Default target
all: build install clean

# Compile only
build: $(OBJ)

$(OBJ): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(OBJ) $(SRC)

# Install to system
install: $(OBJ)
	@mkdir -p $(BINDIR)
	@cp $(OBJ) $(BINDIR)/$(BIN)
	@chmod 755 $(BINDIR)/$(BIN)
	@ln -sf $(BIN) $(BINDIR)/$(LINK)
	@echo "Installed $(BIN) to $(BINDIR)"
	@echo "Created symlink $(LINK) -> $(BIN)"

# Clean local build files
clean:
	@rm -f $(OBJ)
	@echo "Cleaned local build files"

# Remove installed files
uninstall:
	@rm -f $(BINDIR)/$(LINK)
	@rm -f $(BINDIR)/$(BIN)
	@echo "Uninstalled $(BIN) and $(LINK) from $(BINDIR)"

# Clean everything (local + installed)
purge: clean uninstall
	@echo "Purged all files"

# Help
help:
	@echo "Quecto Editor - Makefile targets:"
	@echo ""
	@echo "  make all       - Build, install, and clean local files"
	@echo "  make build     - Compile to $(OBJ)"
	@echo "  make install   - Install to $(BINDIR) with symlink"
	@echo "  make clean     - Remove local $(OBJ)"
	@echo "  make uninstall - Remove installed files"
	@echo "  make purge     - Clean local + uninstall (remove all)"
	@echo ""
	@echo "Options:"
	@echo "  PREFIX=/path   - Installation prefix (default: /usr/local)"
	@echo "  STATIC=1       - Build static binary (for musl)"
	@echo ""
	@echo "Examples:"
	@echo "  make all"
	@echo "  make STATIC=1 build"
	@echo "  sudo make install"
	@echo "  PREFIX=~/.local make install"
