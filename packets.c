/*
 * Copyright (C) 2023,2024 Yuichi Nakamura (@yunkya2)
 * Based upon packets.c by @bet4it
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

// Many codes in this file was borrowed from GdbConnection.cc in rr
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <unistd.h>
#include <stdbool.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include "packets.h"

extern int debuglevel;
extern int ctrlc;

struct packet_buf
{
    uint8_t buf[PACKET_BUF_SIZE];
    int end;
} in, out;

int sock_fd;

uint8_t *inbuf_get()
{
    return in.buf;
}

int inbuf_end()
{
    return in.end;
}

void pktbuf_insert(struct packet_buf *pkt, const uint8_t *buf, ssize_t len)
{
    if (pkt->end + len >= sizeof(pkt->buf))
    {
        puts("Packet buffer overflow");
        exit(-2);
    }
    memcpy(pkt->buf + pkt->end, buf, len);
    pkt->end += len;
}

void pktbuf_erase_head(struct packet_buf *pkt, ssize_t end)
{
    memmove(pkt->buf, pkt->buf + end, pkt->end - end);
    pkt->end -= end;
}

void inbuf_erase_head(ssize_t end)
{
    pktbuf_erase_head(&in, end);
}

void pktbuf_clear(struct packet_buf *pkt)
{
    pkt->end = 0;
}

void write_flush()
{
    size_t write_index = 0;

    if (debuglevel > 1)
        printf("\x1b[31m");

    while (write_index < out.end)
    {
        while (_iocs_osns232c() == 0)
            ;
        if (debuglevel > 1)
            putchar(out.buf[write_index]);
        _iocs_out232c(out.buf[write_index++]);
    }

    if (debuglevel > 1)
        printf("\x1b[m\n");

    pktbuf_clear(&out);
}

void write_data_raw(const uint8_t *data, ssize_t len)
{
    pktbuf_insert(&out, data, len);
}

void write_hex(unsigned long hex)
{
    char buf[32];
    size_t len;

    len = snprintf(buf, sizeof(buf) - 1, "%02lx", hex);
    write_data_raw((uint8_t *)buf, len);
}

void write_packet_bytes(const uint8_t *data, size_t num_bytes)
{
    uint8_t checksum;
    size_t i;

    write_data_raw((uint8_t *)"$", 1);
    for (i = 0, checksum = 0; i < num_bytes; ++i)
        checksum += data[i];
    write_data_raw((uint8_t *)data, num_bytes);
    write_data_raw((uint8_t *)"#", 1);
    write_hex(checksum);
}

void write_packet(const char *data)
{
    write_packet_bytes((const uint8_t *)data, strlen(data));
}

void write_binary_packet(const char *pfx, const uint8_t *data, ssize_t num_bytes)
{
    uint8_t *buf;
    ssize_t pfx_num_chars = strlen(pfx);
    ssize_t buf_num_bytes = 0;
    int i;

    buf = malloc(2 * num_bytes + pfx_num_chars);
    memcpy(buf, pfx, pfx_num_chars);
    buf_num_bytes += pfx_num_chars;

    for (i = 0; i < num_bytes; ++i)
    {
        uint8_t b = data[i];
        switch (b)
        {
        case '#':
        case '$':
        case '}':
        case '*':
            buf[buf_num_bytes++] = '}';
            buf[buf_num_bytes++] = b ^ 0x20;
            break;
        default:
            buf[buf_num_bytes++] = b;
            break;
        }
    }
    write_packet_bytes(buf, buf_num_bytes);
    free(buf);
}

static int inp232c(void)
{
    int c;
    uint16_t sr;
    __asm__ ("move.w %%sr,%0" : "=d"(sr));

    if ((sr & 0x0700) < 0x0500) {           // SCC interrupt enable
        while (_iocs_isns232c() == 0)
            ;
        c = _iocs_inp232c() & 0xff;
    } else {                                // SCC interrupt disable
        if (_iocs_isns232c()) {
            c = _iocs_inp232c() & 0xff;
        } else {
            c = *(volatile uint16_t *)0xe98004;         // Ch.A command port (select RR0)
            do {
                c = *(volatile uint16_t *)0xe98004;
            } while (!(c & 1));                         // wait for Rx char available
            c = *(volatile uint16_t *)0xe98006 & 0xff;  // Ch.A data port
        }
    }

    if (debuglevel > 1)
    {
        if (c < ' ')
            printf("{%02X}", c);
          else
            printf("%c", c);
        fflush(stdout);
    }

    return c;
}

int read_packet(int waitkey)
{
    uint8_t c;
    pktbuf_clear(&in);
    do {
        if (waitkey) {
            do {
                int key = _iocs_b_keysns();
                if (key) {
                    key = _iocs_b_keyinp();
                    if (key & 0xff) {
                        return -1;
                    }
                }
            } while (_iocs_isns232c() == 0);
        }

        c = inp232c();
        if (c == INTERRUPT_CHAR) {
            ctrlc = true;
            continue;
        }
    } while (c != '$');

    pktbuf_insert(&in, &c, 1);
    do {
        c = inp232c();
        pktbuf_insert(&in, &c, 1);
    } while (c != '#');
    c = inp232c();
    pktbuf_insert(&in, &c, 1);
    c = inp232c();
    pktbuf_insert(&in, &c, 1);

    write_data_raw((uint8_t *)"+", 1);
    write_flush();
    return 0;
}

void remote_prepare(char *speed)
{
    static const int bauddef[] = { 75, 150, 300, 600, 1200, 2400, 4800, 9600, 19200, 38400 };
    int bdset = -1;
    int baudrate = atoi(speed);

    for (int i = 0; i < sizeof(bauddef) / sizeof(bauddef[0]); i++) {
        if (baudrate == bauddef[i]) {
            printf("Serial speed:%dbps\n", baudrate);
            bdset = i;
            break;
        }
    }
    if (bdset < 0)
        bdset = 7;      // 9600

    // stop 1 / nonparity / 8bit / nonxoff
    _iocs_set232c(0x4c00 | bdset);
}
