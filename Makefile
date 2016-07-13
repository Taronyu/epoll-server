# epoll-server Makefile

CC = gcc
CFLAGS = -c -Wall
LD = gcc
LDFLAGS =

# Debug build?
ifeq ($(DEBUG), 1)
CFLAGS += -g -O0
else
CFLAGS += -O2 -DNDEBUG
endif

BIN = build/epoll-server
SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	$(LD) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -o $@ $<

build:
	mkdir $@

clean:
	rm -rf build/
