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
ALL_SRC = $(wildcard *.c)
ALL_OBJ = $(ALL_SRC:%.c=build/%.o)

.PHONY: all clean

all: $(BIN)

$(BIN): $(ALL_OBJ)
	$(LD) -o $@ $^ $(LDFLAGS)

build/%.o: %.c | build
	$(CC) $(CFLAGS) -o $@ $<

build:
	mkdir $@

clean:
	rm -rf build/
