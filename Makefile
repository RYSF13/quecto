# Makefile for Quecto - The Minimalist Editor
#
# Running 'make' will:
# 1. Compile 'quecto' with hardcore size optimization
# 2. Install 'quecto' to /usr/local/bin/
# 3. Create a symlink 'q' pointing to 'quecto'

CC = cc
TARGET = quecto
ALIAS = q
SRC = quecto.c
INSTALL_PATH = /usr/local/bin

# Hardcore Compiler Flags
CFLAGS = -Os -s -w -static-libgcc -no-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -fno-stack-protector -fomit-frame-pointer -fmerge-all-constants -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--build-id=none -Wl,-z,norelro -Wl,-N 


# Strip Flags
# -R: Remove sections that 'strip -s' usually keeps
STRIP_FLAGS = -s -R .comment -R .gnu.version

.PHONY: all build install clean uninstall

# Default target: Compile AND Install
all: install

# Compilation Step
build: $(TARGET)

$(TARGET): $(SRC)
	@echo " [CC]   Compiling $(TARGET)..."
	@$(CC) $(CFLAGS) $(SRC) -o $(TARGET)
	@echo " [STRIP] Removing bloat..."
	@strip $(STRIP_FLAGS) $(TARGET)
	@echo " [INFO] Final Binary Size: $$(wc -c $(TARGET)) bytes"

# Installation Step
install: build
	@echo " [INST] Installing to $(INSTALL_PATH)..."
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "        (Root required, using sudo)"; \
		sudo cp -f $(TARGET) $(INSTALL_PATH)/$(TARGET); \
		sudo chmod 755 $(INSTALL_PATH)/$(TARGET); \
		echo " [LINK] Creating symlink '$(ALIAS)' -> '$(TARGET)'..."; \
		sudo ln -sf $(TARGET) $(INSTALL_PATH)/$(ALIAS); \
	else \
		cp -f $(TARGET) $(INSTALL_PATH)/$(TARGET); \
		chmod 755 $(INSTALL_PATH)/$(TARGET); \
		echo " [LINK] Creating symlink '$(ALIAS)' -> '$(TARGET)'..."; \
		ln -sf $(TARGET) $(INSTALL_PATH)/$(ALIAS); \
	fi
	@echo " [DONE] Installed. Use '$(ALIAS) <file>' or '$(TARGET) <file>' to edit."

clean:
	@echo " [RM]   Cleaning up..."
	@rm -f $(TARGET)

uninstall:
	@echo " [RM]   Uninstalling from $(INSTALL_PATH)..."
	@if [ "$$(id -u)" -ne 0 ]; then \
		sudo rm -f $(INSTALL_PATH)/$(TARGET); \
		sudo rm -f $(INSTALL_PATH)/$(ALIAS); \
	else \
		rm -f $(INSTALL_PATH)/$(TARGET); \
		rm -f $(INSTALL_PATH)/$(ALIAS); \
	fi
	@echo " [DONE] Uninstalled."
