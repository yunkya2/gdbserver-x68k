/*
 * Copyright (C) 2023-2025 Yuichi Nakamura (@yunkya2)
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
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <errno.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include "ptrace.h"
#include "pthreadlib.h"

extern int debuglevel;
extern int intrmode;
extern int ctrlc;

/****************************************************************************/

/* デバッグ対象アプリのコンテキスト */

static uint32_t ustack[1024];       // user stack
static uint32_t sstack[1024];       // supervisor stack
static struct pt_regs target_regs;  // デバッグ対象アプリのレジスタ
static struct dos_psp *target_psp;  // デバッグ対象アプリのプロセス管理ポインタ
static volatile uint8_t intarget = false; // デバッグ対象アプリを実行中か

int current_tid = -1;               // 現在実行中のスレッドID
pthread_internal_t *main_pi = NULL; // マルチスレッドアプリの場合のメインスレッド内部構造体

/* gdbserver本体のコンテキスト */

static uint32_t gdb_ustack[256];    // デバッグ対象から見た親プロセスのuser stack
static uint32_t gdb_sstack[256];    // 〃 supervisor stack (デバッグ対象がexitしてからgdbserverに戻るまでに使用)
static uint32_t gdb_regs[16];       // gdbserverのレジスタ (デバッグ対象から戻るために使用)
static struct dos_psp *gdb_psp;     // gdbserverのプロセス管理ポインタ
static int gdb_breakck;             // 設定変更前のブレークチェックフラグ

/****************************************************************************/

/* デバッグ対象の動作中にgdbserverが処理する例外ベクタ一覧 */
static const int gdbvect[] = {
  0x08,         // Bus error
  0x0c,         // Address error
  0x10,         // Illegal instruction
  0x14,         // Division by zero
  0x18,         // CHK, CHK2 instruction
  0x1c,         // TRAPV instruction
  0x20,         // Privilege violation
  0x24,         // Trace
  0x7c,         // NMI
  0xa4,         // Trap #9
};

#define N_GDBVECT     (sizeof(gdbvect) / sizeof(gdbvect[0]))

/* 例外ベクタ保存と処理ルーチンへのジャンプ処理 */
static struct vectdata {
  uint32_t vectaddr;      // 例外ベクタアドレス
  uint32_t oldvect;       // デバッグ対象が実行を開始する前の値

  /* gdbserverが捕捉するデバッグ対象の例外処理 */
  /* 発生した例外のベクタアドレスをスタックに積んでcommon_trapへジャンプする */
  uint16_t instr0;        // 0x3f3c   move.w #data,%sp@-
  uint16_t instr_data0;
  uint16_t instr1;        // 0x4ef9   jmp addr
  uint32_t instr_addr1;
} vectdata[N_GDBVECT];

/* デバッグ対象アプリで発生する例外の共通処理 */
static void common_trap(void)
{
  __asm__ volatile(
    "ori.w #0x0700,%sr\n"         // disable interrupt
    "movem.l %d0-%d7/%a0-%a6,target_regs\n"   // デバッグ対象のレジスタ保存
    "moveq.l #0,%d0\n"
    "move.w %sp@+,%d0\n"                      // 発生した例外のベクタアドレス (do_cont()/do_singlestep()の戻り値)
    "lea.l target_regs,%a0\n"
    "move.l %usp,%a1\n"
    "move.l %a1,%a0@(72)\n"       // save usp
    "move.l %sp,%a0@(76)\n"       // save ssp
    "movem.l gdb_regs,%d2-%d7/%a2-%a7\n"      // gdbserverのレジスタを復帰
  );
}

/* デバッグ対象アプリで発生するNMI処理 (INTERRUPT SW) */
static void nmi_trap(void)
{
  __asm__ volatile(
    "move.b #0x0c,0xe8e007\n"                 // NMI RESET
    "tst.b intarget\n"
    "bne 1f\n"
    "addq.l #2,%sp\n"                         // スタックに積んだベクタアドレスを捨てる
    "rte\n"                                   // デバッグ対象を実行中でなければ何もしない

    "1:\n"
    "tst.b 0xcbc.w\n"             // CPU type
    "beq 2f\n"
    "clr.w %sp@-\n"
    "2:\n"
    "pea.l %pc@(common_trap)\n"               // common_trap へrteでジャンプ
    "move.w #0x2700,%sp@-\n"                  // (NMI割り込み状態を解除するため)
    "rte\n"         
  );
}

