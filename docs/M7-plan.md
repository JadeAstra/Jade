# Plan: M7, Core part two

M7 ports the rest of JadeUI's root package that apps build with: text editing, the
containers, the controls, lists, trees and tables, menus and the screen they open on, and
the window's timed behaviour (tooltips, the blinking caret, hold-to-repeat). It ends with the
examples' `Launcher.kt` ported close to line for line: the acceptance test for "painless"
(gui-plan.md, section 7). Version 0.7.0 when it is done.

Written 2026-11-06 from a reading of every Kotlin file involved, against `lib/Jade` as M6
and M6.7 left it.

## What is ported

About 5,000 lines of Kotlin, into roughly the same number of lines of Astra, beside the
3,300 Jade has now.

| Kotlin | Lines | Astra | Holds |
| --- | --- | --- | --- |
| `TextNav.kt` | 276 | `TextNav.astra` | `TextNav`, `DragSelector`, `CaretGeometry`, `CaretKeys` |
| `Text.kt` | 817 | `Text.astra` | `TextEditable`, `StyleRun`, `TextLink`, `SharedTextModel` (undo, style runs, objects), `TextField`, `textField()` |
| `TextArea.kt` | 887 | `TextArea.astra` | `TextArea`, `textArea()`: wrapping, culled painting, links, squiggles, block indent |
| `HScrollBar.kt` | 130 | in `TextArea.astra` | the area's horizontal bar (internal) |
| `Scroll.kt` | 258 | `Scroll.astra` | `Scroll`, `scroll()`, `ScrollAware`, `Pinnable` |
| `Split.kt` | 159 | `Split.astra` | `Split`, `split()` |
| `Tabs.kt` | 246 | `Tabs.astra` | `Tabs`, `Tabs.Tab`, `tabs()` |
| `Controls.kt` | 532 | `Controls.astra` | `CheckBox`, `RadioGroup`, `RadioButton`, `Slider`, `ProgressBar`, `ListBox`, `Toggle`, `HoldRepeat`, `Spinner` |
| `Collections.kt` | 452 | `Collections.astra` | `TreeNode`, `Tree`, `Table` |
| `Combo.kt` | 151 | `Combo.astra` | `ComboBox` |
| `Screen.kt` | 476 | `Screen.astra` | `Screen`, `screen()`, the menu bar's view, popups, tips, accelerators |
| `Chrome.kt`, part | 140 of 934 | `Chrome.astra` | the menu model (`MenuItem`, `CheckItem`, `Separator`, `Menu`, `MenuBar`), `ContextMenuProvider`, `StyledText`, `Clipboard`, `MemoryClipboard`, `FileFilter`, `FileDialog`, `NoFileDialog` |
| `Drafts.kt` | 64 | `Drafts.astra` | `Drafts` (`Jade.drafts`) |
| `Block.kt`, `Doc.kt` | 316 | `Block.astra` | if chosen (below) |
| the GLFW surface's timing | | `Window.astra` | tooltips after 600 ms, the caret's 500 ms blink, hold-to-repeat, popups taking presses, drop-downs, context menus, accelerators, the system clipboard |

