#ifndef PTHREADLIB_H
#define PTHREADLIB_H

#include <stdint.h>
#include <x68k/dos.h>

/* Human68k ワークエリア */
#define PRC_TABLE   *(struct dos_prcptr **)0x1c50   // スレッド管理構造体テーブル
#define PRC_CURRENT *(struct dos_prcptr **)0x1c54   // 現在のスレッドのスレッド管理構造体
#define PRC_SIZE    0x7c                            // スレッド管理構造体サイズ
                                                    // (sizeof(struct dos_prcptr) ではない)
#define PRC_COUNT   *(uint16_t *)0x1c58             // 最大スレッド数-1

/* 現在のスレッドIDを得る */
#define get_current_tid()    (((int)PRC_CURRENT - (int)PRC_TABLE) / PRC_SIZE)
/* 指定IDのスレッド管理構造体を得る */
#define get_prcptr(tid)      (struct dos_prcptr *)((int)PRC_TABLE + (tid) * PRC_SIZE)


/* 以下は elf2x68k/src/libx68k/libpthread/pthread_internal.h と一致させること */

#define PTH_MAGIC                   0x50746831  // 'Pth1'
typedef uint32_t pthread_t;        // スレッドID

typedef struct pthread_internal {
    struct dos_prcctrl com;             // タスク間通信バッファ

    uint32_t magic;                     // 'Pth1'
    pthread_t tid;                      // スレッドID
    uint32_t stat;                      // スレッド状態 (PTH_STAT_*)

    struct pthread_internal *main_pi;   // メインスレッドのスレッド内部情報へのポインタ
    struct pthread_internal *next;      // 次のスレッド内部情報へのポインタ

    // 以降は未使用
} pthread_internal_t;


/* スレッド管理用変数 */

extern int current_tid;
extern pthread_internal_t *main_pi;

#endif /* PTHREADLIB_H */
