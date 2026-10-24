# Task stack diagnostics

New tasks reserve a 32-byte low-end canary before their usable internal-SRAM
stack. Requested sizes are rounded down to the 16-byte architecture alignment;
less than 512 bytes, arithmetic overflow and null entries are rejected. The
usable byte count reported by `ps` excludes the canary. The allocation pointer
is retained so normal task reclamation frees the entire block.

Usable bytes are painted before constructing the architecture's initial
context. `ps` reports `used~/size` from the first changed byte: this is a
high-water estimate, including context/interrupt frames. Bytes that happen to
match the paint, unwritten stack reservations and concurrent execution can
hide use; this is not exact peak-SP telemetry. Idle's adopted boot stack is
not painted and reports `n/a`.

Inspection reads at most 128 usable bytes plus the canary per critical section,
then restores the caller's interrupt mask and re-finds the PID before reading
again. A reclaimed/reused task returns `ERR_NOENT`; the API never retains a
stack pointer across an interrupt unlock. `task_snapshot` remains a short TCB
copy and does not scan stacks. Call inspection from ordinary task context.

The scheduler checks the outgoing canary and the incoming canary/saved-SP
bounds before a switch. Detected damage is latched and panics rather than
resuming the damaged task. These are software diagnostics, not guard pages or
memory isolation: damage may already have occurred, large jumps can skip the
canary, and a badly damaged current stack may prevent panic reporting.

Run `sh tools/stack/host_test.sh`. ASan/UBSan tests use the actual RISC-V and
Xtensa initial-context builders and production task creation/inspection code.
They cover allocation/permission/table failures, alignment, initial frame use,
watermarks, guard latching, saved-SP bounds, slot reuse and simulated reclamation
between scan chunks. Hardware preemption/interrupt depth and corrupted-stack
panic behavior still require device validation.