/* デバッグ対象アプリの終了処理 */
static void common_exit(void)
{
  __asm__ volatile(
    // デバッグ対象がDOSコール実行時のCTRL+Cで中断・終了すると、DOSコール呼び出し前の
    // 特権モードで終了処理(common_exit())が呼ばれるので、スーパバイザモードに入り直す
    "suba.l %a1,%a1\n"
    "moveq.l #0xffffff81,%d0\n"
    "trap #15\n"                              // IOCS _B_SUPER

    "ori.w #0x0700,%sr\n"                     // disable interrupt
    "movem.l gdb_regs,%d2-%d7/%a2-%a7\n"      // gdbserverのレジスタを復帰
    "moveq.l #-1,%d0\n"                       // do_cont()/do_singlestep()の戻り値
  );
}

/* メイン以外のスレッド終了処理 */
/* 実際の終了はメインスレッドが行うのでそれまでスリープする */
static void thread_exit(void)
{
  while (1) {
    _dos_change_pr();
  }
}

/* デバッグ対象アプリの例外ベクタ初期化 */
static void init_vector(void)
{
  for (int i = 0; i < N_GDBVECT; i++) {
    vectdata[i].vectaddr = gdbvect[i];

    /* ベクタ番号をスタックに積んでcommon_trapへジャンプする */
    vectdata[i].instr0 = 0x3f3c;            // move.w #data,%sp@-
    vectdata[i].instr_data0 = gdbvect[i];
    vectdata[i].instr1 = 0x4ef9;            // jmp addr
    vectdata[i].instr_addr1 = (uint32_t)common_trap;
    if (gdbvect[i] == 0x7c) {
      /* NMIの場合はnmi_trapへ */
      vectdata[i].instr_addr1 = (uint32_t)nmi_trap;
    }
  }
}

/* デバッグ対象アプリの例外ベクタを設定 */
static void set_vector(void)
{
  for (int i = 0; i < N_GDBVECT; i++) {
    vectdata[i].oldvect = *(uint32_t *)vectdata[i].vectaddr;
    *(uint32_t *)vectdata[i].vectaddr = (uint32_t)&vectdata[i].instr0;
  }
}

/* デバッグ対象アプリの例外ベクタを復元 */
static void restore_vector(void)
{
  for (int i = 0; i < N_GDBVECT; i++) {
    *(uint32_t *)vectdata[i].vectaddr = vectdata[i].oldvect;
  }
}

/* 命令キャッシュのあるCPUの場合にキャッシュをフラッシュ */
static void flash_icache(void)
{
  if (*(uint8_t *)0xcbc > 1) {
    __asm__ volatile(
      "moveq.l #0xffffffac,%%d0\n"
      "moveq.l #0x03,%%d1\n"
      "trap #15\n"
      : : : "%%d0", "%%d1"
    );
  }
}

/****************************************************************************/

/* スレッドを一時停止させる際の状態保存用 */
static struct {
  unsigned char wait_flg;   // スレッドが待ち状態かどうか
  unsigned char counter;    // スケジューリングカウンタ
  long wait_time;           // 残り待ち時間
} thread_stat[32];

/* スレッドの停止時に他のスレッドの実行を一時停止する */
static void suspend_thread(void)
{
  if (PRC_TABLE == NULL) {
    return;   // PROCESS= が有効でない
  }

  current_tid = get_current_tid();
  if (main_pi == NULL) {
    // メインスレッドのpthread内部構造体アドレスを得る
    for (int i = 0; i <= PRC_COUNT; i++) {
      struct dos_prcptr *prc = get_prcptr(i);
      if (prc->buf_ptr == NULL) {
        continue;   // 指定IDのスレッドはない
      }
      pthread_internal_t *pi = (pthread_internal_t *)(prc->buf_ptr);
      if (pi->magic != PTH_MAGIC) {
        continue;   // libpthreadで生成したスレッドではない
      }
      main_pi = pi->main_pi;
      break;
    }
  }

  // スレッドデバッグ中に他スレッドが動かないようにするため、
  // 同一プロセス内の自分以外の全スレッドの状態を保存して一時停止する
  pthread_internal_t *pi;
  int i = 0;
  for (pi = main_pi; pi; pi = pi->next, i++) {
    if (pi->tid == current_tid) {
      continue;   // 自分自身の状態は変更しない
    }
    struct dos_prcptr *prc = get_prcptr(pi->tid);
    // 状態を保存
    thread_stat[i].wait_flg = prc->wait_flg;
    thread_stat[i].counter = prc->counter;
    thread_stat[i].wait_time = prc->wait_time;
    // スリープ状態に変更
    prc->wait_flg = 0xff;
    prc->wait_time = 0;
  }
}

