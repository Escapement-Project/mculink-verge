/* mps2-an505, secure aliases: code in SSRAM1, data in SSRAM2. The generator writes above both. */
MEMORY {
  code (rx) : ORIGIN = 0x10000000, LENGTH = 4M
  ram (rw)  : ORIGIN = 0x38000000, LENGTH = 1M
}
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > code
  .ARM.exidx : { *(.ARM.exidx*) } > code
  __data_lma = LOADADDR(.data);
  .data : { __data_start = .; *(.data*) . = ALIGN(4); __data_end = .; } > ram AT > code
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > ram
  __stack_top = ORIGIN(ram) + LENGTH(ram);
}