Not in M7: the rest of `Chrome.kt` (the dialogs, M8; `WindowSpec` and the rest replaced by
`Window` in 6.1), `Layers.kt` (the 3D view's layers, M11), `Commands.kt` and `Overrides.kt`
(they need the XML reader and `Regex`, N5).

Everything is drawn inside the window, as in JadeUI: menus, drop-downs, context menus and
tips are layers of the window's `Screen`, painted last. No native windows are added. The
native layer has what M7 needs (timers on the event loop, the nine cursor shapes, plain text
on the clipboard, focus events), except an input method's composition, which waits (below).

## Order

Each step is committed with its tests, carried over from JadeUI where they exist, as in M6.
Only the Clang Windows build is checked step by step; the full check comes at the end.

1. **Groundwork in `Component`.** The containers become one internal interface instead of a
   growing chain of `is GridPane` tests (layout, children, routing a point, the wheel), timed
   on pointer moves; `takesKeys`, `cacheable` (a scroll and a screen keep no picture),
   `windowPosition` through a scroll, and a way for a container to hand a child the `framed`
   style. A kept picture is refused by its side as well as its area (the GPU's texture
   limit), so a tall list never asks for a surface it cannot have. `TextUnits.charWidth`
   and `offsets`.
2. **`TextNav`**, with a value type for a selection's two ends in place of Kotlin's reversed
   ranges, which an Astra range does not keep. Tests: the word and line rules of
   `TextNavParityTest`.
3. **The text model**: `SharedTextModel` with its undo (the same merging of typing and of
   deleting, `compoundEdit`), style runs kept in step with edits, inline objects' places;
   `StyledText`, `Clipboard`, `MemoryClipboard`, `Jade.clipboard`; `Drafts`, `Jade.drafts`.
   Two things JadeUI does not have, for speed: a revision number, so a view knows the text
   changed without comparing it, and the starts of lines kept per revision, so finding a line
   is a search, not a walk. Tests: the model's half of `MirroredTextTest` and `EditingTest`,
   and a test of `Drafts`.
4. **The menu model and file dialog types** (the part of `Chrome.kt` above). `ChromeTest`.
5. **`TextField`** and `textField()`, with the focus wiring: the window tells the model it
   has the keyboard, and Tab skips a field that shows another's model. Hit-testing and the
   caret's place come from one `Font.offsets` call per line, not from measuring each prefix.
   Tests: `MirroredTextTest` (23), `EditingTest` (10), `DeferredTest` (2),
   `FocusAfterPressTest`, `HarnessTest`.
6. **`Scroll`.** Wheel routing innermost first, claiming the wheel only when it moved;
   revealing a range once, in layout. Tests: `ScrollTest`, `ScrollRepaintTest`,
   `NestedScrollTest`, `WheelRoutingTest` (those without menus), `WindowPositionScrollTest`
   rewritten with a grid.
7. **`TextArea`**: unwrapped, then wrapped, then its horizontal bar, then culling. Timed on a
   text of 680,000 characters, the size that once took ten seconds a frame in JadeUI: wrapped
   rows kept per width and found by search, and a C++ piece for wrapping if the interpreter's
   walk is too slow, public in Astra as such pieces are. Tests: `TextAreaTest` (15),
   `CaretRectTest`, `TextAreaLinksTest`, `TextAreaCurrentLineTest`, `TextAreaHScrollTest`,
   `TextAreaLargeTextTest`, `TextNavParityTest`.
8. **`Split` and `Tabs`.** Tests: `SplitTest`, `JadeTabsChromeTest`, `LayoutPassTest`,
   `LayoutRequestTest`, `ContainerCacheTest`, `ToolbarChromeTest`.
9. **The simple controls, `ListBox` and `Tree`.** Lists and trees measure their items once
   per change, not on every layout, and paint only the rows in view, as JadeUI's table does.
   Tests: `ControlsTest`, `CollectionsTest` (tree), `ContentInvalidationTest`,
   `InvalidateLayoutTest`.
10. **`Table`, `Spinner`, `ComboBox`** (each holds a text field). Tests: the rest of
    `CollectionsTest` and `ControlsTest`, `ComboTest`, `TableActivateTest`,
    `TableColumnWidthTest`, `TablePerformanceTest` (4,000 rows), `FirstClickTest`.
11. **`Screen`** and the window's side of it: every window's root wrapped in a screen; menus
    from the bar, drop-downs from a `DropdownProvider`, context menus on the right button,
    accelerators after the edit keys; `Window.menuBar` typed. Tests: `ScreenMenuTest`,
    `ScreenPopupTest`, `ScreenGeometryTest`, `AcceleratorTest`, `MultiHostHookTest`, the
    menu cases of the wheel and repaint tests, and the GLFW backend's menu and accelerator
    tests rewritten for `jade_host.astra`.
12. **The window's timed behaviour**: tooltips, the caret's blink, hold-to-repeat, each one
    timer set again as needed and cancelled when the window closes or loses focus (a running
    timer keeps a program alive); the system clipboard (plain text to the system, the last
    styled copy kept so a paste of the same text gets its runs back). The times are kept as
    fields that a test can move on, so nothing waits real time. Tests: `ScreenTipTest`,
    `IdleRepaintTest`, `SteadyStateFrameTest` on a hidden window.
13. **`Block` and `Doc`**, if they are to be in M7 (below). `BlockTest` (6 of 7), `DocTest`.
14. **The Launcher**: `examples/launcher.astra`, with its four rows of fields, its menus and
    its widget tabs (Controls, Editor, Items, Tree, Table, Split), and the tabs of later
    milestones as below. Then the docs (gui-plan.md 6.4, "As built in M7"), the full check
    of every build, the benchmarks, and version 0.7.0.

About 250 of JadeUI's tests carry over. None needs JavaFX, Swing or GLFW: they paint into
pictures and send events, as M6's did.

## Speed

The JVM walks text a character at a time cheaply; Astra's interpreter does not, so the text
widgets are where M7's speed is decided. Measured as they are built: typing and moving the
caret in a 680,000-character area (a frame well under 16 ms), a 4,000-row table (JadeUI's
own limits: fewer than 400 text draws, 200 layouts in 1.5 s), a 10,000-item list changing its
selection. The ways listed in steps 3, 5, 7 and 9 change nothing a program sees.

## Left for later

- **An input method's composition** (typing Chinese or Japanese in place, the candidate
  window at the caret): committed text arrives today; showing it while it is composed needs
  the system's IME messages. JadeUI has none either.
- **Inline objects in text** (`TextFieldObjectsTest`): their sizes come from the HTML
  reader of M10.
- **Undo** keeps a whole copy of the text per step, as JadeUI does, up to 200 steps. Kept as
  it is unless a timing or a large text asks for differences instead.
- **Combining marks and right-to-left text** get carets as good as the text engine's first
  part allows, until its second part (shaping and direction).

## Decided (the user, 2026-11-06)

1. **The Launcher's later tabs are placeholders**, one line each in Kotlin's order
   (`"Code" to label("Comes with M9")`), each replaced by its milestone.
2. **`Block` and `Doc` are ported at the end of M7** (step 13).
3. **A `CheckItem` keeps its state**, flipped when it is chosen, and draws its tick from it:
   JadeUI's tick fixed at making is not carried over.
4. **Menus are kept inside the window**: a context menu, drop-down or submenu that would
   cross the right or bottom edge opens to the left or above instead.
