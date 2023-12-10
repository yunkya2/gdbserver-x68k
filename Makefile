#
# Copyright (C) 2023 Yuichi Nakamura (@yunkya2)
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
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
