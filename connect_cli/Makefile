CC       := gcc
CFLAGS   := -Wall -Wextra -O2 -g
OBJDIR   := build
BINDIR   := bin

all: directories $(BINDIR)/chatclient $(BINDIR)/chatserver

directories:
	mkdir -p $(OBJDIR)/shared $(OBJDIR)/client $(OBJDIR)/server
	mkdir -p $(BINDIR) logs files

$(OBJDIR)/shared/%.o: shared/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/client/%.o: client/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/server/%.o: server/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BINDIR)/chatclient: $(OBJDIR)/client/client.o $(OBJDIR)/client/net.o $(OBJDIR)/client/tui.o $(OBJDIR)/shared/protocol.o
	$(CC) $^ -o $@

$(BINDIR)/chatserver: $(OBJDIR)/server/server.o $(OBJDIR)/server/room.o $(OBJDIR)/server/room_access.o $(OBJDIR)/server/logger.o $(OBJDIR)/shared/protocol.o $(OBJDIR)/shared/sha256.o
	$(CC) $^ -o $@ -lpthread

clean:
	rm -rf $(OBJDIR) $(BINDIR)

.PHONY: all clean directories
