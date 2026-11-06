# Plan: M8, Owned windows, dialogs, charts, and JadeMem

M8 ports JadeUI's dialogs and charts, with what they stand on, and ends with JadeMem ported
whole and sampling the real machine: the first real application in Astra. Version 0.8.0 when it
is done.

Written 2026-11-06 from a reading of JadeUI's `dialogs` and `chart` packages, the dialog half
of `Chrome.kt`, the `treemap` package, the GLFW backend's dialogs, and JadeMem's three modules,
against `lib/Jade` as M7 left it.

## What M8 holds

The plan (gui-plan.md, section 7) gave M8 "the dialog-side settings of `Window`, `DialogSession`
and the eight dialogs, async file dialogs (N2), then `chart`", ending with JadeMem's gui module.
Reading JadeMem showed that its gui cannot come alone: it calls nearly all of its `core` module,
and its `native` module calls about twenty Windows functions through Java's foreign-function
API. Four pieces were added for it, at the user's choice (2026-11-06):

1. **Astra calls functions in DLLs**: a foreign-function interface, so that JadeMem's native
   module ports nearly as written, and every later program has the same door to the system.
2. **`treemap`** comes forward from M9: JadeMem's Disk tab is one.
3. **Canvas gains arcs, wedges and a line width** from tier N3: a pie chart is wedges, a line
   chart's lines are 2 units wide.
4. **`Regex`** (tier N5) comes forward from M9: JadeMem's ETW guard keeps its protected session
   names as patterns.

| Kotlin | Lines | Astra | Holds |
| --- | --- | --- | --- |
| `dialogs/*.kt` | 1,401 | `lib/Jade/Dialogs/` | `DialogSession`, the ten sessions behind the eight dialogs, `JadeDialogs` |
| `Chrome.kt`, the dialog half | 840 | `Chrome.astra`, `Dialogs/` | the dialog interfaces, their specs and results, `DialogHandle`, the `No…` dialogs |
| `sheet/Completion.kt`, part | 107 | `lib/Jade/Completion.astra` | `CompletionPopup`, `Completer`: the value-and-format prompt completes |
| `chart/*.kt` | 774 | `lib/Jade/Chart/` | `ChartData`, `ChartSeries`, `ChartRenderer`, `ChartView`, the builders |
| `treemap/*.kt` | 552 | `lib/Jade/Treemap/` | `TreemapNode`, the layout, `TreemapView` |
| the GLFW file dialogs | 160 | `Window.astra`, C++ | `SystemFileDialog`, on Windows' own dialogs |
| JadeMem `native` | 2,106 | JadeMem `native/` | through Astra's foreign functions |
| JadeMem `core` | 2,689 | JadeMem `core/` | sampling, attribution, drill-downs, the guard, reports |
| JadeMem `gui` | 3,831 | JadeMem `gui/` | the application |

About 1,650 lines of JadeUI's tests carry over (dialogs, charts, treemap), and about 4,500 of
JadeMem's (its `core` tests on its fake system, its `ViewTest`, `RecorderTest`).

**Where JadeMem goes.** Its own repository, `D:\Projects\JadeMem`, beside JadeGen, with one
folder per Kotlin module (`native`, `core`, `gui`) and its tests, built with `astra build`. Jade
and Astra stay in this repository.

## Order

Each step is committed with its tests, carried over where they exist. Only the Clang Windows
build is checked step by step; the full check (MSVC, ASan, stress, Linux) comes at the end.

### Part one: what everything else stands on

1. **The standard library's gaps** that JadeMem and the dialogs meet: `Bytes` reads and writes
   of 64-bit ints and floats (8.1e), the environment's variables, the program's own process
   id, the date and time where the program runs (with the formatting JadeMem's file names and
   log use), the roots of the file system with their total and free space, and the computer's
   name. Each named as Kotlin and Java name it where the port reads better that way, and
   asked of the user where a name is new.
2. **`Regex`** (N5): Java's syntax, as Kotlin's `Regex` has it (classes, groups and named
   groups, alternation, greedy, lazy and possessive repeats, anchors, word boundaries,
   lookahead and lookbehind, back references; the options `IGNORE_CASE`, `MULTILINE`,
   `DOT_MATCHES_ALL`, `LITERAL`, `COMMENTS`), matching by code point. Kotlin's API:
   `matches`, `containsMatchIn`, `find`, `findAll`, `matchEntire`, `replace` (with `$1` and
   with a lambda), `replaceFirst`, `split`, `MatchResult` with `value`, `range`, `groupValues`,
   `groups`, `destructured`, `next()`; and the string forms (`s.matches(re)`, `s.replace(re, …)`,
   `s.split(re)`, `s.contains(re)`, `"…".toRegex()`). The engine in C++, compiled once per
   pattern, with a public Astra API as every C++ piece has. Tests from Kotlin's documented
   behaviour and JadeUI's uses.
