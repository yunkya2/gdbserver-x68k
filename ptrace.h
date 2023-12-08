#ifndef _PTRACE_H
#define _PTRACE_H

#include <stdint.h>
#include <x68k/dos.h>

extern int gdbserver_debug;

int target_load(const char *name, struct dos_comline *cmdline, const char *env);
int ptrace(int request, int pid, void *addr, void *data);

#define PTRACE_PEEKTEXT         1
#define PTRACE_PEEKDATA         2
#define PTRACE_POKETEXT         4
#define PTRACE_POKEDATA         5
#define PTRACE_CONT             7
#define PTRACE_KILL             8
#define PTRACE_SINGLESTEP       9
#define PTRACE_GETREGS          12
#define PTRACE_SETREGS          13

struct pt_regs {
    uint32_t d[8];      // 0
    uint32_t a[8];      // 32
    uint32_t sr;        // 64
    uint32_t pc;        // 68
    uint32_t usp;       // 72
    uint32_t ssp;       // 76
};

#endif /* _PTRACE_H */
