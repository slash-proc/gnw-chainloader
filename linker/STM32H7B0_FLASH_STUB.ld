ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 40K
  RAM (xrw)       : ORIGIN = 0x20001000, LENGTH = 124K /* Reserve 4K at 0x20000000 for Retro-Go state */
}

_estack = ORIGIN(RAM) + LENGTH(RAM) - 8; /* Reserve top 8 bytes for boot magic */

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  /* Services table published to the RAM app at a fixed address right after the
     64-byte vector table (see common/stub_services.h). The ASSERT below locks
     the address so it can't silently drift if the vector table changes. */
  .stub_services :
  {
    . = ALIGN(4);
    KEEP(*(.stub_services))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}

/* Keep the services table at the address the app hardcodes (STUB_SERVICES_ADDR). */
ASSERT(ADDR(.stub_services) == 0x08000040,
       "stub_services moved: update STUB_SERVICES_ADDR in common/stub_services.h");
