# Tab5 SDIO receive reassembly

ESP-Hosted streaming mode exposes a byte counter for concatenated frames. A
bounded host transfer may end inside either the 12-byte header or its payload.
Previously MeOS read at most 7,680 logical bytes, dispatched complete frames and
discarded the suffix. Later reads then began in the middle of the discarded
frame. It also bounded frame lengths by the padded DMA size, allowing padding
to masquerade as payload, and encoded the clamped transfer length in CMD53's
address instead of the remaining slave window.

The receive stream now keeps an incomplete suffix in the existing 8 KiB buffer.
The next DMA appends after that suffix; only logical bytes are committed, and
512-byte transfer padding is ignored. The DMA plan reserves space for rounded
reads and supports frames up to 7,680 bytes with payloads up to 4,096 bytes,
preserving the prior payload limit. No extra image-sized buffer or heap
allocation is introduced. The polling loop still performs at most eight reads.
Malformed headers discard the undecodable buffered batch and increment a
counter; this is not general transport-error recovery or checksum negotiation.

The CMD53 address encodes `FIFO_END - remaining`, while DMA length describes
this individual bounded read. This follows Espressif's
[SDIO design](https://github.com/espressif/esp-hosted-mcu/blob/2e3f6c8bd717d67698319a6a24f9f4218d8db068/docs/design/sdio.md)
and [MCU host implementation](https://github.com/espressif/esp-hosted-mcu/blob/2e3f6c8bd717d67698319a6a24f9f4218d8db068/host/mcu/eh_host_mcu_transport/src/eh_host_bus_sdio.c).
MeOS retains its FreeRTOS-free transport and scheduler.

## Diagnostics and reproduction

`wifi stats` reports frames, retained fragments, malformed headers, maximum
pending bytes, register/DMA errors, TX credit failures, physical-interface ICMP
requests/replies and maximum polling/bus-wait intervals. `wifi stats reset`
resets counters only: it preserves pending fragments and SDIO protocol counters.
The ICMP counters include all station traffic, rather than only the test nonce.
Maximum timings are wall time, including blocking and preemption.

```sh
sh tools/wifi/host_test.sh
python3 tools/sdroot/wifi_coexistence_device.py /dev/cu.usbmodemXXXX \
  --reset --settle-seconds 60 --pps 25 100 --reads 1 \
  --burst-packets 12 --diagnostics \
  --log /tmp/meos-wifi-burst.log --report /tmp/meos-wifi-burst.json
```

The host regression first failed with the previous chunk-local parsing logic.
ASan/UBSan now passes a 20-frame burst, every one of 1,465 split positions in a
normal frame, split headers, a maximum-sized frame, 100 irregular mixed streams,
padding exclusion, malformed headers, invalid plans and subsequent clean batches.
The full host suite includes these tests. Simulator replay checks applications
and IME; it does not emulate the SDIO controller.

The optional burst mode sends 1–16 packets together, paced at the requested
average rate (at most 200 pps). The existing one-packet mode remains the default.
This is a different workload from uniformly paced packets: twelve 1,400-byte
payloads can queue more bytes than the host read buffer even at 25 average pps.
The target must still be the address reported by the attached Tab5. Diagnostic
mode rejects transport errors as well as the existing packet-loss and memory
limits. Raw transcripts may contain network identity and stay private. The tool
issues no media-mutation commands; normal background services may persist state.

## Physical comparison, 2026-10-25

Old firmware `14ecd18` and fixed firmware `4503393` ran on the same Tab5/card/AP,
with 60 s of settling while serial remained open. Each group contained twelve
1,400-byte echo payloads. One quiet PAK read preceded the 25/100 average-pps SD,
font and unsaved Write workloads. Diagnostic reset/snapshot commands ran outside
the fixed firmware's traffic phases; the old firmware lacks those counters.

| Workload | Loss at 25 pps, before / after | Loss at 100 pps, before / after |
| --- | ---: | ---: |
| Idle | 34.524% / 0% | 35.333% / 0% |
| PAK CRC read | 46.771% / 0% | 47.042% / 0% |
| Font load/render | 44.444% / 0% | 44.136% / 0% |
| Write size/layout | 50.000% / 0% | 44.271% / 0% |

All 5,880 post-fix echoes were received with valid payloads/checksums. Device
counters independently recorded 5,880 ICMP requests and successful replies.
At 25/100 pps the receive path retained fragments 179/752 times and saw maximum
pending windows of 18,378/17,847 bytes, exercising the corrected split path.
Malformed headers, register/DMA/plan failures, TX credit errors and failed ICMP
replies were zero. At 100 pps the per-phase RTT p95 was 31/65/81/131 ms for
idle/SD/font/Write; maximum polling and bus-wait times were 260/68 ms. These are
measurements, not guaranteed response bounds.

All six PAK reads across both firmware runs matched `74e3b084`. Post-fix SD reads
took 38.432 s without traffic and 40.772 s at 100 average pps. Trimmed PSRAM
returned from 23,135 to 23,119 KiB free, within the unchanged 32 KiB tolerance.
The fixed image is 1,772,448 bytes (`0x1b0ba0`, 56% slot headroom); only OTA slot 1
was flashed, and esptool verified its hash. Exact source/app/log identities and
full phase counters are in `tools/wifi/burst-device-20261025.json`.

This paired experiment reproduces and fixes a framing failure under queued bursts.
It does not identify every possible source of radio loss or establish maximum
bandwidth, power-loss/hot-removal behavior or recovery from failed SDIO transfers.

## Host clock integrity

A supplemental one-packet run recorded a 38,715 ms device CRC operation as
10.397 host-monotonic seconds and incorrectly derived 1,755.5 KiB/s. macOS
sleep/wake was observed in the local power log. That run remains preserved as
`tools/wifi/uniform-invalid-20261025.json` with an invalid-measurement classification;
its timing and network-loss result are not a qualification pass.

The harness now records wall and monotonic elapsed time, checks them during the
traffic worker and at trial checkpoints, and rejects differences over 1 second.
It also compares the device CRC duration with host elapsed time and labels such
failures `INVALID`, separately from a measured loss/transport `FAIL`. PAK throughput
uses the device's CRC duration; host and wall durations remain in the report.
These are CRC-stream timings, including computation and scheduling, rather than
raw controller bandwidth. Offline tests cover normal timing, suspend/clock jumps,
host/device discrepancies and invalid device durations.

On macOS, `caffeinate -i python3 ...` can prevent idle system sleep for the
duration of a test command. It does not change persistent power settings.

With that temporary assertion and clock guards active, the ordinary one-packet
25/100 pps follow-up passed all eight phases: 5,333/5,333 verified echoes and
the same number of device ICMP requests/replies. All three PAK CRCs matched.
At 100 pps, four fragments were retained with up to 13,931 bytes pending, so
the corrected path also ran with single-packet pacing. All transport/error
counters were zero and trimmed memory returned within 16 KiB of baseline.
The complete clock-validated receipt is `tools/wifi/uniform-device-20261025.json`.
