OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(chainloader)

MEMORY
{
  RAM      (xrw) : ORIGIN = __RAM_ORIGIN__, LENGTH = __RAM_LENGTH__
  FLASH    (xr ) : ORIGIN = 0x8000000,  LENGTH = 128K
}

SECTIONS
{
    .text : {
        . = __STOCK_ROM_END__;
        . = ALIGN(4);
        KEEP(*(.text.chainloader))
        KEEP(*(.text.read_buttons))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* used by the startup to initialize data */
    _sidata = LOADADDR(.data);

    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT> FLASH

    .bss : {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM
}
