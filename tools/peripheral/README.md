# Cooperative peripheral reservations

A fixed-size, allocation-free registry reserves I2S0 and AHB DMA TX2/RX2 with
stable driver tokens. Multi-resource claims are atomic: a conflicting or invalid
request leaves all resources unchanged. Claims by the same token are idempotent,
not reference counted. Reservation/release preserves nested interrupt masks.

The output driver claims I2S0 + TX2 before touching hardware. Close stops DMA,
disconnects its channel and releases the reservation. The next open always
reinitializes hardware because another client may have changed I2S0 while the
output driver was closed. Failure paths release their reservations too. The
existing playback mutex and app teardown still stop callbacks before freeing
owner memory.

I2S0 is reserved as a whole: its reset/clocks affect both directions. A future
microphone driver can reserve I2S0 + RX2 and must refuse capture during playback.
Duplex needs a shared audio-clock/session layer and is outside this change.

This registry protects cooperating MeOS drivers only. Direct register writes,
IDF-managed clients and legacy drivers are not automatically covered. The
existing hardware DMA-busy check remains. Resource release never stops hardware
or runs automatically on app death; driver teardown must stop it first.

`sh tools/peripheral/host_test.sh` tests conflict rollback, invalid/duplicate IDs,
wrong-owner release, independent resources, reopen and nested IRQ restoration
with ASan/UBSan. `sh tools/hardware/host_test.sh` covers the I2C helpers. Actual
DMA stop/reopen behavior and cross-client hardware use require device testing.