/* スレッドの再開時に他のスレッドの実行を再開する */
static void resume_thread(void)
{
  if (PRC_TABLE == NULL) {
    return;   // PROCESS= が有効でない
  }

  pthread_internal_t *pi;
  int i = 0;
  for (pi = main_pi; pi; pi = pi->next, i++) {
    if (pi->tid == current_tid) {
      continue;   // 自分自身の状態は変更しない
    }
    struct dos_prcptr *prc = get_prcptr(pi->tid);
    // 状態を復元
    prc->wait_flg = thread_stat[i].wait_flg;
    prc->counter = thread_stat[i].counter;
    prc->wait_time = thread_stat[i].wait_time;
  }
}

/* デバッグを終了させるためメインスレッドを実行状態、他スレッドを待ち状態に設定 */
static void set_thread_terminate(void)
{
  if (PRC_TABLE == NULL) {
    return;   // PROCESS= が有効でない
  }

  pthread_internal_t *pi;
  unsigned char new_wait_flg = 0x00;  // メインスレッドは待ち状態でない
  for (pi = main_pi; pi; pi = pi->next) {
    struct dos_prcptr *prc = get_prcptr(pi->tid);
    prc->wait_flg = new_wait_flg;
    prc->wait_time = 0;
    new_wait_flg = 0xff;  // 他のスレッドは待ち状態
  }
}

/****************************************************************************/

uint32_t sccrx_vect;            // 設定変更前のSCC Rx割り込みベクタ

/* デバッグ対象実行中に使われるSCC Rx割り込み */
/* 実行中にシリアルポートからデータが来たら本来の受信処理を実行後、
 * sccrx_intr_after()へジャンプする
 */
__attribute__((noinline))
static void sccrx_intr(void)
{
  __asm__ volatile(
    "tst.b 0xcbc.w\n"             // CPU type
    "beq 1f\n"
    "clr.w %sp@-\n"
    "1:\n"
    "pea.l %pc@(sccrx_intr_after)\n"
    "move.w %sr,%sp@-\n"
    "move.l sccrx_vect,%sp@-\n"
  );
}

/* デバッグ対象実行中に使われるSCC Rx割り込み後処理 */
/* シリアルポートの入力データをチェックし、CTRL+Cだったら
 * デバッグ対象をSIGINTで停止させる
 */
__attribute__((interrupt, used))
static void sccrx_intr_after(void)
{
  __asm__ volatile(
    "move.l %d0,%sp@-\n"

    "moveq.l #0x33,%d0\n"
    "trap #15\n"                // IOCS _ISNS232C
    "tst.l %d0\n"
    "beq 9f\n"

    "moveq.l #0x32,%d0\n"
    "trap #15\n"                // IOCS _INP232C
    "cmpi.b #0x03,%d0\n"
    "bne 9f\n"

    // CTRL+Cを示すベクタ番号(0)をスタックに積んでcommon_trapへジャンプする
    "move.l %sp@+,%d0\n"
    "clr.w %sp@-\n"
    "bra common_trap\n"

    "9:\n"
    "movem.l %sp@+,%d0\n"
  );
}

/* SCC Rx割り込みのベクタを設定 */
static void set_sccrx_vector(void)
{
  sccrx_vect = *(uint32_t *)0x0170;
  *(uint32_t *)0x0170 = (uint32_t)sccrx_intr;
  *(uint32_t *)0x0174 = (uint32_t)sccrx_intr;
}

