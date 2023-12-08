#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>

struct reg_struct
{
    int idx;
    int size;
};

#define ARCH_REG_NUM (sizeof(regs_map) / sizeof(struct reg_struct))

#define SZ 4
#define FEATURE_STR "l<target version=\"1.0\">\
  <architecture>m68k:68000</architecture>\
  <osabi>none</osabi>\
  <feature name=\"org.gnu.gdb.m68k.core\">\
    <reg name=\"d0\" bitsize=\"32\"/>\
    <reg name=\"d1\" bitsize=\"32\"/>\
    <reg name=\"d2\" bitsize=\"32\"/>\
    <reg name=\"d3\" bitsize=\"32\"/>\
    <reg name=\"d4\" bitsize=\"32\"/>\
    <reg name=\"d5\" bitsize=\"32\"/>\
    <reg name=\"d6\" bitsize=\"32\"/>\
    <reg name=\"d7\" bitsize=\"32\"/>\
    <reg name=\"a0\" bitsize=\"32\" type=\"data_ptr\"/>\
    <reg name=\"a1\" bitsize=\"32\" type=\"data_ptr\"/>\
    <reg name=\"a2\" bitsize=\"32\" type=\"data_ptr\"/>\
    <reg name=\"a3\" bitsize=\"32\" type=\"data_ptr\"/>\
    <reg name=\"a4\" bitsize=\"32\" type=\"data_ptr\"/>\
    <reg name=\"a5\" bitsize=\"32\" type=\"data_ptr\"/>\
    <reg name=\"fp\" bitsize=\"32\" type=\"data_ptr\"/>\
    <reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>\
    <reg name=\"ps\" bitsize=\"32\"/>\
    <reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>\
  </feature>\
</target>"

static uint8_t break_instr[] = {0x4e, 0x49};

#define PC 17
#define EXTRA_NUM -1
#define EXTRA_REG -1
#define EXTRA_SIZE -1

typedef struct pt_regs regs_struct;

struct reg_struct regs_map[] = {
    {0, 4},
    {1, 4},
    {2, 4},
    {3, 4},
    {4, 4},
    {5, 4},
    {6, 4},
    {7, 4},
    {8, 4},
    {9, 4},
    {10, 4},
    {11, 4},
    {12, 4},
    {13, 4},
    {14, 4},
    {15, 4},
    {16, 4},
    {17, 4},
};

#endif /* ARCH_H */
