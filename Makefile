CROSS = m68k-xelf-
CC = $(CROSS)gcc
AS = $(CROSS)gcc
LD = $(CROSS)gcc
AR = $(CROSS)ar
RANLIB = $(CROSS)ranlib
OBJCOPY = $(CROSS)objcopy

#GIT_REPO_VERSION=$(shell git describe --tags --always)

CFLAGS = -g -std=gnu99 -O
CFLAGS += -finput-charset=utf-8 -fexec-charset=cp932

OBJS = gdbserver.o utils.o packets.o ptrace.o

all: gdbserver.x

gdbserver.x: $(OBJS)
	$(CC) -o $@ $^

gdbserver.o : gdbserver.c arch.h utils.h packets.h ptrace.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	-rm -f *.o *.x*

.PHONY: all clean
