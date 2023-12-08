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
    while (_iocs_isns232c() == 0)
        ;
    int c = _iocs_inp232c() & 0xff;

    if (debuglevel > 1)
    {
        if (c < ' ')
            printf("{%02X}", c);
          else
            printf("%c", c);
    }

    return c;
}

void read_packet()
{
    uint8_t c;
    pktbuf_clear(&in);
    do {
        c = inp232c();
    } while (c != '$' && c != INTERRUPT_CHAR);

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
        bdset = 9;      // 38400

    // stop 1 / nonparity / 8bit / nonxoff
    _iocs_set232c(0x4c00 | bdset);
}
