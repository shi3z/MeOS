# Scheme workspace for Tab5

Run `scheme` from the MeOS shell to open a demonstration source buffer, a graph
pane and a REPL transcript. `scheme /home/example.scm` opens an existing UTF-8
file, or an empty buffer if the file does not exist. Relative command-line paths
are resolved by the shell. Opening the IDE does not execute source automatically.
Source, the graph pane, REPL transcript and minibuffer use the native 24px BIZ UD
font on a flicker-free offscreen framebuffer. Comments, strings and delimiters
have restrained syntax colours while retaining a prose-friendly page.

This is an initial, deliberately bounded Scheme subset, not an R7RS-conforming
implementation or an Emacs Lisp compatibility layer. The interpreter and UTF-8
editing model have no FreeRTOS or libc-I/O dependencies. The UI uses MeOS display,
console, VFS and IME APIs. Workspace storage is allocated in PSRAM and released
on normal exit. Shell integration and UI are Tab5-only.

## Editing and evaluation

| Keys | Action |
|---|---|
| `C-a`, `C-e`, `C-b`, `C-f`, `C-p`, `C-n` | Line start/end, character and line movement |
| Arrow keys, Home, End, Delete, Backspace | UTF-8-aware navigation and deletion |
| `C-x C-s` or `C-s` | Save source |
| `C-x C-b` | Evaluate source buffer in the current environment |
| `C-x C-e` | Evaluate the complete expression immediately before point |
| F2 | Read and evaluate an expression in the minibuffer |
| F1, `C-x x`, or terminal `ESC x` | M-x command minibuffer |
| F3 | Brief command help |
| `C-x 1`, `C-x 2` | Hide/show graph pane |
| `C-_` or `M-x undo` | Undo the last edit; one snapshot, not an undo history |
| `C-g` | Cancel a minibuffer or evaluation; the IME handles composition cancellation first |
| `C-x C-c` or `C-q` | Quit, asking before discarding modified source |

`C-x C-e` finds the preceding expression using a bounded syntax scan: unfinished
enclosing lists are allowed, and comments, strings and quote prefixes are
respected. Unfinished strings or a quote without a following expression are
rejected. Earlier forms are not executed, and source after point is not read.
This uses the subset reader syntax described below.
The keyboard HID layer does not currently encode generic Alt combinations as
Meta, so use F1 or `C-x x` on the physical keyboard. No HID behaviour was changed.

Built-in M-x commands are `eval-buffer`, `eval-last-sexp`, `eval-expression`,
`save-buffer`, `find-file`, `undo`, `reset-environment`, `delete-other-windows`,
`split-window-right`, and `quit`. `find-file` takes an absolute VFS path and
retains the old source if loading fails. There is one source buffer and a bounded
read-only transcript; multiple editable buffers are future work. In the M-x
minibuffer, Tab completes built-ins and global zero-argument Scheme procedures.
An ambiguous prefix expands to its common prefix and shows matching names.
Completion preserves UTF-8 boundaries, including Japanese command names.
Procedure redefinition or `reset-environment` is reflected immediately.

Other M-x names call zero-argument Scheme procedures. Define and evaluate:

```scheme
(define (挨拶)
  (insert-text! "; こんにちは\n"))
```

Then F1, `挨拶`, Enter inserts the comment at point. Scheme errors are reported
in the transcript. Evaluation uses a separate source copy, so an editor extension
can insert text without modifying the source currently being read. Extension
insertion is one undo step. General keymaps, hooks and modes are not yet exposed
to Scheme.

## Japanese input

`Ctrl-Space` and `Alt-backtick` retain their existing IME meanings. The editor and
minibuffer consume an immutable `ime_view`: preedit is inserted into the BIZ UD
visual layout at the cursor, reflowing following text without entering the source.
A second Space opens the same nearby two-column candidate popup as the writing
editor. Uncommitted text stays outside the source and
evaluator. Confirmed text arrives as one bounded UTF-8 transaction, making a
converted phrase one insertion, one repaint and one undo step.
Comments, strings, identifiers, source paths, command names and plot titles can
contain Japanese. Movement, backspace and delete preserve UTF-8 codepoint
boundaries, including four-byte UTF-8; display glyph coverage remains that of the
BIZ UD asset. Combining sequences are not treated as grapheme clusters.
Up/Down follows proportional soft-wrapped screen rows as well as explicit lines.

