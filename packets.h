/*
 * Copyright (C) 2023 Yuichi Nakamura (@yunkya2)
 * Based upon packets.h by @bet4it
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PACKETS_H
#define PACKETS_H

#include <stdint.h>

#define PACKET_BUF_SIZE 0x8000

static const char INTERRUPT_CHAR = '\x03';

uint8_t *inbuf_get();
int inbuf_end();
void inbuf_erase_head(ssize_t end);
void write_flush();
void write_packet(const char *data);
void write_binary_packet(const char *pfx, const uint8_t *data, ssize_t num_bytes);
int read_packet(int waitkey);
void remote_prepare(char *name);

#endif /* PACKETS_H */
