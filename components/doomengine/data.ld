/* Loaded by the bootloader like other internal data. Inside the low SRAM used range,
 * between normal initialized data and BSS, so the IDF heap cannot reuse it. */
SECTIONS
{
  .doom_engine_data : ALIGN(16)
  {
    _doom_data_start = ABSOLUTE(.);
    *libdoomengine.a:(.doomengine.data .doomengine.data.* .doomengine.sdata .doomengine.sdata.*)
    . = ALIGN(16);
    _doom_data_end = ABSOLUTE(.);
  } > sram_low
}
INSERT AFTER .dram0.data;
