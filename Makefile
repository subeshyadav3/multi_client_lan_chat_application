# ======================================================================
# ConnectHub -- build script
#
# Produces two executables:
#   bin/chatclient   -> the terminal (TUI) chat client
#   bin/chatserver   -> the multithreaded TCP chat server
#
# Only gcc and make are needed (no external libraries).
# ======================================================================

# --- Compiler settings ------------------------------------------------
CC     := gcc
CFLAGS := -Wall -Wextra -O2 -g

# --- Output directories ------------------------------------------------
OBJDIR := build
BINDIR := bin

# --- Default goal: build both programs ---------------------------------
all: directories $(BINDIR)/chatclient $(BINDIR)/chatserver

# --- Create the directories we need the first time ---------------------
directories:
	mkdir -p $(OBJDIR)/shared $(OBJDIR)/client $(OBJDIR)/server
	mkdir -p $(BINDIR) logs files

# --- Generic rule: compile any shared/ .c into a .o in build/shared ----
$(OBJDIR)/shared/%.o: shared/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# --- Generic rule: compile any client/ .c into a .o in build/client ----
$(OBJDIR)/client/%.o: client/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# --- Generic rule: compile any server/ .c into a .o in build/server ----
$(OBJDIR)/server/%.o: server/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# --- Link the chat client ----------------------------------------------
$(BINDIR)/chatclient: $(OBJDIR)/client/client.o $(OBJDIR)/client/net.o $(OBJDIR)/client/tui.o $(OBJDIR)/client/protocol.o $(OBJDIR)/client/commands.o $(OBJDIR)/client/files.o $(OBJDIR)/shared/protocol.o
	$(CC) $^ -o $@

# --- Link the chat server (threads need -lpthread) ---------------------
$(BINDIR)/chatserver: $(OBJDIR)/server/server.o $(OBJDIR)/server/connection.o $(OBJDIR)/server/net.o $(OBJDIR)/server/handlers.o $(OBJDIR)/server/users.o $(OBJDIR)/server/history.o $(OBJDIR)/server/files.o $(OBJDIR)/server/room.o $(OBJDIR)/server/room_access.o $(OBJDIR)/server/logger.o $(OBJDIR)/shared/protocol.o $(OBJDIR)/shared/sha256.o
	$(CC) $^ -o $@ -lpthread

# --- Remove all build output -------------------------------------------
clean:
	rm -rf $(OBJDIR)
	rm -f $(BINDIR)/chatclient $(BINDIR)/chatserver

# --- Targets that are not real files ------------------------------------
.PHONY: all clean directories

