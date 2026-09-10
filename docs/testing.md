# Testing strategy for a C++ TUI

TUIs are difficult to inspect when presentation, input, and application state are
entangled. The implementation should therefore make the screen a pure projection
of testable state and reserve the real terminal for a thin outer layer.

## Architecture for testability

```text
 keystroke ──> input decoder ──> action ──> reducer ──> application state
                                                        │
                                                        v
 terminal <── ANSI renderer <── cell buffer <── view/layout model
```

- **Reducer:** applies an action to state without reading the clock or terminal.
- **Clock interface:** production uses the system clock; tests use a fake clock.
- **Storage interface:** production uses SQLite; tests use a temporary database.
- **Cell buffer:** a grid of character, style, and semantic-region values.
- **ANSI backend:** the only component that writes terminal escape sequences.

This makes most behaviour testable without launching an interactive application.

## Test layers

### 1. State-machine tests

Exercise commands directly:

```text
Given task A is selected and no timer is running
When StartTimer is applied at 10:00:00
Then task A is active and elapsed time is zero

When the fake clock advances 25 minutes and PauseTimer is applied
Then a 25-minute interval is stored and the timer is paused
```

These tests cover timer switching, midnight boundaries, deletion, undo, recurring
items, and crash recovery without terminal rendering.

### 2. Layout and renderer tests

Render a fixed state into an in-memory cell buffer at known sizes such as 40x12,
80x24, and 120x32. Compare a normalized plain-text representation with reviewed
golden files.

Style assertions remain separate from text snapshots:

```text
region "selected-task" has role=selection
region "active-timer" is visible
region "footer" is on the final row
no glyph lies outside the buffer
```

This avoids snapshots full of unstable ANSI escape sequences.

### 3. PTY integration tests

Run the compiled program inside a pseudo-terminal with fixed `TERM`, dimensions,
locale, temporary data directory, and fake clock. Send key sequences and capture
the resulting terminal state. Test scenarios include:

- create a project and task;
- move the task across columns;
- start, pause, resume, and stop its timer;
- resize while the timer is active;
- open the calendar and return to the same task;
- send an invalid key and verify that state is unchanged;
- terminate and restart, verifying recovery.

`tmux` is available locally and can drive early smoke tests using `send-keys` and
`capture-pane`. For robust CI, a small PTY harness should interpret ANSI output into
a virtual screen rather than inspecting raw bytes.

### 4. Human visual inspection

Automated tests cannot judge whether a dense screen feels calm or whether navigation
is intuitive. For each milestone, record a deterministic demonstration in a fixed
terminal size and inspect:

- 120x32 normal layout;
- 80x24 common layout;
- 40x12 degraded layout;
- light and dark terminal themes;
- monochrome mode;
- long titles and non-ASCII text.

Captured `tmux` panes provide text artifacts that can be attached to bug reports.
Short terminal recordings are useful for timing, flicker, and cursor issues.

## Debug and inspection mode

The application should include deterministic developer options:

```text
planner --demo
planner --data /tmp/planner-test.db
planner --size 80x24 --render board
planner --plain --render calendar
planner --fake-now 2026-08-19T10:00:00
planner --dump-state
```

`--plain --render` prints one frame and exits. This is the most important inspection
feature: it lets developers, tests, and automated assistants examine the interface
without trying to operate a live full-screen terminal.

## Local tooling status

Available now:

- `tmux` for controlled terminal sessions and pane capture
- `script` for PTY session recording
- Python 3 for an optional black-box test driver
- terminfo utilities (`infocmp`, `tput`)

Not currently installed:

- a C++ compiler
- CMake, Make, or Ninja
- detected ncurses development metadata

Before implementation, install a compiler/build system and choose either a small
terminal library or a carefully isolated ncurses/terminfo backend. That decision
should not alter the state, layout, or test architecture above.

