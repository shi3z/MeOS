/* Loaded by the bootloader like other internal data. Inside the low SRAM used range,
 * between normal initialized data and BSS, so the IDF heap cannot reuse it. */
SECTIONS
{
  .quake_engine_data : ALIGN(16)
  {
    _quake_data_start = ABSOLUTE(.);
    *libquakeengine.a:(.quakeengine.data .quakeengine.data.* .quakeengine.sdata .quakeengine.sdata.*)
    . = ALIGN(16);
    _quake_data_end = ABSOLUTE(.);
  } > sram_low
}
INSERT AFTER .dram0.data;
