# MeOS uBASIC adaptation

Based on Adam Dunkels' uBASIC, BSD-3-Clause:
https://github.com/adamdunkels/ubasic
Upstream revision: cc07193c231e21ecb418335aba5b199a08d4685c

This is a substantially modified port, not an unmodified upstream copy.
The original tokenizer / recursive-descent statement architecture is retained;
implementation was rewritten where necessary for bounded MeOS shell execution.
Original copyright/license notices are retained in derived source and headers.

Changes:

- Binary64 variables A-Z and arrays instead of upstream's configured char values.
- Case-insensitive, bounded lexer: whitespace, complete keyword boundaries,
  unterminated strings and oversized literals produce errors.
- Numeric range and divide/modulo by zero are checked; integer-only boundaries
  are validated before conversion, with int64 remainder avoiding INT_MIN % -1 UB.
- Unary signs, <=, >=, <> and REM supported; 16 expression recursion levels.
- A complete fixed 128-entry line index replaces malloc/free and slow jump scans.
- Missing targets, unmatched NEXT/RETURN and stack exhaustion produce errors.
- FOR/NEXT: checked integer +1 by default; explicit STEP supports floating/negative
  increments, zero-iteration skipping and four nested loops.
- GOSUB: ten return addresses, including a return to EOF.
- Callback output replaces printf; local setjmp/longjmp error recovery replaces
  exit. Recovery is entirely inside one ubasic_run call and does not cross tasks.
- No PEEK/POKE/CALL, memory access callbacks or file APIs.
- ubasic_run executes one source line; the session checks cancellation between
  lines and stops after 100000 executed lines. Individual lines are bounded by
  the session input size and parser depth, including skipped loops.
- The single-instance interpreter is owned by the shell. This is not a sandbox
  or a reentrant API. All source storage remains alive while execution runs.

Dialect limits: no string variables, multidimensional arrays,
colon-separated statements, nested inline IF, or THEN <line number>. Use IF ... THEN GOTO <line number>. Each line has exactly
one statement, optionally an inline IF with THEN/ELSE statements. Both branches
are lexically scanned, so lexical errors in an unexecuted branch can be reported.
PRINT supports literal strings, numeric expressions, comma (one space), and
semicolon (no separator / suppress trailing newline). RUN resets all variables;
immediate mode preserves variables. NEW clears variables and source.
Assignments/output before a later syntax error are not rolled back.
Line numbers: 1..65535, ascending and unique in the engine source.
Ordinary literals/values are bounded binary64 decimals with e/E notation.
FOR counters without STEP retain signed int32 semantics and error on overflow.
Explicit STEP enables floating-point counters, bounds and direction.

The session adds sorted numbered-line editing, LIST/RUN/NEW/HELP/BYE, and
immediate execution by wrapping a statement as source line 1. Direct GOTO/GOSUB
does not jump into the stored program; use RUN to execute stored lines.
Live program storage is RAM only, max 128 lines and 127 bytes per input line.
The session now dispatches LOAD/SAVE/EDIT to a VFS adapter outside the interpreter.
LOAD stages source before committing; SAVE overwrites and is not atomic. BYE
discards the live program. File persistence depends on the mounted filesystem. MeOS allocates the session from PSRAM first, with normal heap
fallback and an explicit out-of-memory error.

Validation: sh tools/basic/host_test.sh (ASan/UBSan), including malformed-input
campaign and session/engine checks. Firmware build evidence does not qualify
real keyboard/serial interruption, physical boards or persistence.


INPUT / DIM extension:

- DIM creates explicitly declared zero-based arrays, inclusive upper bounds,
  single-letter names, one dimension, shared fixed pool of 1024 binary64 cells (8192 bytes).
  Scalars and arrays with the same name are independent. Bounds, dimension
  reuse, aggregate capacity and every subscript are checked. No heap allocation.
