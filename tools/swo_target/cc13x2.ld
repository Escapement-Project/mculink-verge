/* Flash stops a sector short of the end, so a download never erases the CCFG sector (0x56000) and
   the CCFG already on the part, which keeps the debug port enabled, stays as it is. */
MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 0x56000
    SRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x14000
}

ENTRY(reset_handler)

_stack_top = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
    } > FLASH

    .swo_ctl (NOLOAD) : {
        KEEP(*(.swo_ctl))
    } > SRAM

    .data : {
        _data = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > FLASH
    _ldata = LOADADDR(.data);

    .bss (NOLOAD) : {
        _bss = .;
        *(.bss* COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM

    /DISCARD/ : { *(.ARM.exidx*) }
}
