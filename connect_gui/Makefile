CC       := gcc
COMMON   := -Wall -Wextra -O2
CLIENT_CFLAGS := $(COMMON) $(shell pkg-config --cflags gtk+-3.0)
SERVER_CFLAGS := $(COMMON)
CLIENT_LDFLAGS := -lpthread $(shell pkg-config --libs gtk+-3.0) -lm
SERVER_LDFLAGS := -lpthread

OBJDIR   := build
BINDIR   := bin

all: directories $(BINDIR)/chatclient $(BINDIR)/chatserver

directories:
	mkdir -p $(OBJDIR)/shared $(OBJDIR)/client $(OBJDIR)/server
	mkdir -p $(BINDIR) logs files

$(OBJDIR)/shared/%.o: shared/%.c
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

$(OBJDIR)/client/%.o: client/%.c
	$(CC) $(CLIENT_CFLAGS) -c $< -o $@

$(OBJDIR)/server/%.o: server/%.c
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

$(BINDIR)/chatclient: $(OBJDIR)/client/client.o $(OBJDIR)/client/chat.o $(OBJDIR)/client/ui.o $(OBJDIR)/shared/protocol.o
	$(CC) $^ -o $@ $(CLIENT_LDFLAGS)

$(BINDIR)/chatserver: $(OBJDIR)/server/server.o $(OBJDIR)/server/room.o $(OBJDIR)/server/logger.o $(OBJDIR)/shared/protocol.o
	$(CC) $^ -o $@ $(SERVER_LDFLAGS)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

.PHONY: all clean directories