- INPUT targets one scalar or array element, optionally preceded by a literal
  prompt and semicolon/comma. It validates the statement before prompting, then
  pauses the engine between ubasic_run calls. An INPUT on the last source line
  remains unfinished until a reply is supplied or execution is stopped.
- ubasic_supply_input accepts signed decimal floating-point text, rejects other input
  without modifying the target, and resumes after a valid reply. Line I/O and
  retry/cancellation belong to the session/console adapter, outside setjmp scope.
- The shell has 128-byte bounded typeahead so cancellation polling does not eat
  the first character of INPUT. CRLF, backspace, Ctrl-U, clipped/invalid lines,
  buffer overflow and Ctrl-C are handled. Replies do not enter shell history.
- NEW, RUN, successful LOAD and a new session reset scalar/array values and
  dimensions. Immediate execution preserves them. SAVE persists source only.
- Host tests include the INPUT/array state machine, numeric/integer boundaries, pool and
  subscript bounds, malformed syntax, retry/cancel, source/array preservation on
  LOAD failure, console typeahead, and an input/array sum program. Real-device
  keyboard/serial qualification is still separate from tests and builds.


Graphics extension:

- SCREEN 0/1, CLS [RGB565], PSET x,y,RGB565, LINE x0,y0,x1,y1,RGB565,
  TEXT x,y,"literal",RGB565, WAIT milliseconds dispatch to a host callback.
  The engine validates argument structure/color/mode/wait bounds before dispatch.
- The Tab5 adapter checks display readiness/ownership and physical coordinates,
  uses direct display primitives, and accepts printable ASCII TEXT at scale 2.
  No caller-supplied pointer or framebuffer is exposed to BASIC code.
- BASIC and WebAssembly share a cooperative IRQ-protected display claim. Other
  legacy drawing apps are not all covered; this is not display isolation.
- WAIT uses <=10 ms sleep slices and cancellation polling. A cancellation flag
  is handled by the engine/session without crossing tasks with longjmp.
- The session closes graphics on every execution exit, including error, input
  failure, cancellation and line budget. INPUT first returns to the console.
  Immediate graphics is scoped to that one immediate statement; stored programs
  with WAIT are the intended way to view graphics.
- Host tests exercise parsing, drawing dispatch, bounds, readiness/claim refusal,
  console restoration, interruptible wait and sample execution. Physical display
  behavior and legacy-app coexistence are separate qualification work.


Binary64 migration:

- Scalar/array storage, expression evaluation and INPUT/PRINT use double.
  Decimal parsing and 12-significant-digit formatting live in kernel/lib/number.c
  and are shared with calc. No strtod/stdio/locale/errno dependency is introduced.
  Nonzero magnitudes are bounded approximately to 1e-100..1e100; arithmetic is
  binary floating point and comparisons are exact, not epsilon-based.
- Division now retains fractions, a compatibility change from the integer port.
  %, & and | accept only exact integer values in signed int32 range. Integer
  conversion is range/NaN/fraction checked before any C floating-to-int cast.
- GOTO/GOSUB targets, dimensions/subscripts, default FOR bounds/counters and every graphics
  numeric argument pass through the same strict integer conversion. Fractional
  coordinates/delays are rejected, not rounded. FOR without STEP remains integer +1.
- Scientific functions and exponentiation were added in a later host-tested
  extension described below.
- The shell currently enables BASIC on Tab5 only. The S3 FPU/context boundary is
  still unqualified; CoreS3/Cardputer return an explicit unavailable message.
- Tests cover the updated division semantics, decimal/scientific values,
  invalid/range input, arrays, integer boundaries and regression of files/graphics.
  Shared conversion has 10000 host-oracle checks and binary64 compile-time checks.
  Audited Tab5 engine/tokenizer/number objects contain no hardware FP instructions;
  this is not real-device concurrent-task qualification.


Scientific extension (host and isolated full-firmware builds checked):

