/*
 * Copyright (C) 2023,2024 Yuichi Nakamura (@yunkya2)
 * Based upon gdbserver.c by @bet4it
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdbool.h>
#include "arch.h"
#include "utils.h"
#include "packets.h"
#include "ptrace.h"
#include <x68k/dos.h>
#include <x68k/iocs.h>

uint32_t target_offset;
uint32_t target_base = 0;
bool terminate = false;
int debuglevel = 0;
int intrmode = 0;
int ctrlc = 0;

#define BREAKPOINT_NUMBER 64

struct debug_breakpoint_t
{
  size_t addr;
  size_t orig_data;
} breakpoints[BREAKPOINT_NUMBER];

uint8_t tmpbuf[0x20000];
bool attach = false;

char msgbuf[256];

void prepare_resume_reply(uint8_t *buf, bool cont, int result, int exitcode)
{
  if (result < 0) {
    sprintf(buf, "W%02x", exitcode);
    terminate = true;
  } else {
    sprintf(buf, "S%02x", exitcode);
  }
}

void process_xfer(const char *name, char *args)
{
  const char *mode = args;
  args = strchr(args, ':');
  *args++ = '\0';
  if (!strcmp(name, "features") && !strcmp(mode, "read"))
    write_packet(FEATURE_STR);
}

void process_query(char *payload)
{
  const char *name;
  char *args;

  args = strchr(payload, ':');
  if (args)
    *args++ = '\0';
  name = payload;
  if (!strcmp(name, "C"))
  {
    snprintf(tmpbuf, sizeof(tmpbuf), "QCp%02x.%02x", 1, 1);
    write_packet(tmpbuf);
  }
  if (!strcmp(name, "Attached"))
  {
    if (attach)
      write_packet("1");
    else
      write_packet("0");
  }
  if (!strcmp(name, "Offsets"))
  {
    snprintf(tmpbuf, sizeof(tmpbuf), "Text=%x;Data=%x;Bss=%x",
             target_offset, target_offset, target_offset);
    write_packet(tmpbuf);
  }
  if (!strcmp(name, "Supported"))
    write_packet("PacketSize=8000;qXfer:features:read+");
  if (!strcmp(name, "Symbol"))
    write_packet("OK");
  if (name == strstr(name, "ThreadExtraInfo"))
  {
    args = payload;
    args = 1 + strchr(args, ',');
    write_packet("41414141");
  }
  if (!strcmp(name, "TStatus"))
    write_packet("");
  if (!strcmp(name, "Xfer"))
  {
    name = args;
    args = strchr(args, ':');
    *args++ = '\0';
    return process_xfer(name, args);
  }
  if (!strcmp(name, "fThreadInfo"))
  {
    uint8_t pid_buf[20];
    strcpy(tmpbuf, "m");
    write_packet(tmpbuf);
  }
  if (!strcmp(name, "sThreadInfo"))
    write_packet("l");
}

void output_string(char *msg)
{
  if (strlen(msg) == 0)
    return;

  sprintf(tmpbuf, "O");
  while (*msg) {
    char hex[8];
    sprintf(hex, "%02x", *(unsigned char *)msg);
    strcat(tmpbuf, hex);
    msg++;
  }
  write_packet(tmpbuf);
}

void process_vpacket(char *payload)
{
  const char *name;
  char *args;
  args = strchr(payload, ';');
  if (args)
    *args++ = '\0';
  name = payload;

  if (!strcmp("Cont", name))
  {
    if (args[0] == 'c')
    {
      int exitcode;
      int result = ptrace(PTRACE_CONT, 0, &exitcode, msgbuf);
      output_string(msgbuf);
      prepare_resume_reply(tmpbuf, true, result, exitcode);
      write_packet(tmpbuf);
    }
    if (args[0] == 's' || args[0] == 'S' || args[0] == 'C')
    {
      int exitcode;
      int result = ptrace(PTRACE_SINGLESTEP, 0, &exitcode, msgbuf);
      output_string(msgbuf);
      prepare_resume_reply(tmpbuf, true, result, exitcode);
      write_packet(tmpbuf);
    }
  }
  if (!strcmp("Cont?", name))
    write_packet("vCont;c;C;s;S;");
  if (!strcmp("Kill", name))
  {
    ptrace(PTRACE_KILL, 0, 0, 0);
    write_packet("OK");
    terminate = true;
  }
  if (!strcmp("MustReplyEmpty", name))
    write_packet("");
}

bool set_breakpoint(pid_t tid, size_t addr, size_t length)
{
  int i;
  for (i = 0; i < BREAKPOINT_NUMBER; i++)
    if (breakpoints[i].addr == 0)
    {
      size_t data = ptrace(PTRACE_PEEKDATA, tid, (void *)addr, NULL);
      breakpoints[i].orig_data = data;
      breakpoints[i].addr = addr;
      assert(sizeof(break_instr) <= length);
      memcpy((void *)&data, break_instr, sizeof(break_instr));
      ptrace(PTRACE_POKEDATA, tid, (void *)addr, (void *)data);
      break;
    }
  if (i == BREAKPOINT_NUMBER)
    return false;
  else
    return true;
}

bool remove_breakpoint(pid_t tid, size_t addr, size_t length)
{
  int i;
  for (i = 0; i < BREAKPOINT_NUMBER; i++)
    if (breakpoints[i].addr == addr)
    {
      ptrace(PTRACE_POKEDATA, tid, (void *)addr, (void *)breakpoints[i].orig_data);
      breakpoints[i].addr = 0;
      break;
    }
  if (i == BREAKPOINT_NUMBER)
    return false;
  else
    return true;
}

size_t restore_breakpoint(size_t addr, size_t length, size_t data)
{
  for (int i = 0; i < BREAKPOINT_NUMBER; i++)
  {
    size_t bp_addr = breakpoints[i].addr;
    size_t bp_size = sizeof(break_instr);
    if (bp_addr && bp_addr + bp_size > addr && bp_addr < addr + length)
    {
      for (size_t j = 0; j < bp_size; j++)
      {
        if (bp_addr + j >= addr && bp_addr + j < addr + length)
          ((uint8_t *)&data)[bp_addr + j - addr] = ((uint8_t *)&breakpoints[i].orig_data)[j];
      }
    }
  }
  return data;
}

void process_packet()
{
  uint8_t *inbuf = inbuf_get();
  int inbuf_size = inbuf_end();
  uint8_t *packetend_ptr = (uint8_t *)memchr(inbuf, '#', inbuf_size);
  int packetend = packetend_ptr - inbuf;
  assert('$' == inbuf[0]);
  char request = inbuf[1];
  char *payload = (char *)&inbuf[2];
  inbuf[packetend] = '\0';

  uint8_t checksum = 0;
  for (int i = 1; i < packetend; i++)
    checksum += inbuf[i];
  assert(checksum == (hex(inbuf[packetend + 1]) << 4 | hex(inbuf[packetend + 2])));

  switch (request)
  {
  case 'g':
  {
    regs_struct regs;
    uint8_t regbuf[20];
    tmpbuf[0] = '\0';
    ptrace(PTRACE_GETREGS, 0, NULL, &regs);
    for (int i = 0; i < ARCH_REG_NUM; i++)
    {
      mem2hex((void *)(((size_t *)&regs) + regs_map[i].idx), regbuf, regs_map[i].size);
      regbuf[regs_map[i].size * 2] = '\0';
      strcat(tmpbuf, regbuf);
    }
    write_packet(tmpbuf);
    break;
  }
  case 'G':
  {
    regs_struct regs;
    for (int i = 0; i < ARCH_REG_NUM; i++)
    {
      hex2mem(payload, (void *)(((size_t *)&regs) + regs_map[i].idx), regs_map[i].size * 2);
      payload += regs_map[i].size * 2;
    }
    ptrace(PTRACE_SETREGS, 0, NULL, &regs);
    write_packet("OK");
    break;
  }
  case 'H':
    write_packet("OK");
    break;
  case 'm':
  {
    size_t maddr, mlen, mdata;
    sscanf(payload, "%x,%x", &maddr, &mlen);
    if (mlen * SZ * 2 > 0x20000)
    {
      puts("Buffer overflow!");
      exit(-1);
    }
    for (int i = 0; i < mlen; i += SZ)
    {
      errno = 0;
      mdata = ptrace(PTRACE_PEEKDATA, 0, (void *)(maddr + i), NULL);
      if (errno)
      {
        sprintf(tmpbuf, "E%02x", errno);
        break;
      }
      mdata = restore_breakpoint(maddr, sizeof(size_t), mdata);
      mem2hex((void *)&mdata, tmpbuf + i * 2, (mlen - i >= SZ ? SZ : mlen - i));
    }
    tmpbuf[mlen * 2] = '\0';
    write_packet(tmpbuf);
    break;
  }
  case 'M':
  {
    size_t maddr, mlen, mdata;
    sscanf(payload, "%x,%x", &maddr, &mlen);
    if ((payload = strchr(payload, ':')) == NULL) {
      write_packet("OK");
      break;
    }
    payload++;
    for (int i = 0; i < mlen; i += SZ)
    {
      if (mlen - i >= SZ)
        hex2mem(payload + i * 2, (void *)&mdata, SZ);
      else
      {
        mdata = ptrace(PTRACE_PEEKDATA, 0, (void *)(maddr + i), NULL);
        hex2mem(payload + i * 2, (void *)&mdata, mlen - i);
      }
      ptrace(PTRACE_POKEDATA, 0, (void *)(maddr + i), (void *)mdata);
    }
    write_packet("OK");
    break;
  }
  case 'q':
    process_query(payload);
    break;
  case 'v':
    process_vpacket(payload);
    break;
  case 'X':
  {
    size_t maddr, mlen, mdata;
    int new_len;
    sscanf(payload, "%x,%x:", &maddr, &mlen);
    if ((payload = strchr(payload, ':')) == NULL) {
      write_packet("OK");
      break;
    }
    payload++;
    new_len = unescape(payload, (char *)packetend_ptr - payload);
    assert(new_len == mlen);
    for (int i = 0; i < mlen; i += SZ)
    {
      if (mlen - i >= SZ)
        memcpy((void *)&mdata, payload + i, SZ);
      else
      {
        mdata = ptrace(PTRACE_PEEKDATA, 0, (void *)(maddr + i), NULL);
        memcpy((void *)&mdata, payload + i, mlen - i);
      }
      ptrace(PTRACE_POKEDATA, 0, (void *)(maddr + i), (void *)mdata);
    }
    write_packet("OK");
    break;
  }
  case 'Z':
  {
    size_t type, addr, length;
    sscanf(payload, "%x,%x,%x", &type, &addr, &length);
    if (type == 0 && sizeof(break_instr))
    {
      bool ret = set_breakpoint(0, addr, length);
      if (ret)
        write_packet("OK");
      else
        write_packet("E01");
    }
    else
      write_packet("");
    break;
  }
  case 'z':
  {
    size_t type, addr, length;
    sscanf(payload, "%x,%x,%x", &type, &addr, &length);
    if (type == 0)
    {
      bool ret = remove_breakpoint(0, addr, length);
      if (ret)
        write_packet("OK");
      else
        write_packet("E01");
    }
    else
      write_packet("");
    break;
  }
  case '?':
    write_packet("S05");
    break;
  default:
    write_packet("");
  }

  inbuf_erase_head(packetend + 3);
}

void get_request()
{
  int first = true;

  while (!terminate)
  {
    if (read_packet(first) < 0) {
      printf("Aborted\n");
      ptrace(PTRACE_KILL, 0, 0, 0);
      break;
    }
    if (first) {
      printf("Connected\n");
      first = false;
    }
    process_packet();
    write_flush();
  }
}

extern struct dos_comline *_cmdline;
static const char *target = NULL;
static struct dos_comline target_cmdline;

static void help(char *argv[])
{
  printf(
    "Usage: %s [<options>] <target> [<target args>..]\n"
    "Options:\n"
    "  -s<speed> : set serial speed\n"
    "  -i<mode>  : select interrupt mode (0-2)\n"
    "  -b<addr>  : ELF binary base address\n"
    , argv[0]);
  exit(1);
}

int main(int argc, char *argv[])
{
  int ac;
  char *speed = "";

  for (ac = 1; ac < argc; ac++) {
    if (argv[ac][0] == '-') {
      /* command option */
      switch (argv[ac][1]) {
      case 's':
        speed = &argv[ac][2];
        break;
      case 'i':
        intrmode = atoi(&argv[ac][2]);
        break;
      case 'b':
        target_base = strtol(&argv[ac][2], NULL, 0);
        break;
      case 'D':
        debuglevel++;
        break;
      default:
        help(argv);
      }
    } else {
      target = argv[ac];
      break;
    }
  }

  if (target == NULL)
    help(argv);

  int p;
  bool f = false;
  for (p = 0; p < _cmdline->len; p++) {
    if (!f) {
      if (_cmdline->buffer[p] == ' ')
        continue;
      f = true;
      if (--ac < 0)
        break;
    } else {
      if (_cmdline->buffer[p] != ' ')
        continue;
      f = false;
    }
  }
  target_cmdline.len = _cmdline->len - p;
  memset(target_cmdline.buffer, 0, sizeof(target_cmdline.buffer));
  memcpy(target_cmdline.buffer, &_cmdline->buffer[p], target_cmdline.len);

  remote_prepare(speed);

  _iocs_b_super(0);
  target_offset = target_load(target, &target_cmdline, NULL) - target_base;

  if ((int)target_offset < 0) {
    printf("Target %s load error\n", target);
    exit(1);
  }

  printf("Target %s waiting for connection...", target);
  fflush(stdout);
  get_request();
  return 0;
}
