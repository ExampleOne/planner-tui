# Planner TUI

A local-first terminal planner built around projects, Kanban tasks, focused work
sessions, and a calendar. The implementation will be C++; the interface is being
specified before code is written.

## Proposed first release

- Projects with independent Kanban boards
- Configurable columns, initially `Backlog`, `Ready`, `Doing`, and `Done`
- One active task at a time
- Start, pause, resume, and stop a timer for that task
- Calendar showing deadlines and scheduled focus sessions
- Project-defined progress units such as points, pages, or applications
- Keyboard-first operation with discoverable help
- Local SQLite storage and exportable data

## Documents

- [Interface draft](docs/interface.md)
- [TUI testing strategy](docs/testing.md)

## Build and run

This workspace already contains a project-local GCC toolchain. On a fresh Ubuntu
machine, either install `g++`, `libncurses-dev`, and `libsqlite3-dev`, or bootstrap
the same local toolchain without administrator access:

```bash
./tools/bootstrap-toolchain.sh
./build.sh
./build/planner --demo
```

Run with persistent personal data:

```bash
./build/planner
```

The default database is `~/.local/share/planner-tui/planner.db`. An alternative can
be selected with `--data FILE`.

The interactive board uses a bright, redundant color scheme: yellow Backlog, cyan
Ready, magenta Doing, and green Done. A running timer uses bold white text on a
green background. Selection also uses reverse video and textual markers, so the
interface remains readable in monochrome terminals.

On a Board with a visible project column, press Left from Backlog to focus Projects,
then press Left again to focus the Daily Calendar. There, Up/Down selects scheduled
tasks; moving above the first entry selects Current Activity. `Space` pauses or
resumes that activity, `t` starts a scheduled task, `s` stops and clears the active
or paused timer, and Right returns to Projects. Paused activity is stored in SQLite
and remains resumable after quitting and reopening the planner.
Within Projects, use Up/Down to select one and press Right to return to cards.
Lowercase `p` remains
a compact-layout fallback when that column cannot fit; uppercase `P` creates a
project. While Projects has focus, `e` renames the selected project and `u` changes
its progress unit.

`ALL PROJECTS` is a virtual board at the top of the project list. It combines every
non-archived task and labels cards with their source project. Editing, timing,
moving, archiving, and deleting operate on the original task. Select a real project
before using `n`, because the aggregate board has no destination for new tasks.

Open a task with `Enter`, then use `e` for the detailed field editor or `z` to schedule a dated
focus block. The editor covers title, notes, due date, tags, estimate, and progress.
As a shortcut, `e` also opens the editor directly for the selected Board card.
Press `c` from the open task to manage its checklist. In the checklist, `n` adds an item,
`Space` completes or reopens it, `e` renames it, uppercase `J`/`K` reorders it, and
`Delete` removes it. Checklist progress is shown on the task detail and as a `[done/total]`
badge on its Board card.
On Board or Task, `a` archives and `Delete` permanently removes a history-free task,
both with confirmation. `Ctrl+Z` walks backward through up to 50 database-changing
actions and `Ctrl+Y` reapplies undone actions. A new change clears the redo history.
In the calendar, `h/l` moves by day, `j/k` by week, and `g` returns to today.