- Shared kernel/lib/number_math.c implements checked SIN/COS/TAN, SQRT, natural
  LOG, LOG10, EXP, ABS and real powers. Calculator spellings and restrictions
  stay compatible; BASIC LOG/LN mean natural log, LOG10 base ten, SQR/SQRT square
  root. All functions take one parenthesized expression; PI is a constant.
- BASIC ^ is right associative and binds above unary minus. Positive bases accept
  real exponents; negative bases require integer exponents -1000..1000. Domain
  and range checks precede libm calls to avoid its error/errno paths.
- RAD is BASIC's default. DEG/RAD persist in immediate mode, resetting on NEW,
  RUN and successful LOAD. E remains a variable; use EXP(1) for Euler's number.
- Host regressions cover precedence, nested functions, domains, angle modes and
  comparisons against host libm. Individual changed Tab5 sources compile into
  temporary objects; subsequent isolated full firmware builds also pass without
  regenerating the shared build or touching hardware.
- The reusable serial qualification driver is tools/basic/device_test.py. Its
  first physical attempt ended before BASIC tests after an unexpected USB reset
  during parallel Doom/Scheme/BASIC device work. No runtime pass is claimed.


INT/FIX and explicit STEP extension:

- INT uses floor and FIX uses trunc, returning checked binary64 values through
  the common math backend. Host-call integer and range checks remain unchanged.
- FOR ... STEP captures a nonzero floating-point increment once, supports either
  direction and zero-iteration skip. FOR without STEP retains integer semantics.
- NEXT detects non-advancing increments and range failure. Explicit floating
  endpoints are snapped with min(8*epsilon*scale, abs(step)*1e-9) tolerance;
  snapping is disabled when the current counter already equals the bound.
  Normal relation comparisons do not use this tolerance.
- The normal execution-line budget/cancellation remain in force. Tests cover
  fractional endpoints, negative direction, nested skips, default compatibility,
  zero/no-progress/range errors and the sine_plot.bas graphics dispatch.
- Host suites and isolated full-firmware builds are used; no shared build output,
  board reset or flash is touched during parallel device work.


Integration / capture qualification:

- A detached dirty-source snapshot builds the complete normal and Doom-enabled
  firmware in separate directories. The combined ELF retains BASIC, Scheme and
  Doom entries. Image/source hashes, compiler/link warnings and host-suite results
  are recorded in the local build-tab5-basic/qualification.json artifact.
- Doom linker warnings remain; this is compile/link qualification, not proof of
  runtime coexistence or panel/keyboard behavior.
- tools/basic/render.sh runs this interpreter and the graphics adapter to SVG
  without a board. WAIT is instantaneous and TEXT uses a host monospace font.
  Capture excludes the console repaint and is not a hardware emulator. Program
  errors preserve a previous output file. render_test.py covers frame contents,
  markup escaping and failure handling.


Optional persistent VFS extension (outside the interpreter):

- fs/programs and user/basic/programs_tab5.c add explicit /programs mount/init
  commands for an opt-in 1 MiB partition at 0xC00000. No default-table edit,
  boot-time mount, actual board reset, flash or initialization is performed.
- Per-file dual banks, generation/CRC checks, readback and last-word commit keep
  complete old/new versions. A failed/ambiguous transaction freezes writes until
  remount. Rejected buffered writes abort close without truncating the old file.
- The VFS has permission checks, snapshot readers, one writer, 16 names of at most
  23 ASCII bytes and 20416 bytes per file. Metadata is allocated only when needed,
  preferring PSRAM; it does not consume a full static SRAM catalog at boot.
- ROM access is bounded to a validated partition; provisioning refuses nonblank
  media. First provisioning interruption may require external recovery. CRC is
  not authenticity and this is not a new hardware isolation boundary.
- Simulated flash fault tests, VFS/BASIC save-remount-load, adapter guards and
  config preparation pass. Optional-layout normal/Doom-enabled firmware builds
  are separate from real-device reboot/power-cut qualification, which is pending.
