/* 文件路径：SoC总线集成/firmware/cold_start/cold_start_start.s
 * 用途：cold-start 固件裸机入口副本（与 SoC总线集成/firmware/systembus_start.s 相同逻辑）。
 * 每次任务由测试台整机复位后重新从 0x8000_0000 取指，因此本入口必须自包含：
 * 关全局中断、设 DTCM 栈、调用 main，返回后写退出邮箱并停留 WFI。
 */
.section .text.entry
.globl _start
_start:
    csrci mstatus, 8
    li sp, 0x90001800
    call main

_exit:
    li t0, 0x90001ffc
    sw a0, 0(t0)
1:
    wfi
    j 1b

.section .text
.align 2
