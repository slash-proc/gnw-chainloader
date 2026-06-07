/*
 * Shared linker script for PIE (position-independent) modules.
 *
 * Linked at base 0 with -pie so the linker emits R_ARM_RELATIVE entries into
 * .rel.dyn. The chainloader's loader copies the image to a bump-allocated RAM
 * address and patches every .rel.dyn entry by that base (see system/loader.c).
 *
 * Layout (contiguous, single load segment):
 *   .module_header  (offset 0)
 *   .text/.rodata/.data/.got
 *   .rel.dyn        (kept in the image; bounds exported)
 *   .bss            (NOLOAD; loader zeroes it)
 */

ENTRY(init_module)

PHDRS { load PT_LOAD FLAGS(7); }   /* one RWX segment */

SECTIONS
{
  . = 0;

  .module_header : { KEEP(*(.module_header)) } :load

  .text :
  {
    . = ALIGN(4);
    *(.text.init_module)
    *(.text .text*)
    *(.glue_7) *(.glue_7t)
    *(.rodata .rodata*)
    . = ALIGN(4);
  } :load

  . = ALIGN(4);
  __data_start = .;   /* boundary: .text/.rodata above take the (flash/XIP) text base;
                         .data/.got/.bss below take the RAM data base. See loader.c. */

  /* .got FIRST in the data region, so it sits exactly at __data_start (== data_offset). The GOT
   * base is then data_base, which the loader already computes -- an r9-pic (-msingle-pic-base)
   * module just gets r9 = data_base, no separate got_offset field needed. */
  .got :
  {
    . = ALIGN(4);
    *(.got.plt) *(.got)
    . = ALIGN(4);
  } :load

  .data :
  {
    . = ALIGN(4);
    *(.data .data*)
    . = ALIGN(4);
  } :load

  . = ALIGN(4);
  __rel_start = .;
  .rel.dyn : { *(.rel.init_array) *(.rel.dyn) *(.rel.text*) *(.rel.data*) *(.rel.got) } :load
  __rel_end = .;

  . = ALIGN(4);
  __bss_start = .;
  .bss (NOLOAD) :
  {
    *(.bss .bss*)
    *(COMMON)
    . = ALIGN(4);
  } :load
  __bss_end = .;

  /* Dynamic-linker metadata we don't use — our loader applies .rel.dyn by hand
     using the header, so discard the rest to keep the image lean. */
  /DISCARD/ :
  {
    *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash)
    *(.interp) *(.gnu.version*) *(.ARM.attributes) *(.comment)
  }
}

/* Header field values (offsets/sizes from base 0). */
_module_entry_offset = init_module;
_module_reloc_offset = __rel_start;
_module_reloc_count  = (__rel_end - __rel_start) / 8;   /* sizeof(Elf32_Rel) == 8 */
_module_bss_offset   = __bss_start;
_module_bss_size     = __bss_end - __bss_start;
_module_data_offset  = __data_start;   /* relocs below this offset resolve against text_base,
                                          at/above against data_base (two-base loader, Stage 3b). */
