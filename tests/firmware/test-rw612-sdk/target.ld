/* Linker script for RW612 emulator tests: a FlexSPI NOR boot image, laid out
 * the way the boot ROM expects it (FCB at 0x400, vector table at 0x1000). */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 0x00200000   /* FlexSPI XIP */
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00130000   /* 1.2 MB SRAM */
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_sidata = LOADADDR(.data);

SECTIONS
{
    .flash_config :
    {
        . = 0x400;
        KEEP(*(.flash_conf))
        . = 0x1000;
    } > FLASH

    .isr_vector :
    {
        _image_start = .;
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
        *(.text*)
        *(.rodata*)
        *(.ARM.extab* .gnu.linkonce.armextab.*)
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
    } > FLASH

    .preinit_array :
    {
        __preinit_array_start = .;
        KEEP(*(.preinit_array*))
        __preinit_array_end = .;
    } > FLASH

    .init_array :
    {
        __init_array_start = .;
        KEEP(*(.init_array*))
        __init_array_end = .;
    } > FLASH

    .fini_array :
    {
        __fini_array_start = .;
        KEEP(*(.fini_array*))
        __fini_array_end = .;
    } > FLASH

    .data :
    {
        _sdata = .;
        *(.data*)
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > RAM
}

/* Boot ROM image header fields in the vector table (ivt.c). */
_image_length = LOADADDR(.data) + SIZEOF(.data) - _image_start;
