CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_DARWIN_C_SOURCE -D_GNU_SOURCE
LDLIBS   = -lz
SRC      = $(wildcard src/*.c)
BIN      = claude-backup

all: $(BIN)

$(BIN): $(SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

# macOS universal binary (Apple Silicon + Intel) for distribution
dist: $(SRC)
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 -o $(BIN) $(SRC) $(LDLIBS)
	strip $(BIN)
	@echo "built $(BIN): $$(file $(BIN))"

install: $(BIN)
	install -d $(HOME)/.local/bin
	install -m 755 $(BIN) $(HOME)/.local/bin/$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all dist install clean