Cursor columns follow MeOS's existing one-cell-per-codepoint convention. Touch
placement and graph gestures are not implemented yet.

## Language and plotting

Supported forms: `define` (variable and procedure shorthand), lexical `lambda`,
`set!`, `let` (parallel bindings), `if`, `begin`, `and`, `or`, `quote` and `'`.
Closures retain their lexical environments; recursive functions and redefinition
are supported. Only `#f` is false. Procedure parameters are a fixed list of
distinct identifiers. Forms are fully parsed before any are executed; execution
errors do not roll back earlier definitions, output or editor changes.

Built-ins: `+ - * /`, two-argument `= < > <= >=`, `sin cos exp sqrt log abs`,
`cons car cdr list null? pair? not`, `display newline`, `plot`, `insert-text!`.
`pi` is predefined. Trigonometry uses radians; `log` is the natural logarithm.
All numeric values use binary64 with the shared MeOS bounded decimal conversion
(zero or approximately 1e-100..1e100, twelve significant displayed digits).
There are no exact integer/rational or complex numeric types.

```scheme
;; Change the function and evaluate again to explore another curve.
(define (振動 x)
  (* (sin x) (exp (* -0.1 x))))
(plot 振動 0 30 "減衰振動")
```

`plot` accepts a procedure, increasing finite x bounds and an optional title
string. It evaluates 129 samples under the same interpreter limits, then publishes
the complete curve. A failed plot retains the previous curve. The y range is
automatic; the pane shows the x bounds. This is a sampled line plot, not an
adaptive discontinuity-aware graphing engine. Changing a definition requires
calling `plot` again. Nested plotting is rejected. Labels, panning, zooming,
sliders, FFT, typed arrays, camera/audio bindings and hardware acceleration are
future work.

Limits: 16 KiB source, 127 UTF-8 bytes per string or identifier, 4,096 heap cells,
64 reader/evaluator nesting levels and 60,000 evaluation steps per request.
There is no proper tail recursion, macro system, continuation support, dotted
reader syntax, vectors, imports or standard Scheme library. Mark/sweep GC runs
between requests and preserves definitions and captured environments. It does
not collect during one evaluation; large plots/functions can exhaust the heap.
`reset-environment` drops definitions while retaining source and the last plot.

## Files and verification

Saving explicitly overwrites the selected VFS file, loops over short writes and
checks close errors. There is no VFS rename transaction, so an I/O error can leave
a partial file; the in-memory source and modified state are retained. Loading
stages and validates a complete regular UTF-8 file before replacing the buffer.
On Tab5, `/home` persists with a prepared littlefs/COW SD root; otherwise it is
RAM-backed. Separate VFAT cards support explicit read/write or read-only `/sd` mounts.
The following RAM-root caveats apply when SD-root boot is unavailable:
saving to `/home` does not establish reboot persistence.

Run `sh tools/scheme/host_test.sh` for ASan/UBSan interpreter and scripted IDE
integration tests. These exercise lexical closures, repeated GC, Japanese text,
plot publication, bounded recursion, cancellation, 2,500 deterministic malformed
reader inputs, editor extensions, UTF-8 deletion/undo, short VFS I/O, failure
preservation, preceding-expression selection inside unfinished lists, Japanese
command completion, and display/memory cleanup. Physical keyboard, IME overlay placement,
rendering and concurrent floating-point execution still require Tab5 qualification.
`sh tools/sim/scheme_ui_test.sh` additionally replays the BIZ UD minibuffer,
evaluates `(sqrt 81)`, and checks the REPL result frame.

Write and Scheme share an idle BIZ UD atlas cache. The first editor reads it
from microSD; subsequent editor launches reuse it. `mem trim` explicitly
reclaims that cache and the optional 1,170 KiB SD asset-read cache. The next
asset read makes one attempt to recreate its cache; editor allocation pressure
can also reclaim it before retrying. Home trims BIZ UD before starting a
non-editor application.
