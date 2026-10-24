# External-BSS heap boundary

With external BSS enabled, the MeOS heap begins at the next 64-byte boundary
after `_ext_ram_bss_end`. Its end remains unchanged, preserving the IDF reserve
at the top of PSRAM. If BSS consumes the MeOS slice, no heap is returned.
Physical RAM no larger than the reserve is rejected before subtraction.

`sh tools/psram/host_test.sh` compiles the production allocator-boundary code
with external BSS enabled and disabled. It tests BSS overlap/exhaustion,
64-byte alignment, the preserved reserved tail, MMU failures and small RAM.
These host fixtures do not establish physical PSRAM/cache behavior.
