CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11 -Iinclude -Ithird_party/inc
LDFLAGS = -lpthread

FDB_SRC = third_party/src/fdb.c third_party/src/fdb_utils.c third_party/src/fdb_kvdb.c third_party/src/fdb_tsdb.c third_party/src/fdb_file.c
SRC = src/server.c $(FDB_SRC)
BIN = gta6-blog

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

debug: CFLAGS += -g -O0 -DDEBUG
debug: $(BIN)

clean:
	rm -f $(BIN)

.PHONY: all debug clean
