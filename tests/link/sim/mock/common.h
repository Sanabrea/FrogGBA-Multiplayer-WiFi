/* Test-only replacement for FrogGBA's common.h: just what the link code needs. */
#ifndef COMMON_H
#define COMMON_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;
#define ALIGN_DATA __attribute__((aligned(4)))
typedef enum { CPU_ALERT_NONE = 0, CPU_ALERT_HALT = 1, CPU_ALERT_SMC = 2, CPU_ALERT_IRQ = 4 } CPU_ALERT_TYPE;
enum {
  REG_SIODATA32_L = 0x120 / 2, REG_SIODATA32_H = 0x122 / 2,
  REG_SIOMULTI0 = 0x120 / 2, REG_SIOMULTI1 = 0x122 / 2, REG_SIOMULTI2 = 0x124 / 2, REG_SIOMULTI3 = 0x126 / 2,
  REG_SIOCNT = 0x128 / 2, REG_SIOMLT_SEND = 0x12A / 2, REG_SIODATA8 = 0x12A / 2, REG_RCNT = 0x134 / 2,
  REG_TM2D = 0x108 / 2, REG_TM2CNT = 0x10A / 2, REG_TM3D = 0x10C / 2, REG_TM3CNT = 0x10E / 2,
  REG_IE = 0x200 / 2, REG_IF = 0x202 / 2, REG_IME = 0x208 / 2
};
enum { REG_PC = 15, REG_CPSR = 20, CPU_HALT_STATE = 30 };
extern u32 reg[64];
#define MAX_PATH 512
extern char main_path[MAX_PATH];
typedef int SceUID;
typedef struct { int st_mode; unsigned st_attr; long long st_size; } SceIoStat;
#define PSP_O_WRONLY 0x0002
#define PSP_O_CREAT  0x0200
#define PSP_O_APPEND 0x0100
#define PSP_O_TRUNC  0x0400
SceUID sceIoOpen(const char *file, int flags, int mode);
int sceIoWrite(SceUID fd, const void *data, unsigned size);
int sceIoClose(SceUID fd);
int sceIoGetstat(const char *file, SceIoStat *stat);
extern u16 io_registers[0x400];
extern char gamepak_filename[256];
typedef enum { CURSOR_RTRIGGER, CURSOR_LTRIGGER, CURSOR_UP, CURSOR_DOWN, CURSOR_LEFT, CURSOR_RIGHT,
               CURSOR_SELECT, CURSOR_BACK, CURSOR_EXIT, CURSOR_DEFAULT, CURSOR_NONE } GUI_ACTION_TYPE;
GUI_ACTION_TYPE get_gui_input(void);
#define COLOR15(r, g, b) ((r) | ((g) << 5) | ((b) << 10))
#define COLOR15_TO_32(c) (c)
#define COLOR15_GREEN 0
#define X_POS_CENTER (-1)
#define BG_NO_FILL (-1)
void clear_screen(u32 c);
void print_string(const char *s, s16 x, u16 y, u16 fg, s16 bg);
void flip_screen(u32 vsync);
u32 sceKernelGetSystemTimeLow(void);
int sceKernelDelayThread(u32 us);
#include "serial.h"
#include "link.h"
#include "link_log.h"
#endif
