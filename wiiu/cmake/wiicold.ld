/*
 * wiicold.ld - fragment that WiiUCold.cmake splices into a copy of wut.ld,
 * ahead of the RPX's own .text, so the code of the "cold" static libraries is
 * claimed here first and placed at the Aroma codegen (JIT) area address.
 * wiiu_cold_split.py later moves this section out of the RPX into an image that
 * wiicold_loader.c copies into the codegen area at startup.
 *
 * Archives whose file name starts with "libwiicold_" are cold. Their .eh_frame
 * goes into the same region so its PC-relative references never cross between
 * the RPX and the cold image.
 */
   .wiicold : {
      *libwiicold_*.a:*(.text .text.* .gnu.linkonce.t.*)
      . = ALIGN(4);
      __wiicold_code_end = .;
      __wiicold_eh_frame_start = .;
      KEEP (*libwiicold_*.a:*(.eh_frame))
      LONG(0)
      __wiicold_eh_frame_end = .;
   } > coldmem
