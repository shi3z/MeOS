# Optional native keyboard input

Text/IME delivery remains the default. A cooperative client may claim the
`keyboard_raw` lease. Tab5 then switches only while claimed to Normal matrix
mode (0x20 events: pressed bit 7, row bits 4..6, column bits 0..3). On release it
verifies/restores the usual HID mode and clears queued/repeat input. USB keyboards
use their existing boot reports; no USB mode is changed. No firmware is flashed
to the keyboard. All-zero HID release events lack per-key identity, so HID text
mode cannot provide reliable simultaneous movement keys.

The producer keeps a bounded event queue plus held-key bitmap. Brief down/up
pairs survive a slower render frame. Modifier events precede new key presses and
follow key releases. Old producer epochs are rejected after ownership changes.
I/O loss, rollover and overflow release keys; an overflow may require lifting
and re-pressing keys. The lease copies state, never dereferences an app callback.

The Tab5 base map is factual HID usage data from M5Stack's matrix specification.
In native mode Aa acts as a held Shift, Sym selects the symbol layer, and Ctrl/Alt
are actual held modifiers. Text mode retains its previous capitalization/IME
behavior. Native clients must release their lease in app cleanup.

Primary references:
- https://github.com/m5stack/M5Tab5-Keyboard-Internal-FW
  `code/Keyboard_APP/Core/User/keyboard/user_keyboard_handle.c`
  and `Core/User/sys/user_sys.h` (matrix and packed-event layout).
- https://m5stack.github.io/M5Unit-KEYBOARD/unit___tab5_keyboard_8hpp_source.html
- Copied MIT notice: `LICENSE.tab5-reference`.

Validation runs via `sh tools/quake/host_test.sh`: production driver mode changes,
independent matrix releases, modifier/chord and brief-tap routing, USB reports and
rollover, producer epochs, input error cleanup and normal text restoration.
These host checks do not replace physical accessory/USB qualification.

Reference snapshot: bbfd579182563e63acd867333e6138f3e5e6e047
