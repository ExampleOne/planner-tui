#include "planner.hpp"
#include "render.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void model_and_storage() {
    planner::Store store(":memory:");
    store.initialize();
    require(store.timezone() == "UTC", "default timezone is not UTC");
    store.set_timezone("Europe/London");
    require(store.timezone() == "Europe/London", "timezone setting was not persisted");
    store.set_timezone("UTC");
    const int project = store.add_project("Test project");
    const int task = store.add_task(project, "Test task", planner::Status::Ready);
    const int checklist_first = store.add_checklist_item(task, "Outline approach");
    const int checklist_second = store.add_checklist_item(task, "Implement change");
    auto initial = store.load(planner::Clock::now());
    initial.projects[0].unit_name = "pages";
    initial.projects[0].name = "Renamed project";
    initial.projects[0].plan_start = "2026-08-15";
    initial.projects[0].plan_end = "2026-09-10";
    store.update_project(initial.projects[0]);
    initial.tasks[0].progress_done = 3;
    initial.tasks[0].progress_target = 8;
    initial.tasks[0].notes = "Detailed research notes";
    initial.tasks[0].tags = "writing,important";
    initial.tasks[0].estimate_minutes = 90;
    store.update_task(initial.tasks[0]);
    auto checklist_item = std::find_if(initial.checklist_items.begin(), initial.checklist_items.end(),
        [checklist_first](const auto& item) { return item.id == checklist_first; });
    require(checklist_item != initial.checklist_items.end(), "task checklist item was not loaded");
    checklist_item->title = "Outline implementation";
    checklist_item->completed = true;
    store.update_checklist_item(*checklist_item);
    store.move_checklist_item(checklist_second, 0);
    store.add_schedule(task, "2026-08-19", "10:00", 45);
    const auto start = planner::parse_time("2026-08-19T10:00:00");
    store.start_timer(task, start);
    auto state = store.load(start + std::chrono::minutes(25));
    require(state.projects.size() == 1, "project was not persisted");
    require(state.tasks.size() == 1, "task was not persisted");
    require(state.projects[0].unit_name == "pages", "project unit was not persisted");
    require(state.projects[0].name == "Renamed project", "project rename was not persisted");
    require(state.projects[0].plan_start == "2026-08-15" && state.projects[0].plan_end == "2026-09-10",
            "project planning range was not persisted");
    require(state.tasks[0].progress_done == 3 && state.tasks[0].progress_target == 8,
            "task progress was not persisted");
    require(state.tasks[0].tags == "writing,important" && state.tasks[0].estimate_minutes == 90,
            "task detail fields were not persisted");
    require(state.checklist_items.size() == 2 && state.checklist_items[0].id == checklist_second &&
            state.checklist_items[1].id == checklist_first && state.checklist_items[1].completed &&
            state.checklist_items[1].title == "Outline implementation",
            "task checklist fields or ordering were not persisted");
    store.delete_checklist_item(checklist_second);
    require(store.load(start).checklist_items.size() == 1, "task checklist item was not deleted");
    require(state.schedule.size() == 1 && state.schedule[0].duration_minutes == 45,
            "schedule block was not persisted");
    require(state.timer_task_id == task, "timer was not restored");
    require(planner::active_seconds(state, task) == 1500, "active elapsed time is wrong");
    store.stop_timer(start + std::chrono::minutes(25));
    state = store.load(start + std::chrono::minutes(25));
    require(!state.timer_task_id, "timer did not stop");
    require(state.tasks[0].tracked_seconds == 1500, "stored duration is wrong");
    store.remember_paused_task(task);
    state = store.load(start + std::chrono::minutes(25));
    require(state.paused_task_id == task, "paused task was not persisted");
    store.clear_paused();
    const int todo = store.add_todo("Send application", "2026-08-19");
    state = store.load(start);
    require(state.todos.size() == 1 && !state.todos[0].completed, "daily todo was not persisted");
    state.todos[0].completed = true;
    state.todos[0].date = "2026-08-20";
    store.update_todo(state.todos[0]);
    state = store.load(start);
    require(state.todos[0].completed && state.todos[0].date == "2026-08-20",
            "daily todo completion or rescheduled date was not persisted");
    store.delete_todo(todo);
    require(store.load(start).todos.empty(), "daily todo was not deleted");
    const int recurring = store.add_todo("Morning review", "2026-08-19", "morning");
    store.set_todo_completed(recurring, "2026-08-19", true);
    state = store.load(start);
    require(state.todos.size() == 1 && state.todos[0].recurring && state.todos[0].list_name == "morning",
            "recurring morning todo was not persisted");
    require(state.todo_completions.size() == 1 && state.todo_completions[0].date == "2026-08-19",
            "per-date recurring completion was not persisted");
    store.set_todo_completed(recurring, "2026-08-19", false);
    require(store.load(start).todo_completions.empty(), "recurring completion was not reopened");
    store.rename_todo_on(recurring, "2026-08-19", "Special review");
    state = store.load(start);
    require(planner::todo_title_on(state, state.todos[0], "2026-08-19") == "Special review" &&
            planner::todo_title_on(state, state.todos[0], "2026-08-20") == "Morning review",
            "single-date recurring todo rename leaked to other dates");
    store.set_todo_completed(recurring, "2026-08-19", true);
    const int renamed_series = store.split_todo_from(recurring, "2026-08-21", "Morning planning");
    state = store.load(start);
    const auto old_series = std::find_if(state.todos.begin(), state.todos.end(),
        [recurring](const auto& item) { return item.id == recurring; });
    const auto new_series = std::find_if(state.todos.begin(), state.todos.end(),
        [renamed_series](const auto& item) { return item.id == renamed_series; });
    require(old_series != state.todos.end() && old_series->end_date == "2026-08-20" &&
            new_series != state.todos.end() && new_series->date == "2026-08-21",
            "from-date recurring todo rename did not split the series");
    require(state.todo_completions.size() == 1, "series split discarded historical completion data");
    store.rename_todo_all(renamed_series, "Daily planning");
    state = store.load(start);
    require(std::count_if(state.todos.begin(), state.todos.end(), [](const auto& item) {
                return item.title == "Daily planning";
            }) == 2, "all-date rename did not cover the complete split series");
    store.hide_todo_on(renamed_series, "2026-08-22");
    state = store.load(start);
    const auto hidden_series = std::find_if(state.todos.begin(), state.todos.end(),
        [renamed_series](const auto& item) { return item.id == renamed_series; });
    require(hidden_series != state.todos.end() && !planner::todo_visible_on(state, *hidden_series, "2026-08-22") &&
            planner::todo_visible_on(state, *hidden_series, "2026-08-23"),
            "single-date recurring todo deletion was not scoped");
    store.end_todo_before(renamed_series, "2026-08-24");
    state = store.load(start);
    const auto ended_series = std::find_if(state.todos.begin(), state.todos.end(),
        [renamed_series](const auto& item) { return item.id == renamed_series; });
    require(ended_series != state.todos.end() && planner::todo_visible_on(state, *ended_series, "2026-08-23") &&
            !planner::todo_visible_on(state, *ended_series, "2026-08-24"),
            "future recurring todo deletion did not preserve prior dates");
    store.delete_todo(recurring); store.delete_todo(renamed_series);

    const int goal = store.add_goal("Read books", "pages", 0, "2026-08-19", "", 12,
                                    "Keep notes about the reading goal.");
    store.add_goal_entry(goal, "2026-08-19", 12.5, "Chapter one");
    state = store.load(start);
    require(state.goals.size() == 1 && state.goals[0].target == 0 && state.goals[0].daily_goal == 12 &&
            state.goals[0].target_date.empty() &&
            state.goals[0].comment == "Keep notes about the reading goal." &&
            state.goal_entries.size() == 1 && state.goal_entries[0].amount == 12.5,
            "goal or datapoint was not persisted");
    store.clear_goal_entries_on(goal, "2026-08-19");
    require(store.load(start).goal_entries.empty(), "daily goal datapoints were not cleared");
    store.add_goal_entry(goal, "2026-08-19", 12.5, "Restored");
    state.goals[0].daily_goal = 15;
    state.goals[0].comment = "Updated comment\nwith a second line";
    store.update_goal(state.goals[0]);
    const auto updated_goal = store.load(start).goals[0];
    require(updated_goal.daily_goal == 15 && updated_goal.comment == "Updated comment\nwith a second line",
            "goal field or comment edit was not persisted");
    store.delete_goal(goal);
    state = store.load(start);
    require(state.goals.empty() && state.goal_entries.empty(), "goal deletion did not remove datapoints");
    const int manual = store.add_session(task, planner::parse_time("2026-08-19T09:00:00"),
                                         planner::parse_time("2026-08-19T10:00:00"));
    store.update_session(manual, planner::parse_time("2026-08-19T09:15:00"),
                         planner::parse_time("2026-08-19T10:00:00"));
    state = store.load(start + std::chrono::minutes(25));
    require(state.sessions.size() == 2, "manual tracking interval was not persisted");
    require(state.tasks[0].tracked_seconds == 4200, "corrected tracking total is wrong");
    store.update_session_start(manual, planner::parse_time("2026-08-19T09:20:00"));
    state = store.load(start);
    require(std::find_if(state.sessions.begin(), state.sessions.end(), [manual](const auto& session) {
        return session.id == manual && session.started_at == planner::parse_time("2026-08-19T09:20:00");
    }) != state.sessions.end(), "session start-only correction was not persisted");

    const int second = store.add_task(project, "Second task", planner::Status::Ready);
    store.update_session_task(manual, second);
    state = store.load(start);
    require(std::find_if(state.sessions.begin(), state.sessions.end(), [manual, second](const auto& session) {
        return session.id == manual && session.task_id == second;
    }) != state.sessions.end(), "tracked interval task reassignment was not persisted");
    store.move_task(second, planner::Status::Backlog, 0);
    state = store.load(start);
    const auto moved = std::find_if(state.tasks.begin(), state.tasks.end(), [second](const planner::Task& item) {
        return item.id == second;
    });
    require(moved != state.tasks.end() && moved->status == planner::Status::Backlog && moved->position == 0,
            "card move was not persisted");
    const int destination = store.add_project("Destination");
    store.move_task_to_project(second, destination);
    state = store.load(start);
    require(std::find_if(state.tasks.begin(), state.tasks.end(), [second, destination](const planner::Task& item) {
        return item.id == second && item.project_id == destination;
    }) != state.tasks.end(), "task project transfer was not persisted");
    store.checkpoint();
    store.archive_task(second);
    state = store.load(start);
    const auto archived = std::find_if(state.tasks.begin(), state.tasks.end(), [second](const planner::Task& item) {
        return item.id == second;
    });
    require(archived != state.tasks.end() && archived->archived, "task was not archived");
    require(store.undo(), "archive undo was unavailable");
    state = store.load(start);
    require(std::find_if(state.tasks.begin(), state.tasks.end(), [second](const planner::Task& item) {
        return item.id == second && !item.archived;
    }) != state.tasks.end(), "archive was not undone");

    const int disposable = store.add_task(project, "Accidental", planner::Status::Backlog);
    store.add_checklist_item(disposable, "Disposable checklist item");
    store.delete_task(disposable);
    state = store.load(start);
    require(std::none_of(state.tasks.begin(), state.tasks.end(), [disposable](const planner::Task& item) {
        return item.id == disposable;
    }), "history-free task was not deleted");
    require(std::none_of(state.checklist_items.begin(), state.checklist_items.end(),
        [disposable](const auto& item) { return item.task_id == disposable; }),
        "deleting a task did not remove its checklist");
    bool protected_history = false;
    try { store.delete_task(task); } catch (const std::exception&) { protected_history = true; }
    require(protected_history, "task with tracking history was deletable");
    const int allowance = store.add_allowance("YouTube", 30, "daily");
    store.start_allowance(allowance, planner::parse_time("2026-08-19T11:00:00"));
    state = store.load(planner::parse_time("2026-08-19T11:10:00"));
    require(state.active_allowance_id == allowance, "allowance timer did not start");
    store.stop_timer(planner::parse_time("2026-08-19T11:10:00"));
    state = store.load(planner::parse_time("2026-08-19T11:10:00"));
    require(!state.active_allowance_id && state.allowance_sessions.size() == 1,
            "allowance timer did not stop and persist");
    store.remember_paused_allowance(allowance);
    state = store.load(planner::parse_time("2026-08-19T11:10:00"));
    require(state.paused_allowance_id == allowance, "paused allowance was not persisted");
    store.clear_paused();
    store.start_timer(task, planner::parse_time("2026-08-19T12:00:00"));
    state = store.load(planner::parse_time("2026-08-19T12:00:00"));
    const auto active = std::find_if(state.sessions.begin(), state.sessions.end(), [](const auto& session) {
        return !session.ended_at;
    });
    require(active != state.sessions.end(), "active session for start edit is missing");
    store.update_session_start(active->id, planner::parse_time("2026-08-19T11:45:00"));
    state = store.load(planner::parse_time("2026-08-19T12:05:00"));
    require(state.timer_started_at == planner::parse_time("2026-08-19T11:45:00"),
            "active session start was not editable");
    store.stop_timer(planner::parse_time("2026-08-19T12:05:00"));
}