At 90 columns or wider, a persistent Today rail appears on the left. It shows the
clock, current activity, elapsed time, today's scheduled blocks, a NOW marker, and
tasks due today.
The project workspace remains on the right. Press `Tab` to focus the rail, `j/k` to
select a scheduled block, `Enter` to open its task, or `t` to start its timer.
Narrower terminals retain the compact single-pane layout. Top-level navigation is
centered on `M Views`; there are no direct Board or Calendar shortcuts. The menu
provides Board, Daily Todos, Deadlines, Calendar, Tracker, Allowances, Goals, Gantt, Settings, and Help. Deadlines is a
date-sorted, scrollable list of past and future task due dates; the Today rail also shows the next three
unfinished upcoming tasks. Settings stores
an internal IANA timezone (for example `Europe/London`) used for clocks, calendar dates,
day/week boundaries, and entered local times without changing stored timestamps. Daily Todos are a dated
checklist with Today Only, Morning, and Evening sublists. `h`/`l` selects a sublist and `n`
adds there; Morning and Evening entries recur daily with independent completion history.
`Space` completes/reopens an item, `e` renames it, `Delete` removes it, `[`/`]` browse days,
`Enter` opens the displayed-day selector, and `g` returns to today. On a selected Today Only
todo, `d` opens a calendar to change its date and `b` bumps it to the following day. These
actions also reschedule a `{task}` by changing its deadline; daily recurring items and goals
keep their schedule and direct you to their respective editor. Pressing `Esc` from a top-level view
Kanban tasks with a deadline automatically appear as `{task}` entries in Today Only on
their due date; `Space` marks the card Done or reopens it in Ready, and `e` opens task editing.
Editing or deleting a recurring Morning/Evening todo asks whether the change applies only
to the displayed date, from that date onward, or to all dates. Single-date exceptions and
past completion history are retained when a recurrence is changed from a chosen date.
Active goals also appear as `{goal}` entries in Today Only. Press `Space` on one to record
an amount and optional note for the displayed date, or `e` to open it in Goals. Their
fraction is the amount recorded on that date over the daily goal (for example `0/1`),
and `[x]` appears only once the full daily target has been reached. Each row also shows
the cumulative total through that date, expected current goal, and net position. Press
`u` on a goal row to clear all of its datapoints for the displayed date; on a completed
todo, `u` explicitly reopens it (`Space` also toggles ordinary todos).
also opens Views, while `Esc` from Task returns to the view that opened it. Tracker shows today's
recorded intervals, including any interval that overlaps the selected day (overnight
intervals are clipped and counted on both dates). `e` opens the selected interval so
its task, start, or end can be changed; a running interval allows its task and start to
be edited while End remains `RUNNING` (blank time input preserves the current value), and `n` adds a manual
interval for the task most recently selected on the Board.
The tracked-interval activity picker groups activities as Project folders containing
status folders (`Backlog`, `Ready`, `Doing`, and `Done`). `j`/`k` moves between activities
without selecting folder headings, and the tree scrolls automatically to keep the selected
activity visible.
The Tracker timeline bar maps intervals proportionally across the selected local day;
task and allowance rows use matching stable colours, with `=` for task time and `~` for allowances.
Tasks whose title contains `sleep` always use a dedicated light tan/brown colour and `z`
characters in the timeline.
Move above the first Tracker interval to focus its date; `h`/`l` or Left/Right changes
the day, `g` returns to today, and `Enter` opens the calendar date selector.
In Task editing, pressing `Enter` on Due date opens the same calendar-style selector;
`Enter` sets the deadline, `c` clears it, and `Esc` cancels.

On the Board, `Space` picks up the selected card, arrow keys move it between columns
or reorder it vertically, and `Space` drops it. From Backlog, move Left into Projects,
choose a destination with Up/Down, then press `Space` to transfer and drop the card;
its tracking history moves with it. Outside the Board, `Space` continues
to pause or resume the focus timer. Quitting does not stop a running timer: its open
interval is restored when the application is launched again.
Columns scroll independently when they contain more cards than fit on screen. Moving
the selection with Up/Down keeps its card visible; `^` and `v` mark hidden cards above
or below the current column viewport.

Goals is a local cumulative goal tracker. Press `n` to enter a name, unit, and daily goal;
the final goal total and final due date are optional. Press `+` to record a dated increment, `e` to revise the goal, and
`Delete` to remove it with its datapoints. The view shows total progress, remaining work,
the expected cumulative goal today, green/red net status, actual pace, projected finish,
and recent datapoints. Total, current goal, net, and daily target also appear directly in
every overview row, without requiring the goal to be selected.
Goal editing uses a field page like the Tracker interval editor: move between Name, Unit,
Final goal, Daily goal, Start date, Final due, and the Comment window with `j`/`k`, then press
`Enter` or `e` to change the selected value. The Comment window is a multiline editor: use
`Enter` for a new line, `Ctrl+S` to save, and `Esc` to cancel. Enter `-` on Final goal or Final
due to clear it. `Esc` returns to Goals.

Gantt combines project planning ranges and goal runtimes on one timeline. Use `w`, `m`,
or `y` for week, month, or year scale; `h`/`l` moves between periods and `g` returns to
today. `f` cycles through all rows, projects only, and goals only. Select a project and
press `e` to assign its planned start/end dates, or `Delete` to clear the range without
deleting the project. Goal bars use their start and final due dates when available, show
projected completion with `*`, and remain open-ended with `>` when no finish can yet be
projected. Without a due date, a final goal plus daily target infers a planned finish as
`start + ceil(final goal / daily goal) - 1 day`. `Enter` opens the selected project or goal.

Inspect a deterministic frame without launching a TUI:

```bash
./build/planner --demo --plain --render board --size 80x24 \
  --fake-now 2026-08-19T10:00:00
```

Run all model, persistence, layout, and smoke tests:

```bash
./test.sh
```

## Current status

The first usable vertical slice is implemented. See the interface draft for later
features that are not yet part of this slice, including recurring schedule blocks.