/* SCC Rx割り込みのベクタを復元 */
static void restore_sccrx_vector(void)
{
  *(uint32_t *)0x0170 = sccrx_vect;
  *(uint32_t *)0x0174 = sccrx_vect;
}

/****************************************************************************/

/* 68000 例外スタックフレーム */
struct frame_m68000_excep {
  uint16_t sr;
  uint32_t pc;
};

/* 68000 バスエラースタックフレーム */
struct frame_m68000_buserr {
  uint16_t funccode;
  uint32_t address;
  uint16_t instr;
  uint16_t sr;
  uint32_t pc;
};

/* 68010～ フレームタイプごとのスタックフレームサイズ */
static int frame_m680x0_fixup[] = {
  8, 8, 12, 12, 16, 0, 0, 60, 58, 20, 32, 92, 24, 0, 0, 0
};

/* 例外発生の後処理 */
static int decode_trap(int trapvect, char *msg)
{
  int res = 0;
  *msg = '\0';

  if (debuglevel > 0) {
    printf("trap 0x%x\n", trapvect);
    uint16_t *ssp = (uint16_t *)target_regs.ssp;
    for (int i = 0; i < 32; i++) {
      if ((i % 8) == 0)
        printf("%08x:", (int)ssp);
      printf(" %04x", *ssp++);
      if ((i % 8) == 7)
        printf("\n");
    }
  }

  /* スタックフレームに積まれたSR,PCを引き上げる */
  struct frame_m68000_excep *fe = (struct frame_m68000_excep *)target_regs.ssp;
  target_regs.sr = fe->sr & 0x7fff;
  target_regs.pc = fe->pc;
  if (debuglevel > 0) {
    if (trapvect != 0x08 && trapvect != 0x0c)
      printf("sr=0x%x pc=0x%x\n", target_regs.sr, target_regs.pc);
  }

  switch (trapvect) {
  case 0x08:          // Bus error
  case 0x0c:          // Address error
    if (*(uint8_t *)0xcbc == 0) {     // 68000
      struct frame_m68000_buserr *fp = (struct frame_m68000_buserr *)target_regs.ssp;
      target_regs.ssp += sizeof(*fp);
      target_regs.sr = fp->sr & 0x7fff;
      target_regs.pc = fp->pc;
      sprintf(msg, "%s error by %s memory access of 0x%08x.",
              trapvect == 0x08 ? "Bus" : "Address",
              fp->funccode & 0x10 ? "READ" : "WRITE",
              fp->address);
    } else {                          // 68010～
      target_regs.ssp += sizeof(struct frame_m68000_excep);
      int type = (*(uint16_t *)target_regs.ssp >> 12) & 0xf;
      uint32_t faultaddr = 0;
      int rw = 0;
      if (debuglevel > 0) {
        printf("exception frame type %d\n", type);
      }
      switch (type) {
      case 0x4:   // 68060 access error stack frame
        faultaddr = *(uint32_t *)(target_regs.ssp + 0x08 - 0x06);
        rw = *(uint32_t *)(target_regs.ssp + 0x0c - 0x06) & (1 << 24);
        break;
      case 0x7:   // 68040 access error stack frame
        faultaddr = *(uint32_t *)(target_regs.ssp + 0x14 - 0x06);
        rw = *(uint16_t *)(target_regs.ssp + 0x0c - 0x06) & 0x0100;
        break;
      case 0x8:   // 68010 bus and address error stack frame
        faultaddr = *(uint32_t *)(target_regs.ssp + 0x0a - 0x06);
        rw = *(uint16_t *)(target_regs.ssp + 0x08 - 0x06) & 0x0100;
        break;
      case 0xa:   // 68020,68030 short bus cycle stack frame
      case 0xb:   // 68020,68030 long bus cycle stack frame
        faultaddr = *(uint32_t *)(target_regs.ssp + 0x10 - 0x06);
        rw = *(uint16_t *)(target_regs.ssp + 0x0a - 0x06) & 0x0040;
        break;
      }
      sprintf(msg, "%s error by %s memory access of 0x%08x.",
              trapvect == 0x08 ? "Bus" : "Address",
              rw ? "READ" : "WRITE",
              faultaddr);
    }
    res = 10;         // SIGBUS
    break;

  case 0x10:          // Illegal instruction
    target_regs.ssp += sizeof(struct frame_m68000_excep);
    res = 4;          // SIGILL
    break;

  case 0x14:          // Division by zero
  case 0x18:          // CHK, CHK2 instruction
  case 0x1c:          // TRAPV instruction
    target_regs.ssp += sizeof(struct frame_m68000_excep);
    res = 8;          // SIGFPE
    break;

  case 0x20:          // Privilege violation
    target_regs.ssp += sizeof(struct frame_m68000_excep);
    res = 11;         // SIGSEGV
    break;

  case 0x00:          // CTRL+C
  case 0x7c:          // NMI
    target_regs.ssp += sizeof(struct frame_m68000_excep);
    res = 2;          // SIGINT
    break;

  case 0xa4:          // Trap #9 instruction
    target_regs.pc -= 2;
    /* fall through */
  case 0x24:          // Trace
    target_regs.ssp += sizeof(struct frame_m68000_excep);
    res = 5;          // SIGTRAP
    if (ctrlc) {            // ステップ実行中にCTRL+Cを受信したらSIGINTを返す
      res = 2;        // SIGINT
      ctrlc = false;
    }
    break;
  }

  if (*(uint8_t *)0xcbc > 0) {    // 68010～
    int type = (*(uint16_t *)target_regs.ssp >> 12) & 0xf;
    target_regs.ssp += frame_m680x0_fixup[type] - sizeof(struct frame_m68000_excep);
  }

  return res;
}