void timezone_handling() {
    planner::set_app_timezone("UTC");
    const auto summer_utc = planner::parse_time("2026-08-21T10:00:00");
    const auto winter_utc = planner::parse_time("2026-01-21T10:00:00");
    planner::set_app_timezone("Europe/London");
    const auto summer_london = planner::parse_time("2026-08-21T10:00:00");
    const auto winter_london = planner::parse_time("2026-01-21T10:00:00");
    require(std::chrono::duration_cast<std::chrono::hours>(summer_utc - summer_london).count() == 1,
            "London summer time offset is wrong");
    require(winter_utc == winter_london, "London winter time offset is wrong");
    planner::set_app_timezone("UTC");
}

void rendering() {
    planner::Store store(":memory:");
    store.initialize(); store.seed_demo();
    auto state = store.load(planner::parse_time("2026-08-19T10:00:00"));
    const auto call = std::find_if(state.tasks.begin(), state.tasks.end(), [](const planner::Task& task) {
        return task.title == "Call adviser";
    });
    require(call != state.tasks.end(), "demo call task is missing");
    const int call_id = call->id;
    store.add_schedule(call->id, "2026-08-19", "10:00", 30);
    state = store.load(planner::parse_time("2026-08-19T10:00:00"));
    state.tasks[0].due_date = "2026-08-19"; store.update_task(state.tasks[0]);
    state.tasks[1].due_date = "2026-08-18"; store.update_task(state.tasks[1]);
    state.tasks[2].due_date = "2026-08-21"; store.update_task(state.tasks[2]);
    const int checklist_task = state.tasks[0].id;
    const int checklist_done = store.add_checklist_item(checklist_task, "Collect source papers");
    store.add_checklist_item(checklist_task, "File source papers");
    state = store.load(planner::parse_time("2026-08-19T10:00:00"));
    const auto rendered_check = std::find_if(state.checklist_items.begin(), state.checklist_items.end(),
        [checklist_done](const auto& item) { return item.id == checklist_done; });
    require(rendered_check != state.checklist_items.end(), "render checklist setup failed");
    auto completed_check = *rendered_check; completed_check.completed = true;
    store.update_checklist_item(completed_check);
    state = store.load(planner::parse_time("2026-08-19T10:00:00"));
    auto normal = planner::render_text(state, 120, 32);
    require(normal.find("PROJECTS") != std::string::npos, "normal board lacks projects");
    require(normal.find("Update CV") != std::string::npos, "normal board lacks task");
    require(normal.find("DAILY CALENDAR") != std::string::npos, "wide layout lacks Today rail");
    require(normal.find("NOW") != std::string::npos, "wide layout lacks now marker");
    require(normal.find("DUE TODAY") != std::string::npos && normal.find("OVERDUE") != std::string::npos &&
            normal.find("UPCOMING") != std::string::npos,
            "due, overdue, and upcoming rail sections are missing");
    require(normal.find("TASK     total") != std::string::npos || normal.find("No focus task") != std::string::npos,
            "left panel task total area is missing");
    require(normal.find("total ") != std::string::npos && normal.find("today ") != std::string::npos,
            "project time breakdown is missing");
    require(normal.find("[1/2]") != std::string::npos,
            "task checklist progress is missing from the board card");
    state.project_focused = true;
    auto project_focus = planner::render_text(state, 120, 32);
    require(project_focus.find("@ PROJECTS") != std::string::npos,
            "project-column focus is not visible");
    state.project_focused = false;
    state.all_projects = true;
    auto all_projects = planner::render_text(state, 120, 32);
    require(all_projects.find("ALL PROJECTS") != std::string::npos,
            "aggregate project board is missing");
    require(planner::visible_tasks(state, planner::Status::Ready).size() >= 3,
            "aggregate board does not combine project tasks");
    state.all_projects = false;
    auto compact = planner::render_text(state, 40, 12);
    require(compact.find("column 1 of 4") != std::string::npos, "compact board was not selected");

    const int scrolling_project = state.projects[0].id;
    for (int i = 0; i < 10; ++i)
        store.add_task(scrolling_project, "Overflow " + std::to_string(i), planner::Status::Backlog);
    state = store.load(planner::parse_time("2026-08-19T10:00:00"));
    state.column = 0;
    state.row = static_cast<int>(planner::visible_tasks(state, planner::Status::Backlog).size()) - 1;
    state.board_scroll[0] = state.row;
    const auto compact_scrolled = planner::render_text(state, 40, 16);
    require(compact_scrolled.find("Overflow 9") != std::string::npos &&
            compact_scrolled.find("Overflow 0") == std::string::npos,
            "compact board did not scroll to the selected task");
    require(compact_scrolled.find('^') != std::string::npos,
            "compact board lacks an upward overflow indicator");
    state.board_scroll[1] = 0;
    const auto column_scrolled = planner::render_text(state, 80, 16);
    require(column_scrolled.find("Overflow 9") != std::string::npos &&
            state.board_scroll[1] == 0,
            "wide board did not preserve per-column scrolling");
    state.view = planner::View::Calendar;
    auto calendar = planner::render_text(state, 80, 24);
    require(calendar.find("August 2026") != std::string::npos, "calendar month is wrong");
    require(calendar.find("[19]") != std::string::npos, "calendar selection is missing");
    require(calendar.find("Call adviser") != std::string::npos, "calendar schedule is missing");
    auto wide_calendar = planner::render_text(state, 140, 34);
    require(wide_calendar.find("DAILY CALENDAR") != std::string::npos,
            "calendar lost the persistent Today rail");
    state.calendar_selecting_task_due = true;
    auto deadline_picker = planner::render_text(state, 120, 32);
    require(deadline_picker.find("Select Task Deadline") != std::string::npos &&
            deadline_picker.find("c clear") != std::string::npos,
            "task deadline calendar selector is missing");
    state.calendar_selecting_task_due = false;
    state.view = planner::View::Tracker;
    const int sleep = store.add_task(state.projects[0].id, "Sleep", planner::Status::Ready);
    store.add_session(sleep, planner::parse_time("2026-08-19T01:00:00"),
                      planner::parse_time("2026-08-19T07:00:00"));
    store.add_session(call_id, planner::parse_time("2026-08-18T23:30:00"),
                      planner::parse_time("2026-08-19T00:30:00"));
    state = store.load(planner::parse_time("2026-08-19T10:00:00"));
    state.view = planner::View::Tracker;
    auto tracker = planner::render_text(state, 120, 32);
    require(tracker.find("TRACKED DATE") != std::string::npos, "tracker view is missing");
    require(tracker.find("00:00") != std::string::npos && tracker.find("[] ") != std::string::npos &&
            tracker.find('=') != std::string::npos,
            "tracker activity timeline is missing");
    require(tracker.find("[] Sleep") != std::string::npos && tracker.find('z') != std::string::npos,
            "sleep timeline styling is missing");
    require(tracker.find("00:00-00:30") != std::string::npos,
            "tracker does not show the portion of an interval overlapping the selected day");
    state.tracker_row = -1;
    tracker = planner::render_text(state, 120, 32);
    require(tracker.find("> TRACKED DATE") != std::string::npos && tracker.find("Enter calendar") != std::string::npos,
            "tracker date cursor is missing");
    const int rendered_session = store.add_session(call_id, planner::parse_time("2026-08-19T08:00:00"),
                                                    planner::parse_time("2026-08-19T08:30:00"));
    state = store.load(planner::parse_time("2026-08-19T10:00:00"));
    state.view = planner::View::EditSession;
    state.editing_session_id = rendered_session;
    auto session_editor = planner::render_text(state, 120, 32);
    require(session_editor.find("EDIT TRACKED INTERVAL") != std::string::npos &&
            session_editor.find("Personal reset / Call adviser") != std::string::npos,
            "tracked interval editor is missing");
    require(session_editor.find("Start date") != std::string::npos &&
            session_editor.find("Start time") != std::string::npos &&
            session_editor.find("End date") != std::string::npos &&
            session_editor.find("End time") != std::string::npos,
            "tracked interval date and time fields are not independently selectable");
    state.calendar_selecting_session_date = 1;
    state.calendar_date = planner::parse_time("2026-08-18T08:00:00");
    state.view = planner::View::Calendar;
    auto session_date_picker = planner::render_text(state, 120, 32);
    require(session_date_picker.find("Select Interval Start Date") != std::string::npos &&
            session_date_picker.find("Enter choose date") != std::string::npos,
            "tracked interval date calendar is missing");
    state.view = planner::View::SessionTaskPicker;
    auto picker = planner::render_text(state, 120, 32);
    require(picker.find("MOVE TRACKED INTERVAL TO TASK") != std::string::npos &&
            picker.find("[-]") != std::string::npos && picker.find("Sort papers") != std::string::npos,
            "tracked interval task picker hierarchy is missing");
    const int picker_project = state.projects.back().id;
    for (int i = 0; i < 18; ++i)
        store.add_task(picker_project, "Overflow activity " + std::to_string(i), planner::Status::Done);
    state = store.load(planner::parse_time("2026-08-19T10:00:00"));
    state.view = planner::View::SessionTaskPicker;
    state.task_picker_row = static_cast<int>(std::count_if(state.tasks.begin(), state.tasks.end(),
        [](const auto& task) { return !task.archived; })) - 1;
    picker = planner::render_text(state, 80, 16);
    require(picker.find("Overflow activity 17") != std::string::npos &&
            picker.find("Overflow activity 0") == std::string::npos && picker.find('^') != std::string::npos,
            "tracked interval task picker did not scroll to the selected activity");
    state.view = planner::View::Menu;
    auto menu = planner::render_text(state, 120, 32);
    require(menu.find("CHOOSE A VIEW") != std::string::npos, "view menu is missing");
    require(menu.find("Allowances") != std::string::npos, "allowance menu entry is missing");
    require(menu.find("Daily Todos") != std::string::npos, "daily todo menu entry is missing");
    require(menu.find("Deadlines") != std::string::npos, "deadline menu entry is missing");
    require(menu.find("Settings") != std::string::npos, "settings menu entry is missing");
    require(menu.find("Gantt") != std::string::npos, "Gantt menu entry is missing");
    state.view = planner::View::Settings;
    auto settings = planner::render_text(state, 120, 32);
    require(settings.find("Timezone") != std::string::npos && settings.find("UTC") != std::string::npos,
            "timezone settings view is missing");
    state.view = planner::View::Deadlines;
    auto deadlines = planner::render_text(state, 120, 32);
    require(deadlines.find("ALL DEADLINES") != std::string::npos && deadlines.find("OVERDUE") != std::string::npos &&
            deadlines.find("TODAY") != std::string::npos && deadlines.find("UPCOMING") != std::string::npos,
            "sorted deadline view is missing deadline categories");
    store.add_todo("Check email", "2026-08-19");
    state = store.load(planner::parse_time("2026-08-19T10:00:00"));
    state.view = planner::View::Todos;
    auto todos = planner::render_text(state, 120, 32);
    require(todos.find("{task}") != std::string::npos,
            "tasks due on the displayed date are missing from Today Only todos");
    require(todos.find("DAILY TODOS") != std::string::npos && todos.find("Check email") != std::string::npos,
            "daily todo view is missing");
    require(todos.find("d date") != std::string::npos && todos.find("b tomorrow") != std::string::npos,
            "daily todo reschedule controls are missing");
    const int check_email = state.todos.front().id;
    state.calendar_rescheduling_todo_id = check_email;
    state.calendar_date = planner::parse_time("2026-08-21T10:00:00");
    state.view = planner::View::Calendar;
    const auto todo_rescheduler = planner::render_text(state, 120, 32);
    require(todo_rescheduler.find("Reschedule Todo") != std::string::npos &&
            todo_rescheduler.find("Enter move todo") != std::string::npos,
            "todo calendar rescheduler is missing");
    state.calendar_rescheduling_todo_id.reset();
    store.add_todo("Morning routine", "2026-08-19", "morning");
    state = store.load(planner::parse_time("2026-08-20T10:00:00"));
    state.view = planner::View::Todos; state.todo_list = 1;
    todos = planner::render_text(state, 120, 32);
    require(todos.find("[MORNING - DAILY]") != std::string::npos && todos.find("Morning routine") != std::string::npos,
            "recurring morning list is missing on a later date");
    state.todo_date_focused = true;
    todos = planner::render_text(state, 120, 32);
    require(todos.find("> DAILY TODOS") != std::string::npos && todos.find("Enter calendar") != std::string::npos,
            "daily todo date cursor is missing");
    const int reading_goal = store.add_goal("Read consistently", "pages", 300,
                                             "2026-08-19", "", 10,
                                             "Keep this visible in the goal editor.\nCapture obstacles and next steps.");
    store.add_goal_entry(reading_goal, "2026-08-19", 10, "First session");
    store.add_goal("Meditate", "sessions", 0, "2026-08-19", "", 1);
    state = store.load(planner::parse_time("2026-08-19T10:00:00"));
    state.view = planner::View::Todos; state.todo_list = 0;
    todos = planner::render_text(state, 120, 32);
    require(todos.find("Read consistently") != std::string::npos && todos.find("{goal}") != std::string::npos,
            "active goal is missing from Daily Todos");
    require(todos.find("[x] Read consistently") != std::string::npos &&
            todos.find("10/10 pages today") != std::string::npos &&
            todos.find("[ ] Meditate") != std::string::npos && todos.find("0/1 sessions today") != std::string::npos,
            "Daily Todos goal completion does not reflect the daily target fraction");
    state.view = planner::View::Goals;
    const auto goals = planner::render_text(state, 120, 32);
    require(goals.find("Read consistently") != std::string::npos && goals.find("Daily goal 10") != std::string::npos &&
            goals.find("Current total 10") != std::string::npos && goals.find("Current goal 10") != std::string::npos &&
            goals.find("NET +0") != std::string::npos && goals.find("no final due date") != std::string::npos &&
            goals.find("First session") != std::string::npos,
            "goal view lacks progress or pace information");
    require(goals.find("total 0  goal 1  net -1  daily 1") != std::string::npos,
            "unselected goal overview lacks current goal and net metrics");
    state.view = planner::View::EditGoal; state.editing_goal_id = reading_goal; state.goal_edit_row = 2;
    const auto goal_editor = planner::render_text(state, 120, 32);
    require(goal_editor.find("EDIT GOAL") != std::string::npos &&
            goal_editor.find("> Final goal") != std::string::npos &&
            goal_editor.find("Comment") != std::string::npos &&
            goal_editor.find("Keep this visible in the goal editor.") != std::string::npos &&
            goal_editor.find("Capture obstacles and next steps.") != std::string::npos &&
            goal_editor.find("Esc goals") != std::string::npos,
            "goal field editor is missing its comment window or selection controls");
    state.goal_edit_row = 6;
    const auto selected_comment = planner::render_text(state, 120, 32);
    require(selected_comment.find("> Comment") != std::string::npos &&
            selected_comment.find("Enter write comment") != std::string::npos &&
            selected_comment.find("Ctrl+S save") != std::string::npos,
            "goal comment window cannot be selected for editing");
    const auto compact_comment = planner::render_text(state, 80, 24);
    require(compact_comment.find("Keep this visible in the goal editor.") != std::string::npos &&
            compact_comment.find("Capture obstacles and next steps.") != std::string::npos,
            "goal comment window is not usable at the standard 80x24 terminal size");
    state.projects[0].plan_start = "2026-08-10"; state.projects[0].plan_end = "2026-09-10";
    store.update_project(state.projects[0]);
    store.add_goal("Draft chapters", "pages", 30, "2026-08-19", "", 5);
    state = store.load(planner::parse_time("2026-08-19T10:00:00"));
    state.view = planner::View::Gantt; state.gantt_date = state.now;
    auto gantt = planner::render_text(state, 120, 32);
    require(gantt.find("MONTH") != std::string::npos && gantt.find("Research paper") != std::string::npos &&
            gantt.find("Read consistently") != std::string::npos && gantt.find("Draft chapters") != std::string::npos &&
            gantt.find('*') != std::string::npos && gantt.find("Legend: # project") != std::string::npos,
            "monthly Gantt view lacks project or goal timelines");
    state.gantt_scale = 0;
    gantt = planner::render_text(state, 120, 32);
    require(gantt.find("WEEK") != std::string::npos, "weekly Gantt scale is missing");
    state.gantt_scale = 2; state.gantt_filter = 2;
    gantt = planner::render_text(state, 120, 32);
    require(gantt.find("YEAR") != std::string::npos && gantt.find("Filter: Goals") != std::string::npos,
            "yearly goal-only Gantt scale is missing");
    state.view = planner::View::Allowances;
    auto allowances = planner::render_text(state, 120, 32);
    require(allowances.find("FIXED TIME BUDGETS") != std::string::npos,
            "allowance view is missing");
    require(allowances.find("This period") != std::string::npos && allowances.find("Time: total") != std::string::npos,
            "allowance period and lifetime totals are missing");
    state.view = planner::View::Task;
    auto task_detail = planner::render_text(state, 120, 32);
    require(task_detail.find("Checklist 1/2 complete") != std::string::npos &&
            task_detail.find("c checklist") != std::string::npos,
            "task detail does not expose its checklist progress or control");
    state.view = planner::View::TaskChecklist; state.checklist_row = 1;
    auto checklist = planner::render_text(state, 120, 32);
    require(checklist.find("TASK CHECKLIST") != std::string::npos &&
            checklist.find("[x] Collect source papers") != std::string::npos &&
            checklist.find("> [ ] File source papers") != std::string::npos &&
            checklist.find("J/K reorder") != std::string::npos,
            "task checklist view is missing items or controls");
    state.view = planner::View::EditTask;
    auto edit = planner::render_text(state, 120, 32);
    require(edit.find("EDIT TASK DETAILS") != std::string::npos, "task edit view is missing");
    require(edit.find("Time  total") != std::string::npos && edit.find("session") != std::string::npos,
            "task edit time breakdown is missing");
    state.view = planner::View::Board;
    state.project_index = 3;
    auto paper = planner::render_text(state, 120, 32);
    require(paper.find("2/8 pages") != std::string::npos, "project-specific progress is missing");
}

