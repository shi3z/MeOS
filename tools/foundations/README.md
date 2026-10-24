# Kernel foundation reliability

This layer builds on the preceding hardware/stack/audio PRs. Run
`sh tools/host_checks.sh`; `CC=gcc` or `CC=clang` selects the host compiler.
`tools/foundations/host_test.sh` covers the new portable mechanisms.

## Panic evidence

MeOS panic records are schema/checksum validated, bounded and stored in internal
`.noinit` DRAM. Boot reports the firmware commit/reset reason, copies a surviving
record for `crash`, and consumes the retained magic so unrelated resets cannot
replay it. Power-on/brownout records are discarded. This is best effort across
resets retaining DRAM, not flash persistence or exception-frame capture. Panic
output bypasses normal console locks/heap/VFS/display and never scans stacks.
It still depends on intact executing code, TCB metadata and enough current stack.
IDF handles CPU exception dumps separately; no FreeRTOS core-dump compatibility
is assumed.

## I2C and mutexes

Extended system-bus I2C calls return status before unlocking. NACK, SCL timeout
and invalid input are distinct; legacy booleans return false for each failure.
Clock-stretch waits are bounded per SCL release, including STOP; counters count
transactions, not startup bus-recovery attempts. Failed reads can modify part
of the supplied buffer: callers must discard it. A low SCL at START, ACK, read
or STOP is injected in the production-HAL host tests on all three boards.

Mutex priority donation is recalculated from configured priorities to a bounded
fixed point across explicit mutex wait links, distinct from mailbox wait data.
Multiple held locks, transitive chains, handoff, waiter/owner termination and
cycles are tested. Cycles remain deadlocks; inheritance does not detect/resolve
them. `ps` shows effective/base priority. Normal task exit now releases locks,
matching forced termination. All calculations occur under the scheduler lock.

## Cleanup before reclaim

Callbacks are registered and sealed at boot. Hardware callbacks precede window,
file and WebAssembly cleanup. All hooks run even if one fails. Any failure
keeps app memory quarantined and schedules a retry (`apps -l` exposes the last
cleanup error); leases are released and
owner memory reclaimed only after success. Reserved-app abort and task-launch
failure use this same rule. Callbacks must be idempotent; current legacy void
cleanup adapters report success after their existing stop routine returns.
Registry tests and production app lifecycle tests cover failure/retry/reclaim.
Microphone cleanup uses a nonblocking ownership check, so another app's healthy
10-second capture does not block appd and falsely expire its progress lease.

## RTC and UTC

`date` and `rtc` use explicit UTC ISO timestamps (`...Z`), 2000..2099. No NTP,
automatic time setting, full-chip power-on reset, alarms, backup power switching
or charging configuration is added. Runtime wall time is an anchor plus elapsed
monotonic time; changing it never changes scheduler deadlines. `date set` changes
only RAM, `date sync` reads RTC, and only `rtc set` writes hardware. Mutating shell
commands are kernel-only. RTC reads reject stopped/test mode, VLF, invalid BCD,
invalid calendar/weekday and unstable snapshots. Set pauses the clock and
checks all writes. Partial writes leave it stopped/tainted; VLF must actually
clear before restart. Write-zero-to-clear flags use ones for unrelated event
bits to preserve events arriving during the operation. Register 0x1f is never
written. Time-setting is not a substitute for manufacturer power-on setup.

Primary register reference: Epson RX8130CE application manual, sections 13/14:
https://download.epsondevice.com/td/pdf/app/RX8130CE_en.pdf

## Progress and optional reset watchdog

Explicit progress leases have non-reused handles, bounded timeouts and owner
cleanup. Expiry samples monotonic time while holding the same IRQ lock as
renewal, so a concurrent heartbeat cannot appear to come from the future. Ordinary waiting tasks need not subscribe. The app cleanup service is
monitored. A high-priority sleeping supervisor checks leases and scheduler ticks.
Only its task context feeds MWDT0; the timer interrupt never does. Software
inspection is enabled by default; hardware arming is disabled by default.

`M5OS_WATCHDOG=1 sh tools/build.sh tab5 reconfigure build` opts into experimental
P4 hardware reset behavior: cooperative MWDT0 reservation, rejection of an
already-enabled timer, no whole timer-group reset, XTAL 40MHz/40000 for 1ms ticks,
and 2000 ticks to system reset. Expired leases create a panic reason containing
the lease name/owner, then feeding stops. If the supervisor or tick cannot run,
the hardware timeout expires without necessarily creating a new panic record.
P4 LL/HAL source is pinned by the ESP-IDF 5.4.4 build. No FreeRTOS WDT API is used.
Opt-in on S3 is rejected at configure time. An unregistered application hang is
not detected unless it prevents scheduler/service progress.

## Evidence and remaining device gates

Host ASan/UBSan checks cover record corruption, mutex chains, cleanup ordering,
real app abort/launch-failure quarantine, the complete 2000..2099 calendar,
RTC rollover/invalid data/each write failure/flag preservation, lease timing and
stale handles. CI runs GCC and Clang plus Tab5 watchdog-off/on firmware builds.
The workflow has read-only permissions, pinned action commits, no secrets,
no privileged PR event and no flashing. Artifacts contain firmware and symbols.

Physical qualification remains separate: reset retention and panic output;
priority inversion under actual interrupt load; repeated app/driver teardown;
RTC oscillator validity, power retention and explicit set; deliberate scheduler/
IRQ starvation, lease timeout and reset timing with watchdog enabled. Do not
present build/host success as those hardware tests. microSD remains excluded.