/* デバッグ対象の実行を再開 */
/* デバッグ対象に何らかの例外が発生する or プログラム終了すると戻る
 * out: >=0 例外発生による停止 値はベクタアドレス
 *      <0  プログラム終了
 */
// デバッグ対象から戻ってくるとこの関数から復帰する所から実行を再開するのでinlineにしない
__attribute__((noinline))
static int do_cont(void)
{
  __asm__ volatile(
    "movem.l %d2-%d7/%a2-%a7,gdb_regs\n"  // gdbserverのレジスタを保存
    "lea.l target_regs,%a0\n"
    "move.l %a0@(72),%a1\n"
    "move.l %a1,%usp\n"           // restore usp
    "move.l %a0@(76),%sp\n"       // restore ssp
    "tst.b 0xcbc.w\n"             // CPU type
    "beq 1f\n"
    "clr.w %sp@-\n"
    "1:\n"
    "move.l %a0@(68),%sp@-\n"     // restore pc
    "move.w %a0@(66),%sp@-\n"     // restore sr
    "movem.l %a0@,%d0-%d7/%a0-%a6\n"
    "rte\n"
  );
}

/* デバッグ対象をステップ実行 */
/* デバッグ対象に何らかの例外が発生する or プログラム終了すると戻る
 * out: >=0 例外発生による停止 値はベクタアドレス
 *      <0  プログラム終了
 */
// デバッグ対象から戻ってくるとこの関数から復帰する所から実行を再開するのでinlineにしない
__attribute__((noinline))
static int do_singlestep(void)
{
  __asm__ volatile(
    "movem.l %d2-%d7/%a2-%a7,gdb_regs\n"  // gdbserverのレジスタを保存
    "lea.l target_regs,%a0\n"
    "move.l %a0@(72),%a1\n"
    "move.l %a1,%usp\n"           // restore usp
    "move.l %a0@(76),%sp\n"       // restore ssp
    "tst.b 0xcbc.w\n"             // CPU type
    "beq 1f\n"
    "clr.w %sp@-\n"
    "1:\n"
    "move.l %a0@(68),%sp@-\n"     // restore pc
    "move.w %a0@(66),%d0\n"
    "ori.w #0x8000,%d0\n"         // enable TRACE bit
    "move.w %d0,%sp@-\n"          // restore sr
    "movem.l %a0@,%d0-%d7/%a0-%a6\n"
    "rte\n"
  );
}

/* SRの実行モードを見てA7にUSP or SSPを設定する */
static void update_sp(void)
{
  if (target_regs.sr & 0x2000)
    target_regs.a[7] = target_regs.ssp;   // Supervisor stack
  else
    target_regs.a[7] = target_regs.usp;   // User stack
}

