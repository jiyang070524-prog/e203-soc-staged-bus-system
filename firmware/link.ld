/* 文件路径：SoC总线集成/firmware/cold_start/cold_start_link.ld
 * 用途：cold-start 单任务固件的独立链接脚本副本。
 * 来源：SoC总线集成/firmware/systembus_link.ld（内容相同，仅注释差异；SHA 记在 INTERFACE_READY.json）。
 * 记忆布局：ITCM 64KB 放代码/只读常量；DTCM 64KB 放数据/栈；DTCM 高地址固定为状态邮箱。
 */
OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
  itcm (rxai!w) : ORIGIN = 0x80000000, LENGTH = 64K
  dtcm (wxa!ri) : ORIGIN = 0x90000000, LENGTH = 64K
}

SECTIONS
{
  .text : {
    . = ALIGN(4);
    *(.text.entry)
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
    . = ALIGN(4);
  } > itcm

  .data : {
    . = ALIGN(4);
    *(.data .data.*)
    *(.sdata .sdata.*)
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
    . = ALIGN(4);
  } > dtcm

  /DISCARD/ : {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.riscv.attributes)
  }
}
