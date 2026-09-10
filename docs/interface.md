# Interface draft

## Design principles

1. The current task and timer must always be visible.
2. There is only one selected project and at most one active task.
3. Common actions require one key; destructive actions require confirmation.
4. The interface remains useful without a mouse or color.
5. Every view has an empty state and works in a small terminal.

## Main workspace

Target size: 120 x 32 or larger.

```text
┌ Planner ─────────────────────────────────────────────────────────── Wed 19 Aug ┐
│ PROJECTS        │ Personal reset                                                │
│                 │                                                               │
│ > Personal reset│  BACKLOG         READY           DOING           DONE         │
│   Job search    │ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌─────────┐ │
│   Learn C++     │ │ Sort papers │ │ Update CV  ! │ │ Call adviser │ │ Set up  │ │
│   Home          │ │              │ │ Fri 21 Aug  │ │              │ │ calendar│ │
│                 │ └──────────────┘ └──────────────┘ └──────────────┘ └─────────┘ │
│ + New project   │ ┌──────────────┐ ┌──────────────┐                  ┌─────────┐ │
│                 │ │ Book dentist │ │ Find 3 roles │                  │ Go for  │ │
│                 │ └──────────────┘ └──────────────┘                  │ a walk  │ │
│                 │                                                    └─────────┘ │
│                 │                                                               │
├─────────────────┴───────────────────────────────────────────────────────────────┤
│ FOCUS  Call adviser                                      00:18:42  [Running]    │
│        Today: 00:43:10   Task total: 01:12:36             Space pause  s stop   │
├─────────────────────────────────────────────────────────────────────────────────┤
│ h/j/k/l move  Enter open  n new  m move  t timer  c calendar  / search  ? help │
└─────────────────────────────────────────────────────────────────────────────────┘
```

The left panel changes projects. The board retains focus when the project panel is
hidden. Cards show only information useful for scanning: title, due date, and small
status markers. Full notes live in the task view.

## Task detail and focus

`Enter` opens the selected task. `t` starts it immediately; if another task is
running, the user is asked whether to switch.

```text
┌ Task ───────────────────────────────────────────────────────────────────────────┐
│ Call employment adviser                                               DOING     │
├─────────────────────────────────────────────────────────────────────────────────┤
│ Project     Personal reset                                                     │
│ Due         Thu 20 Aug, 14:00                                                  │
│ Estimate    30 minutes                                                         │
│ Tags        job-search, admin                                                   │
│                                                                                │
│ Notes                                                                          │
│ Ask about local return-to-work programmes and CV review appointments.          │
│                                                                                │
│ Checklist                                                                      │
│ [x] Find telephone number                                                      │
│ [ ] Write down questions                                                       │
│ [ ] Make call                                                                  │
│                                                                                │
├─────────────────────────────────────────────────────────────────────────────────┤
│                              00:18:42                                           │
│                         FOCUS SESSION RUNNING                                   │
│                                                                                │
│                    [Space] Pause     [s] Stop                                   │
├─────────────────────────────────────────────────────────────────────────────────┤
│ e edit  c checklist  m move  d due date  Esc close                             │
└─────────────────────────────────────────────────────────────────────────────────┘
```

Stopping a session records its start, end, duration, and task. Pausing closes the
current interval; resuming creates another interval so the history is honest and
recoverable after a crash.

## Calendar

The first version is a read-oriented month view. Tasks with due dates and scheduled
focus blocks appear in the agenda. Editing can reuse the task form.

```text
┌ Calendar ─────────────────────── August 2026 ───────────────────────────────────┐
│         Mon       Tue       Wed       Thu       Fri       Sat       Sun          │
│                                                                                │
│          17        18      [ 19 ]      20        21        22        23          │
│                              2          1         2                              │
│                                                                                │
│          24        25        26        27        28        29        30          │
│           1                             1                                       │
│                                                                                │
├ Wed 19 Aug ─────────────────────────────────────────────────────────────────────┤
│ 10:00  Focus block: Find suitable roles                         45 min          │
│ 14:00  Dentist                                                  appointment     │
│ 17:00  Daily shutdown                                           recurring       │
│         2 tasks due, 1 focus block                                              │
├─────────────────────────────────────────────────────────────────────────────────┤
│ h/l day  j/k week  [/ ] month  Enter open  n schedule  g today  Esc board      │
└─────────────────────────────────────────────────────────────────────────────────┘
```

Numbers beneath dates are item counts. Selection is also shown with brackets so it
does not depend on terminal color.

## Project overview

This view supports weekly planning without turning the board into a dashboard.

```text
┌ Project: Job search ─────────────────────────────────────────────────────────────┐
│ Outcome     Obtain a suitable support or junior development role                │
│ This week   Submit 3 considered applications; contact 2 people                  │
├─────────────────────────────────────────────────────────────────────────────────┤
│ Tasks       5 ready   1 doing   3 waiting   12 done                             │
│ Time        Today 00:45   This week 04:20   Last week 03:10                     │
│ Due soon    Update CV (Fri) · Acme application (Mon)                            │
│ Waiting     Adviser reply (4 days) · Reference from Sam (2 days)                │
├─────────────────────────────────────────────────────────────────────────────────┤
│ b board  r review  e edit project  Esc projects                                │
└─────────────────────────────────────────────────────────────────────────────────┘
```

## Small terminal layout

At widths below roughly 90 columns, show one Kanban column at a time. The project
picker becomes a pop-up. The timer remains visible.

```text
┌ Personal reset ─────────────── DOING  1/4 ┐
│                                           │
│ > Call employment adviser                 │
│   Due tomorrow · 30m · job-search         │
│                                           │
├───────────────────────────────────────────┤
│ FOCUS Call adviser   00:18:42  Running    │
├───────────────────────────────────────────┤
│ h/l column  j/k task  p projects  ? help  │
└───────────────────────────────────────────┘
```

## Global keys

```text
q          quit (confirm if a timer is active)
?          contextual help
p          choose project
b          board
c          calendar
t          start/switch timer on selected task
Space      pause/resume active timer
s          stop active timer
/          search tasks
Ctrl+P     command palette (later release)
```

Keys displayed in the footer are contextual and are the source of truth for the
current view.

## Open interaction questions

- Should a project have its own columns, or should all projects share one workflow?
- Should calendar focus blocks be independent events or always linked to tasks?
- Is a dedicated `Waiting` column preferable to a waiting flag?
- Should completed tasks remain on the board for the current week or disappear
  immediately into an archive?