/* SRの実行モードを見てA7をUSP or SSPに設定する */
static void retrieve_sp(void)
{
  if (target_regs.sr & 0x2000)
    target_regs.ssp = target_regs.a[7];   // Supervisor stack
  else
    target_regs.usp = target_regs.a[7];   // User stack
}

/* メモリのread/write (バスエラーチェックつき) */
/* in:  addr  = 読み書きアドレス
 *      datap = 読み書きデータへのポインタ　　
 *      size  = 0:byte / 1:word / 2:long word
 *      rw    = 0:read / 1:write
 * out: 0:正常に読み書きできた / 1:バスエラー発生
*/
static int memory_rw(void *addr, void *datap, int size, int rw)
{
  int res;
  __asm__ volatile(
    "move.w %%sr,%%sp@-\n"
    "ori.w #0x0700,%%sr\n"        // disable interrupt
    "movea.l %%sp,%%a5\n"
    "movea.l 0x0008.w,%%a4\n"     // save bus error vector
    "movea.l 0x000c.w,%%a3\n"     // save address error vector
    "move.l #8f,0x0008.w\n"
    "move.l #8f,0x000c.w\n"

    "tst.w %4\n"
    "beq 1f\n"
    "exg %1,%2\n"                 // write
    "1:"

    "tst.b %3\n"
    "bne 2f\n"
    "move.b %1@,%2@\n"            // byte access
    "bra 7f\n"

    "2:"
    "cmpi.b #1,%3\n"
    "bne 3f\n"
    "move.w %1@,%2@\n"            // word access
    "bra 7f\n"

    "3:"
    "move.l %1@,%2@\n"            // long word access
    "7:\n"
    "moveq.l #0,%0\n"
    "bra 9f\n"

    "8:\n"                        // bus error
    "moveq.l #-1,%0\n"
    "movea.l %%a5,%%sp\n"
    "9:\n"
    "move.l %%a4,0x0008.w\n"      // restore bus error vector
    "move.l %%a3,0x000c.w\n"      // restore bus error vector
    "move.w %%sp@+,%%sr\n"        // restore interrupt
    : "=d"(res) : "a"(addr), "a"(datap), "d"(size), "d"(rw)
    : "%%d1", "%%a3", "%%a4", "%%a5"
  );
}

/****************************************************************************/

