# Tab5 multi-touch snapshots

`tab5_touch_read_points` exposes up to five GT911 or ten ST712x contacts,
limited by caller capacity. IDs are GT911 track IDs or ST712x slot indices;
they are not persistent gesture identities. The original one-point reader
continues to return the first valid contact.

Each report is read once, invalid coordinates are excluded, and failed reads
release contacts. GT911 retains the previous snapshot until its ready bit is
set; a failed acknowledgement clears the snapshot. Callers should have one
polling owner and distribute the snapshot, rather than concurrently polling.
No gestures, full-screen UI integration or I2C speed changes are introduced.

`sh tools/touch/host_test.sh` exercises sparse slots, IDs, coordinate mapping,
caller capacity, legacy reads, frame retention and error paths with ASan/UBSan.
Physical multi-finger operation on both controller types remains unverified.