void undo_and_redo_history() {
    planner::Store store(":memory:");
    store.initialize();
    const int project = store.add_project("History");
    store.checkpoint();
    store.add_task(project, "First", planner::Status::Backlog);
    store.checkpoint();
    store.add_task(project, "Second", planner::Status::Backlog);
    require(store.load(planner::Clock::now()).tasks.size() == 2, "history setup failed");
    require(store.undo(), "first multi-level undo failed");
    require(store.load(planner::Clock::now()).tasks.size() == 1, "first undo restored wrong state");
    require(store.undo(), "second multi-level undo failed");
    require(store.load(planner::Clock::now()).tasks.empty(), "second undo restored wrong state");
    require(store.redo(), "first redo failed");
    require(store.load(planner::Clock::now()).tasks.size() == 1, "first redo restored wrong state");
    require(store.redo(), "second redo failed");
    require(store.load(planner::Clock::now()).tasks.size() == 2, "second redo restored wrong state");
    require(!store.redo(), "redo was available beyond newest state");
    require(store.undo(), "undo before divergent change failed");
    store.checkpoint();
    store.add_task(project, "Replacement", planner::Status::Ready);
    require(!store.redo(), "divergent change did not clear redo history");
}

}  // namespace

int main() {
    try {
        model_and_storage();
        timezone_handling();
        undo_and_redo_history();
        rendering();
        std::cout << "All planner tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
