ENTRY(init_driver)

MEMORY
{
  RAM (xrw) : ORIGIN = 0x240A0000, LENGTH = 128K
}

SECTIONS
{
  .text :
  {
    . = ALIGN(4);
    KEEP(*(.text.init_driver))
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    . = ALIGN(4);
  } >RAM

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >RAM

  .data :
  {
    . = ALIGN(4);
    *(.data)
    *(.data*)
    . = ALIGN(4);
  } >RAM

  .bss :
  {
    . = ALIGN(4);
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
  } >RAM
}
