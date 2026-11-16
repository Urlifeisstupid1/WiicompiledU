/*
 * wiicold.ld - fragment that WiiUCold.cmake splices into a copy of wut.ld,
 * ahead of the RPX's own .text, so the code of the "cold" static libraries is
 * claimed here first. wiiu_cold_split.py later moves both sections out of the RPX
 * into cold.bin, which wiicold_loader.c loads at startup.
 *
 *   .wiicold     cold code, linked at the Aroma codegen (JIT) area address, where
 *                it runs. The coldmem region (32 MiB) enforces the size limit.
 *   .wiicold_eh  the cold code's .eh_frame. Not executable, so it is loaded into
 *                ordinary heap memory instead of the codegen area; its link
 *                address (coldehmem) is arbitrary and the loader rebases it.
 *
 * Archives whose file name starts with "libwiicold_" are cold.
 */
   .wiicold : {
      *libwiicold_*.a:*(.text .text.* .gnu.linkonce.t.*)
      . = ALIGN(4);
      __wiicold_code_end = .;
   } > coldmem

   .wiicold_eh : {
      __wiicold_eh_frame_start = .;
      KEEP (*libwiicold_*.a:*(.eh_frame))
      LONG(0)
      __wiicold_eh_frame_end = .;
   } > coldehmem