int ptrace(int request, int pid, void *addr, void *data)
{
  int result = 0;

  switch (request) {
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA:
      /* addrのアドレスのメモリを読んで値を返す
       * バスエラーならerrnoにEFAULTを設定
       */

      if (!((int)addr & 1)) {
        uint32_t d;
        if (memory_rw(addr, &d, 2, 0))
          errno = EFAULT;
        result = d;
      } else {
        uint8_t d0, d1, d2, d3;
        if (memory_rw(addr,     &d0, 0, 0) ||
            memory_rw(addr + 1, &d1, 0, 0) ||
            memory_rw(addr + 2, &d2, 0, 0) ||
            memory_rw(addr + 3, &d3, 0, 0))
          errno = EFAULT;
        result = (d0 << 24) | (d1 << 16) | (d2 << 8) | d3;
      }
      break;

    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA:
      /* addrのアドレスにdataの値を書き込む
       * バスエラーならerrnoにEFAULTを設定
       */

      if (!((int)addr & 1)) {
        uint32_t d = (uint32_t)data;
        if (memory_rw(addr, &d, 2, 1))
          errno = EFAULT;
      } else {
        uint8_t d0 = (uint32_t)data >> 24;
        uint8_t d1 = (uint32_t)data >> 16;
        uint8_t d2 = (uint32_t)data >> 8;
        uint8_t d3 = (uint32_t)data;
        if (memory_rw(addr,     &d0, 0, 1) ||
            memory_rw(addr + 1, &d1, 0, 1) ||
            memory_rw(addr + 2, &d2, 0, 1) ||
            memory_rw(addr + 3, &d3, 0, 1))
          errno = EFAULT;
      }
      break;

    case PTRACE_GETREGS:
      /* デバッグ対象アプリのレジスタ値をdataにコピーする
       */
      if (current_tid < 0 || pid == current_tid) {
        // 対象が現在実行中のスレッドならgdbserverが保存したレジスタ値を返す
        update_sp();
        memcpy(data, &target_regs, sizeof(target_regs));
      } else {
        // 他のスレッドならスレッド管理構造体から値を得る
        struct dos_prcptr *prc = get_prcptr(pid);
        struct pt_regs *dest = data;
        memcpy(dest->d, prc->d_reg, sizeof(prc->d_reg));
        memcpy(dest->a, prc->a_reg, sizeof(prc->a_reg));
        dest->sr = prc->sr_reg;
        dest->pc = prc->pc_reg;
        dest->usp = prc->usp_reg;
        dest->ssp = prc->ssp_reg;
        dest->a[7] = dest->sr & 0x2000 ? dest->ssp : dest->usp;
      }
      break;

    case PTRACE_SETREGS:
      /* dataをデバッグ対象アプリのレジスタ値として設定する
       */
      struct pt_regs *newregs = data;
      if (current_tid < 0 || pid == current_tid) {
        // 対象が現在実行中のスレッドならgdbserverが保存したレジスタ値を変更する
        for (int i = 0; i < 8; i++) {
          target_regs.d[i] = newregs->d[i];
          target_regs.a[i] = newregs->a[i];
        }
        target_regs.sr = newregs->sr;
        target_regs.pc = newregs->pc;
        retrieve_sp();
      } else {
        // 他のスレッドならスレッド管理構造体の値を変更する
        struct dos_prcptr *prc = get_prcptr(pid);
        memcpy(prc->d_reg, newregs->d, sizeof(prc->d_reg));
        memcpy(prc->a_reg, newregs->a, sizeof(prc->a_reg));
        prc->sr_reg = newregs->sr;
        prc->pc_reg = newregs->pc;
        prc->usp_reg = newregs->usp;
        prc->ssp_reg = newregs->ssp;
      }
      break;

    case PTRACE_KILL:
      /* デバッグ対象アプリを終了させる
       */
      if (main_pi == NULL) {
        // バックグラウンドプロセスがない場合
        // PCをDOS _EXITに設定して実行を再開する
        target_regs.pc = *(uint32_t *)0x1800;         // DOS _EXIT
        target_regs.ssp = (uint32_t)target_psp->ssp;
        target_regs.sr = 0x2000;
      } else {
        // バックグラウンドプロセスがある場合
        // メインスレッドのPCをDOS _EXITに設定、それ以外のスレッドはスリープする
        pthread_internal_t *pi;
        // メインスレッドに設定する値
        uint32_t new_pc = *(uint32_t *)0x1800;        // DOS _EXIT
        uint32_t new_ssp = (uint32_t)target_psp->ssp;

      __asm__ ("ori.w #0x0700,%sr");
        for (pi = main_pi; pi; pi = pi->next) {
          struct dos_prcptr *prc = get_prcptr(pi->tid);
          if (pi->tid == current_tid) {
            // 現在実行中のスレッドの場合 gdbserverが保存したレジスタ値を変更する
            target_regs.pc = new_pc;
            target_regs.sr = 0x2000;
            if (new_ssp) {
              target_regs.ssp = new_ssp;
            }
          } else {
            // 現在実行中のスレッドでない場合 スレッド管理構造体の値を変更する
            prc->pc_reg = new_pc;
            prc->sr_reg = 0x2000;
            if (new_ssp) {
              prc->ssp_reg = new_ssp;
            }
          }

          // メインスレッド以外の設定
          new_pc = (uint32_t)thread_exit; // DOS _CHANGE_PRを実行し続ける
          new_ssp = 0;
        }
        set_thread_terminate();
      }
      /* fall through */

    case PTRACE_CONT:
    case PTRACE_SINGLESTEP:
      /* デバッグ対象アプリの実行を再開する
       * 戻り値 >=0 なら例外発生による停止
       *              *addr: 発生した例外に対応するgdbのシグナル番号
       *              *data: 発生した例外に関するメッセージ文字列
       *       <0   プログラム終了
       *              *addr: 終了コード
       */
      flash_icache();
      if (request != PTRACE_KILL) {
        _dos_breakck(gdb_breakck);
      }
      __asm__ ("ori.w #0x0700,%sr");
      resume_thread();
      set_sccrx_vector();
      intarget = true;
      result = (request != PTRACE_SINGLESTEP) ? do_cont() : do_singlestep();
      intarget = false;
      restore_sccrx_vector();
      suspend_thread();
      _dos_breakck(2);

      if (result >= 0) {     // デバッグ対象の実行が中断された
        // 例外スタックフレームの内容を引き上げる
        *(uint32_t *)addr = decode_trap(result, data);

        // do_cont()/do_singlestep()からは割り込み禁止状態で戻ってくるので、
        // モードに応じて適切な状態に変更する
        if (intrmode == 0) {                // -i0: デバッガ内では常に割り込み許可
          __asm__ ("andi.w #0xf8ff,%sr");
        } else if (intrmode == 1) {         // -i1: デバッグ対象の割り込み許可状態を引き継ぐ
          __asm__ ("move.w %0,%%sr" : : "d"((target_regs.sr & 0x0700)|0x2000));
        }                                   // -i2: デバッガ内では常に割り込み禁止
      } else {              //　デバッグ対象の実行が終了した
        if (addr != NULL)
          *(uint32_t *)addr = _dos_wait();  // 終了コードを引き取る
        // デバッガ自体を終了するため、変更したベクタを復帰しプロセス管理ポインタをデバッガに切り替え
        restore_vector();
        _dos_setpdb(gdb_psp);
        _dos_breakck(gdb_breakck);
        __asm__ ("andi.w #0xf8ff,%sr");
      }
      break;
  }
  return result;
}

