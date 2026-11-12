/* Secure image: bank 1 flash, the lower (Secure) half of SRAM1. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x0C000000, LENGTH = 0x00040000
    RAM   (rwx) : ORIGIN = 0x30000000, LENGTH = 0x00020000
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
        *(.text*)
        *(.rodata*)
    } > FLASH

    .data :
    {
        _sidata = LOADADDR(.data);
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