3. **Canvas: a line width, arcs and wedges.** `canvas.lineWidth` for every outline and line;
   `fillArc`/`drawArc` (an arc's outline, or a pie wedge filled) in angles as Java2D takes them;
   in the graphics card's canvas, the software canvas (`ImageCanvas`) and the recording one
   (`DrawOp`), drawn alike. Shapes gain what charts hit-test with: `contains` and `bounds` for
   `Oval` and a new `Arc`, besides `Rect`. Tests: the pixels of each, at scales 1 and 2.
4. **Foreign functions.** Load a library (`kernel32`, `libc`), find a function, call it with
   whole numbers, pointers, and (later) floats, and read what it gives. Structures are blocks of
   native memory, read and written by offset, as Java's `MemorySegment` is; text goes in and out
   as UTF-16 (Windows) or UTF-8. A call that may wait lets the other threads run. A sandbox
   refuses foreign calls, and a built program says it makes them. The design (its names, how
   memory is freed, what a wrong call can do) is written up and put to the user before it is
   built.
5. **File dialogs** (N2): Windows' own Open, Save and Choose Folder dialogs, through
   `IFileOpenDialog`/`IFileSaveDialog`, on a thread of their own, the answer brought back on
   the window's thread; installed as `Jade.fileDialog` when a window is shown, as the clipboard
   is. Filters and the starting folder as JadeUI passes them. Tests answer from a script, as the
   GLFW backend's hidden mode does (`ScriptedFileDialog`).

### Part two: dialogs

6. **`DialogSession`** and the windows it opens: one per key (asked again, the open one comes
   forward and is updated), drafts kept on cancel and offered again, fields followed while
   untouched and offered when touched ("Use it" / "Keep mine"), detaching from a subject that
   went away, Enter and Escape by the key filter, `Jade.openDialogs()` and
   `Jade.closeAllDialogs()`. A dialog's owner is the window in front (`Jade.frontWindow`),
   which a program may give instead. What the window still lacks for dialogs: owned windows
   passing to their owner's owner when it closes first, and the keyboard going back to the
   owner when a dialog closes.
7. **The prompts**: `prompt` (one line or several, live, with links) and
   `promptValueAndFormat` (with completion).
8. **The rest of the eight**: `choose`, `pick`, `check`, `edit` (forms), `show` (options),
   the image chooser (its preview an Astra `Image`), the find panel and the source window;
   `JadeDialogs.install()`. Tests: `JadeDialogsTest` (20 of its 22; the two others are about the
   forms that wait, which Astra does not have), and of `BlockingDialogsTest` the five that are
   about the asynchronous forms. JadeUI's four small bugs found in reading (a checklist's draft
   broken by an item holding a line break, the image dialog's fields made after the listeners
   that use them, the find panel telling `onClosed` when it switches to Replace, the options
   dialog's action buttons disabled with its OK when detached) are fixed and noted.

### Part three: charts and the treemap

9. **Charts**: `ChartData`, `ChartSeries`, `ChartType`, `ChartHit` with its shape,
   `ChartLayout`, `ChartRenderer` (bar, line, area, scatter, pie; the axis ticks, the palette and
   its shading when it wraps, legends, the hovered datum), `ChartView` (hover, tooltips, a
   click choosing, the hand cursor), and `barChart`, `lineChart`, `pieChart`, `chartView`.
   Numbers formatted as Java's `%.4g` does, rounding halves up. Tests: the six chart test files
   (48 tests).
10. **The treemap**: `TreemapNode`, the squarified layout, `TreemapRenderer`, `TreemapView`.
    `TreemapTest` (10).

### Part four: JadeMem

11. **`native`**: JadeMem's Windows calls (`NtQuerySystemInformation`, PDH, ETW, the token and
    privileges, `ShellExecuteW`, the raw MFT reader) on Astra's foreign functions, behind its
    `NativeApi` interface. Tests: `Win32NativeApiTest` and `MftTest`, on this machine.
12. **`core`**: the model, sampling, attribution, drill-downs, the ETW guard (its patterns as
    written), reports, the disk scan and its cache, the recorder's formats. Its 14 test files,
    on its fake system (`FakeNativeApi`).
13. **`gui`**: the application, its threads (sampler, refresh, attribution, disk scan) as Astra
    threads handing their results to the window with `EventLoop.post` in place of
    `Platform.runLater`. Tests: `ViewTest` (54), `RecorderTest`, `DiskTabTest`. Then run against
    the machine, elevated and not, and compared with the Kotlin JadeMem side by side.
14. **The end**: the Launcher's Chart and Dialogs tabs filled in; the docs (gui-plan.md 6.5
    "As built in M8"; the outstanding list); the full check of every build; the benchmarks;
    `build\m8` with the examples and JadeMem built; version 0.8.0.

## Left for later

- **Floats as arguments and results of foreign calls** come when a program needs them;
  JadeMem's are all whole numbers and pointers.
- **File dialogs on Linux**: Linux has no one system dialog. `NoFileDialog` there until a
  milestone takes it up.
- **The rest of N3** (paths with curves, dashes, gradients, transforms) waits for M10.

## Speed

- **Regex**: within a few times of Java's on JadeUI's uses (CSS, Markdown, code highlighting
  in later milestones), timed when built.
- **Charts**: a hovered chart repaints without laying out; JadeMem's timeline of 960 points
  draws within a frame.
- **JadeMem**: a sample every 5 seconds costs the window nothing it can see; the attribution
  scan of every driver file stays on its own thread.