/* デバッグ対象アプリをメモリにロードする */
int target_load(const char *name, struct dos_comline *cmdline, const char *env)
{
  int res;

  gdb_breakck = _dos_breakck(-1);
  _dos_breakck(2);
  init_vector();
  memset(&target_regs, 0, sizeof(target_regs));
  memset(&gdb_regs, 0, sizeof(gdb_regs));
  gdb_psp = _dos_getpdb();

  /* DOS _EXECでアプリをロードし、A0～A4レジスタをtarget_regsに設定 */
  __asm__ volatile(
    "move.l %1,%%sp@-\n"
    "move.l %2,%%sp@-\n"
    "move.l %3,%%sp@-\n"
    "move.w #0x0001,%%sp@-\n"
    ".short 0xff4b\n"           // DOS _EXEC
    "lea %%sp@(14),%%sp\n"
    "move.l %%d0,%0\n"
    "movem.l %%a0-%%a4,%4@\n"
    : "=d"(res)
    : "r"(env), "r"(cmdline), "r"(name), "a"(target_regs.a)
    : "%%d0","%%a0","%%a1","%%a2","%%a3","%%a4", "memory"
  );

  if (res >= 0) {
    /* デバッグ対象実行時のレジスタ値を設定 */
    target_regs.pc = target_regs.a[4];
    target_regs.usp = (uint32_t)ustack + sizeof(ustack);
    target_regs.ssp = (uint32_t)sstack + sizeof(sstack);

    /* デバッグ対象実行時のプロセス管理ポインタを設定 */
    target_psp = (struct dos_psp *)(target_regs.a[0] + 0x10);
    target_psp->exit = common_exit;
    target_psp->ctrlc = common_exit;
    target_psp->errexit = common_exit;
    target_psp->usp = (void *)gdb_ustack + sizeof(gdb_ustack);
    target_psp->ssp = (void *)gdb_sstack + sizeof(gdb_sstack);
    target_psp->sr = 0x0000;
    target_psp->abort_sr = 0x0000;

    /* 例外ベクタを設定してプロセス管理ポインタをデバッグ対象に切り替え */
    _dos_setpdb(target_psp);
    set_vector();

    printf("Target addr:0x%x usp:0x%x ssp:0x%x\n", target_regs.a[0] + 0x100, target_regs.usp, target_regs.ssp);
    if (debuglevel > 0) {
      extern char _start;
      printf("Debugger addr:%p\n", &_start);
    }

    /* デバッグ対象アプリのロードアドレスを返す */
    return target_regs.a[0] + 0x100;
  }

  return res;
}
