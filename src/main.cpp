#include "planner.hpp"
#include "render.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>

#include <ncursesw/ncurses.h>

namespace {

struct Options {
    std::string data;
    std::string render_view;
    std::string fake_now;
    int width{100};
    int height{28};
    bool demo{};
    bool plain{};
    bool dump_state{};
};

Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("Missing value for ") + flag);
            return argv[++i];
        };
        if (arg == "--data") result.data = value("--data");
        else if (arg == "--render") result.render_view = value("--render");
        else if (arg == "--fake-now") result.fake_now = value("--fake-now");
        else if (arg == "--size") {
            const std::string size = value("--size");
            const auto split = size.find('x');
            if (split == std::string::npos) throw std::runtime_error("Size must be WIDTHxHEIGHT");
            result.width = std::stoi(size.substr(0, split));
            result.height = std::stoi(size.substr(split + 1));
        } else if (arg == "--demo") result.demo = true;
        else if (arg == "--plain") result.plain = true;
        else if (arg == "--dump-state") result.dump_state = true;
        else if (arg == "--help") {
            std::cout << "planner [--demo] [--data FILE] "
                         "[--plain --render board|todos|deadlines|calendar|tracker|allowances|goals|gantt|settings|menu|help] "
                         "[--size WxH] [--fake-now ISO] [--dump-state]\n";
            std::exit(0);
        } else throw std::runtime_error("Unknown option: " + arg);
    }
    return result;
}

std::string default_path() {
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = home ? std::filesystem::path(home) : std::filesystem::current_path();
    const auto directory = base / ".local" / "share" / "planner-tui";
    std::filesystem::create_directories(directory);
    return (directory / "planner.db").string();
}

void normalize(planner::State& state) {
    if (state.projects.empty()) {
        state.project_index = 0; state.row = 0;
        state.board_scroll.fill(0);
        return;
    }
    state.project_index = std::clamp(state.project_index, 0, static_cast<int>(state.projects.size()) - 1);
    state.column = std::clamp(state.column, 0, 3);
    const auto tasks = planner::visible_tasks(state, static_cast<planner::Status>(state.column));
    state.row = tasks.empty() ? 0 : std::clamp(state.row, 0, static_cast<int>(tasks.size()) - 1);
    for (int column = 0; column < 4; ++column) {
        const int count = static_cast<int>(planner::visible_tasks(
            state, static_cast<planner::Status>(column)).size());
        state.board_scroll[column] = std::clamp(state.board_scroll[column], 0, std::max(0, count - 1));
    }
}

int board_capacity(int width, int height, bool show_focus) {
    const int panel = show_focus ? height - 6 : height - 2;
    return width < 72 ? std::max(1, (panel - 5) / 2) : std::max(1, (panel - 6) / 3);
}

void reveal_board_selection(planner::State& state, int width, int height, bool show_focus) {
    const int column = std::clamp(state.column, 0, 3);
    const int capacity = board_capacity(width, height, show_focus);
    int& first = state.board_scroll[column];
    if (state.row < first) first = state.row;
    else if (state.row >= first + capacity) first = state.row - capacity + 1;
    const int count = static_cast<int>(planner::visible_tasks(
        state, static_cast<planner::Status>(column)).size());
    first = std::clamp(first, 0, std::max(0, count - capacity));
}

const planner::Task* scheduled_task(const planner::State& state) {
    const std::string today = planner::date_iso(state.now);
    int index = 0;
    for (const auto& block : state.schedule) {
        if (block.date != today) continue;
        if (index++ != state.schedule_row) continue;
        const auto task = std::find_if(state.tasks.begin(), state.tasks.end(), [&block](const planner::Task& item) {
            return item.id == block.task_id;
        });
        return task == state.tasks.end() ? nullptr : &*task;
    }
    return nullptr;
}

int today_schedule_count(const planner::State& state) {
    const std::string today = planner::date_iso(state.now);
    return static_cast<int>(std::count_if(state.schedule.begin(), state.schedule.end(), [&today](const auto& block) {
        return block.date == today;
    }));
}

std::pair<planner::TimePoint, planner::TimePoint> local_day_bounds(planner::TimePoint point) {
    const std::time_t value = planner::Clock::to_time_t(point);
    std::tm tm{}; localtime_r(&value, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0; tm.tm_isdst = -1;
    const auto start = planner::Clock::from_time_t(mktime(&tm));
    tm.tm_mday += 1; tm.tm_isdst = -1;
    return {start, planner::Clock::from_time_t(mktime(&tm))};
}

const planner::Session* selected_tracker_session(const planner::State& state) {
    if (state.tracker_row < 0) return nullptr;
    const auto [day_start, day_end] = local_day_bounds(state.tracker_date);
    int index = 0;
    for (const auto& session : state.sessions) {
        const auto end = session.ended_at.value_or(state.now);
        if (session.started_at >= day_end || end <= day_start) continue;
        if (index++ == state.tracker_row) return &session;
    }
    return nullptr;
}

int today_session_count(const planner::State& state) {
    const auto [day_start, day_end] = local_day_bounds(state.tracker_date);
    return static_cast<int>(std::count_if(state.sessions.begin(), state.sessions.end(), [&state, day_start, day_end](const auto& session) {
        return session.started_at < day_end && session.ended_at.value_or(state.now) > day_start;
    }));
}

const planner::Session* session_by_id(const planner::State& state, int id) {
    const auto found = std::find_if(state.sessions.begin(), state.sessions.end(), [id](const auto& session) {
        return session.id == id;
    });
    return found == state.sessions.end() ? nullptr : &*found;
}

std::vector<const planner::Task*> picker_tasks(const planner::State& state) {
    std::vector<const planner::Task*> tasks;
    for (const auto& task : state.tasks) if (!task.archived) tasks.push_back(&task);
    std::sort(tasks.begin(), tasks.end(), [&state](const auto* left, const auto* right) {
        auto project_order = [&state](int id) {
            const auto found = std::find_if(state.projects.begin(), state.projects.end(), [id](const auto& project) {
                return project.id == id;
            });
            return found == state.projects.end() ? static_cast<int>(state.projects.size()) :
                                                   static_cast<int>(found - state.projects.begin());
        };
        return std::tuple{project_order(left->project_id), static_cast<int>(left->status), left->position, left->id} <
               std::tuple{project_order(right->project_id), static_cast<int>(right->status), right->position, right->id};
    });
    return tasks;
}

std::vector<const planner::Task*> deadline_tasks(const planner::State& state) {
    std::vector<const planner::Task*> tasks;
    for (const auto& task : state.tasks)
        if (!task.archived && !task.due_date.empty()) tasks.push_back(&task);
    std::sort(tasks.begin(), tasks.end(), [](const auto* left, const auto* right) {
        return left->due_date != right->due_date ? left->due_date < right->due_date : left->title < right->title;
    });
    return tasks;
}

planner::Todo* selected_todo(planner::State& state) {
    const std::string date = planner::date_iso(state.todo_date);
    static const std::array<const char*, 3> lists{"today", "morning", "evening"};
    const std::string list = lists[std::clamp(state.todo_list, 0, 2)];
    int index = 0;
    for (auto& todo : state.todos)
        if (todo.list_name == list && planner::todo_visible_on(state, todo, date) &&
            index++ == state.todo_row) return &todo;
    return nullptr;
}

int stored_todo_count(const planner::State& state) {
    const std::string date = planner::date_iso(state.todo_date);
    static const std::array<const char*, 3> lists{"today", "morning", "evening"};
    const std::string list = lists[std::clamp(state.todo_list, 0, 2)];
    return static_cast<int>(std::count_if(state.todos.begin(), state.todos.end(),
        [&state, &date, &list](const auto& todo) {
            return todo.list_name == list && planner::todo_visible_on(state, todo, date);
        }));
}

planner::Task* selected_due_todo_task(planner::State& state) {
    if (state.todo_list != 0) return nullptr;
    const int wanted = state.todo_row - stored_todo_count(state);
    if (wanted < 0) return nullptr;
    const std::string date = planner::date_iso(state.todo_date);
    int index = 0;
    for (auto& task : state.tasks) {
        if (!task.archived && task.due_date == date && index++ == wanted) return &task;
    }
    return nullptr;
}

int due_todo_count(const planner::State& state) {
    if (state.todo_list != 0) return 0;
    const std::string date = planner::date_iso(state.todo_date);
    return static_cast<int>(std::count_if(state.tasks.begin(), state.tasks.end(),
        [&date](const auto& task) { return !task.archived && task.due_date == date; }));
}

bool goal_active_on(const planner::Goal& goal, const std::string& date) {
    return goal.start_date <= date && (goal.target_date.empty() || date <= goal.target_date);
}

double selected_goal_net(const planner::State& state) {
    if (state.goals.empty()) return 0;
    const auto& goal = state.goals[std::clamp(state.goal_row, 0, static_cast<int>(state.goals.size()) - 1)];
    double total = 0;
    for (const auto& entry : state.goal_entries) if (entry.goal_id == goal.id) total += entry.amount;
    long long elapsed = 1;
    try {
        elapsed = std::max<long long>(1, std::chrono::duration_cast<std::chrono::hours>(
            planner::parse_time(planner::date_iso(state.now) + "T12:00:00") -
            planner::parse_time(goal.start_date + "T12:00:00")).count() / 24 + 1);
    } catch (const std::exception&) {}
    const double expected = goal.target > 0 ? std::min(goal.target, goal.daily_goal * elapsed) :
                                              goal.daily_goal * elapsed;
    return total - expected;
}

int goal_net_y(const planner::State& state, int height) {
    const int shown = std::min(static_cast<int>(state.goals.size()), std::max(0, height - 16));
    return std::max(7 + shown, height - 10) + 3;
}

planner::Goal* selected_daily_goal(planner::State& state) {
    if (state.todo_list != 0) return nullptr;
    const int wanted = state.todo_row - stored_todo_count(state) - due_todo_count(state);
    if (wanted < 0) return nullptr;
    const std::string date = planner::date_iso(state.todo_date);
    int index = 0;
    for (auto& goal : state.goals)
        if (goal_active_on(goal, date) && index++ == wanted) return &goal;
    return nullptr;
}

int todo_count(const planner::State& state) {
    int count = stored_todo_count(state);
    if (state.todo_list == 0) {
        const std::string date = planner::date_iso(state.todo_date);
        count += due_todo_count(state);
        count += static_cast<int>(std::count_if(state.goals.begin(), state.goals.end(),
            [&date](const auto& goal) { return goal_active_on(goal, date); }));
    }
    return count;
}

std::vector<const planner::ChecklistItem*> selected_task_checklist(const planner::State& state) {
    std::vector<const planner::ChecklistItem*> result;
    const auto* task = planner::selected_task(state);
    if (!task) return result;
    for (const auto& item : state.checklist_items)
        if (item.task_id == task->id) result.push_back(&item);
    return result;
}

planner::ChecklistItem* selected_checklist_item(planner::State& state) {
    const auto* task = planner::selected_task(state);
    if (!task) return nullptr;
    int index = 0;
    for (auto& item : state.checklist_items) {
        if (item.task_id != task->id) continue;
        if (index++ == state.checklist_row) return &item;
    }
    return nullptr;
}

bool todo_completed_on(const planner::State& state, const planner::Todo& todo, const std::string& date) {
    if (!todo.recurring) return todo.completed;
    return std::any_of(state.todo_completions.begin(), state.todo_completions.end(),
        [&todo, &date](const auto& completion) {
            return completion.todo_id == todo.id && completion.date == date;
        });
}

void reveal_task(planner::State& state, int task_id, bool open) {
    const auto task = std::find_if(state.tasks.begin(), state.tasks.end(), [task_id](const planner::Task& item) {
        return item.id == task_id;
    });
    if (task == state.tasks.end()) return;
    const auto project = std::find_if(state.projects.begin(), state.projects.end(), [task](const planner::Project& item) {
        return item.id == task->project_id;
    });
    if (project != state.projects.end()) state.project_index = static_cast<int>(project - state.projects.begin());
    state.all_projects = false;
    state.column = static_cast<int>(task->status);
    const auto column_tasks = planner::visible_tasks(state, task->status);
    const auto row = std::find_if(column_tasks.begin(), column_tasks.end(), [task_id](const planner::Task* item) {
        return item->id == task_id;
    });
    state.row = row == column_tasks.end() ? 0 : static_cast<int>(row - column_tasks.begin());
    if (open) state.view = planner::View::Task;
}

std::string prompt(const std::string& label, int height) {
    echo(); curs_set(1); timeout(-1);
    move(height - 1, 0); clrtoeol();
    addnstr(label.c_str(), COLS - 1);
    char buffer[512]{};
    getnstr(buffer, 511);
    noecho(); curs_set(0); timeout(250);
    return buffer;
}

std::optional<std::string> edit_comment(const std::string& initial, int height, int width) {
    constexpr int box_y = 12;
    const int rail_width = width >= 90 ? std::min(38, std::max(32, width / 3)) : 0;
    const int main_width = width - rail_width;
    const int box_x = rail_width + 3;
    const int box_width = main_width - 6;
    const int box_height = height - box_y - 3;
    if (box_width < 8 || box_height < 3) {
        return prompt("Goal comment (blank clears): ", height);
    }

    std::vector<std::string> lines(1);
    for (const char ch : initial) {
        if (ch == '\n') lines.emplace_back();
        else if (ch != '\r') lines.back().push_back(ch);
    }
    int line = static_cast<int>(lines.size()) - 1;
    int column = static_cast<int>(lines.back().size());
    int scroll = 0;
    const int inner_width = box_width - 2;
    const int inner_height = box_height - 2;
    WINDOW* editor = newwin(inner_height, inner_width, box_y + 1, box_x + 1);
    if (!editor) return std::nullopt;
    keypad(editor, TRUE);
    wtimeout(editor, -1);
    curs_set(1);

    const std::string help = "Ctrl+S save  Enter newline  arrows move  Esc cancel";
    move(height - 2, box_x - 1);
    addnstr((help + std::string(std::max(0, main_width - 4 - static_cast<int>(help.size())), ' ')).c_str(),
            main_width - 4);
    refresh();

    bool save = false;
    while (true) {
        std::vector<std::string> visual;
        int cursor_y = 0;
        for (int row = 0; row < static_cast<int>(lines.size()); ++row) {
            const int chunks = static_cast<int>(lines[row].size()) / inner_width + 1;
            if (row < line) cursor_y += chunks;
            for (int chunk = 0; chunk < chunks; ++chunk)
                visual.push_back(lines[row].substr(chunk * inner_width, inner_width));
        }
        cursor_y += column / inner_width;
        const int cursor_x = column % inner_width;
        if (cursor_y < scroll) scroll = cursor_y;
        if (cursor_y >= scroll + inner_height) scroll = cursor_y - inner_height + 1;

        werase(editor);
        for (int row = 0; row < inner_height && scroll + row < static_cast<int>(visual.size()); ++row)
            mvwaddnstr(editor, row, 0, visual[scroll + row].c_str(), inner_width);
        wmove(editor, cursor_y - scroll, cursor_x);
        wrefresh(editor);

        const int key = wgetch(editor);
        if (key == 19) { save = true; break; }
        if (key == 27) break;
        if (key == '\n' || key == KEY_ENTER) {
            const std::string remainder = lines[line].substr(column);
            lines[line].erase(column);
            lines.insert(lines.begin() + line + 1, remainder);
            ++line; column = 0;
        } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
            if (column > 0) lines[line].erase(--column, 1);
            else if (line > 0) {
                column = static_cast<int>(lines[line - 1].size());
                lines[line - 1] += lines[line];
                lines.erase(lines.begin() + line);
                --line;
            }
        } else if (key == KEY_DC) {
            if (column < static_cast<int>(lines[line].size())) lines[line].erase(column, 1);
            else if (line + 1 < static_cast<int>(lines.size())) {
                lines[line] += lines[line + 1];
                lines.erase(lines.begin() + line + 1);
            }
        } else if (key == KEY_LEFT) {
            if (column > 0) --column;
            else if (line > 0) { --line; column = static_cast<int>(lines[line].size()); }
        } else if (key == KEY_RIGHT) {
            if (column < static_cast<int>(lines[line].size())) ++column;
            else if (line + 1 < static_cast<int>(lines.size())) { ++line; column = 0; }
        } else if (key == KEY_UP) {
            if (line > 0) { --line; column = std::min(column, static_cast<int>(lines[line].size())); }
        } else if (key == KEY_DOWN) {
            if (line + 1 < static_cast<int>(lines.size())) {
                ++line; column = std::min(column, static_cast<int>(lines[line].size()));
            }
        } else if (key == KEY_HOME) column = 0;
        else if (key == KEY_END) column = static_cast<int>(lines[line].size());
        else if (key >= 32 && key <= 255 && std::isprint(static_cast<unsigned char>(key))) {
            std::size_t size = 0;
            for (const auto& item : lines) size += item.size();
            if (size < 4096) lines[line].insert(lines[line].begin() + column++, static_cast<char>(key));
        }
    }

    delwin(editor);
    touchwin(stdscr);
    curs_set(0);
    timeout(250);
    if (!save) return std::nullopt;
    std::string result;
    for (int row = 0; row < static_cast<int>(lines.size()); ++row) {
        if (row) result.push_back('\n');
        result += lines[row];
    }
    return result;
}

char todo_scope(int height, const std::string& action) {
    const std::string value = prompt(action + " scope: [t]oday, [f]rom this date, [a]ll (Esc/cancel blank): ", height);
    return value.empty() ? '\0' : static_cast<char>(std::tolower(static_cast<unsigned char>(value[0])));
}

bool confirm(const std::string& question, int height) {
    const std::string answer = prompt(question + " [y/N]: ", height);
    return answer == "y" || answer == "Y" || answer == "yes" || answer == "YES";
}

planner::TimePoint shift_local_days(planner::TimePoint point, int days) {
    const std::time_t value = planner::Clock::to_time_t(point);
    std::tm tm{}; localtime_r(&value, &tm);
    tm.tm_mday += days; tm.tm_isdst = -1;
    return planner::Clock::from_time_t(mktime(&tm));
}

planner::TimePoint shift_gantt_period(planner::TimePoint point, int scale, int direction) {
    const std::time_t value = planner::Clock::to_time_t(point);
    std::tm tm{}; localtime_r(&value, &tm);
    if (scale == 0) tm.tm_mday += 7 * direction;
    else if (scale == 1) tm.tm_mon += direction;
    else tm.tm_year += direction;
    tm.tm_isdst = -1;
    return planner::Clock::from_time_t(mktime(&tm));
}

int gantt_item_count(const planner::State& state) {
    return (state.gantt_filter == 2 ? 0 : static_cast<int>(state.projects.size())) +
           (state.gantt_filter == 1 ? 0 : static_cast<int>(state.goals.size()));
}

planner::Project* selected_gantt_project(planner::State& state) {
    if (state.gantt_filter == 2 || state.gantt_row < 0 || state.gantt_row >= static_cast<int>(state.projects.size()))
        return nullptr;
    return &state.projects[state.gantt_row];
}

planner::Goal* selected_gantt_goal(planner::State& state) {
    if (state.gantt_filter == 1) return nullptr;
    const int offset = state.gantt_filter == 2 ? 0 : static_cast<int>(state.projects.size());
    const int row = state.gantt_row - offset;
    return row >= 0 && row < static_cast<int>(state.goals.size()) ? &state.goals[row] : nullptr;
}

std::string local_hhmmss(planner::TimePoint point) {
    const std::time_t value = planner::Clock::to_time_t(point);
    std::tm tm{}; localtime_r(&value, &tm);
    char result[9]{};
    std::strftime(result, sizeof(result), "%H:%M:%S", &tm);
    return result;
}

void refresh_state(planner::State& state, planner::Store& store, planner::TimePoint now) {
    const int project = state.project_index, column = state.column, row = state.row;
    const auto view = state.view;
    const auto calendar_date = state.calendar_date;
    const int schedule_row = state.schedule_row;
    const bool today_focused = state.today_focused;
    const bool project_focused = state.project_focused;
    const bool all_projects = state.all_projects;
    const int tracker_row = state.tracker_row;
    const int session_edit_row = state.session_edit_row;
    const int task_picker_row = state.task_picker_row;
    const auto editing_session_id = state.editing_session_id;
    const auto tracker_date = state.tracker_date;
    const bool calendar_selecting_tracker = state.calendar_selecting_tracker;
    const bool calendar_selecting_task_due = state.calendar_selecting_task_due;
    const bool calendar_selecting_todo = state.calendar_selecting_todo;
    const auto calendar_rescheduling_todo_id = state.calendar_rescheduling_todo_id;
    const auto calendar_rescheduling_task_id = state.calendar_rescheduling_task_id;
    const int calendar_selecting_session_date = state.calendar_selecting_session_date;
    const int menu_row = state.menu_row;
    const int edit_row = state.edit_row;
    const int checklist_row = state.checklist_row;
    const int allowance_row = state.allowance_row;
    const int todo_row = state.todo_row;
    const int todo_list = state.todo_list;
    const bool todo_date_focused = state.todo_date_focused;
    const int deadline_row = state.deadline_row;
    const int goal_row = state.goal_row;
    const int goal_edit_row = state.goal_edit_row;
    const auto editing_goal_id = state.editing_goal_id;
    const int gantt_row = state.gantt_row;
    const int gantt_scale = state.gantt_scale;
    const int gantt_filter = state.gantt_filter;
    const auto gantt_date = state.gantt_date;
    const auto todo_date = state.todo_date;
    const bool dragging = state.dragging;
    const auto dragged_task_id = state.dragged_task_id;
    const auto board_scroll = state.board_scroll;
    const auto previous_view = state.previous_view;
    const std::string message = state.message;
    state = store.load(now);
    planner::set_app_timezone(state.timezone);
    state.project_index = project; state.column = column; state.row = row; state.view = view;
    state.message = message;
    state.calendar_date = calendar_date;
    state.schedule_row = schedule_row;
    state.today_focused = today_focused;
    state.project_focused = project_focused;
    state.all_projects = all_projects;
    state.tracker_row = tracker_row;
    state.session_edit_row = session_edit_row;
    state.task_picker_row = task_picker_row;
    state.editing_session_id = editing_session_id;
    state.tracker_date = tracker_date;
    state.calendar_selecting_tracker = calendar_selecting_tracker;
    state.calendar_selecting_task_due = calendar_selecting_task_due;
    state.calendar_selecting_todo = calendar_selecting_todo;
    state.calendar_rescheduling_todo_id = calendar_rescheduling_todo_id;
    state.calendar_rescheduling_task_id = calendar_rescheduling_task_id;
    state.calendar_selecting_session_date = calendar_selecting_session_date;
    state.menu_row = menu_row;
    state.edit_row = edit_row;
    state.checklist_row = checklist_row;
    state.allowance_row = allowance_row;
    state.todo_row = todo_row;
    state.todo_list = todo_list;
    state.todo_date_focused = todo_date_focused;
    state.deadline_row = deadline_row;
    state.goal_row = goal_row;
    state.goal_edit_row = goal_edit_row;
    state.editing_goal_id = editing_goal_id;
    state.gantt_row = gantt_row;
    state.gantt_scale = gantt_scale;
    state.gantt_filter = gantt_filter;
    state.gantt_date = gantt_date;
    state.todo_date = todo_date;
    state.dragging = dragging;
    state.dragged_task_id = dragged_task_id;
    state.board_scroll = board_scroll;
    state.previous_view = previous_view;
    normalize(state);
}

void color_range(int y, int x, int length, int pair, int attributes = A_BOLD) {
    if (y < 0 || y >= LINES || x >= COLS || length <= 0) return;
    x = std::max(0, x);
    length = std::min(length, COLS - x);
    if (length > 0) mvchgat(y, x, length, attributes, static_cast<short>(pair), nullptr);
}

bool is_sleep_activity(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value.find("sleep") != std::string::npos;
}

int activity_color_pair(int id, bool allowance = false, const std::string& name = {}) {
    if (!allowance && is_sleep_activity(name)) return 16;
    const unsigned value = static_cast<unsigned>(id) * 2654435761u + (allowance ? 101u : 0u);
    return 7 + static_cast<int>(value % 7u);
}

void paint_tracker_timeline(const planner::State& state, int origin_x, int width, int height) {
    const auto [day_start, day_end] = local_day_bounds(state.tracker_date);
    const long long day_seconds = std::max<long long>(1,
        std::chrono::duration_cast<std::chrono::seconds>(day_end - day_start).count());
    const int bar_x = origin_x + 3, bar_width = std::max(8, width - 6);
    const bool show_focus = origin_x == 0;
    const int bar_y = show_focus ? height - 8 : height - 4;
    auto paint_segment = [&](planner::TimePoint raw_start, planner::TimePoint raw_end, int pair) {
        if (raw_start >= day_end || raw_end <= day_start) return;
        const auto start = std::max(raw_start, day_start), end = std::min(raw_end, day_end);
        const long long from = std::chrono::duration_cast<std::chrono::seconds>(start - day_start).count();
        const long long to = std::chrono::duration_cast<std::chrono::seconds>(end - day_start).count();
        const int x1 = std::clamp(static_cast<int>(from * bar_width / day_seconds), 0, bar_width - 1);
        const int x2 = std::clamp(static_cast<int>((to * bar_width + day_seconds - 1) / day_seconds), x1 + 1, bar_width);
        color_range(bar_y, bar_x + x1, x2 - x1, pair, A_BOLD);
    };
    int y = 5;
    const int limit = std::max(6, bar_y - 5);
    for (const auto& session : state.sessions) {
        const auto end = session.ended_at.value_or(state.now);
        if (session.started_at >= day_end || end <= day_start) continue;
        const auto task = std::find_if(state.tasks.begin(), state.tasks.end(), [&session](const auto& item) {
            return item.id == session.task_id;
        });
        const std::string name = task == state.tasks.end() ? "Unknown task" : task->title;
        const int pair = activity_color_pair(session.task_id, false, name);
        paint_segment(session.started_at, end, pair);
        if (y < limit) {
            const bool selected = y == 5 + state.tracker_row;
            color_range(y, origin_x + 2, width - 4, pair, A_BOLD | (selected ? A_REVERSE : 0));
            ++y;
        }
    }
    for (const auto& session : state.allowance_sessions) {
        const auto end = session.ended_at.value_or(state.now);
        if (session.started_at >= day_end || end <= day_start) continue;
        const int pair = activity_color_pair(session.allowance_id, true);
        paint_segment(session.started_at, end, pair);
        if (y < limit) color_range(y++, origin_x + 2, width - 4, pair, A_BOLD);
    }

    int legend_x = origin_x + 3;
    std::vector<int> seen_tasks, seen_allowances;
    auto label = [&](int id, bool allowance, const std::string& name, int pair) {
        auto& seen = allowance ? seen_allowances : seen_tasks;
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) return;
        seen.push_back(id);
        const int label_width = 3 + std::min(14, static_cast<int>(name.size()));
        if (legend_x + label_width >= origin_x + width - 2) return;
        color_range(bar_y - 2, legend_x, label_width, pair, A_BOLD);
        legend_x += label_width + 2;
    };
    for (const auto& session : state.sessions) {
        const auto end = session.ended_at.value_or(state.now);
        if (session.started_at >= day_end || end <= day_start) continue;
        const auto task = std::find_if(state.tasks.begin(), state.tasks.end(), [&session](const auto& item) {
            return item.id == session.task_id;
        });
        const std::string name = task == state.tasks.end() ? "Unknown task" : task->title;
        label(session.task_id, false, name, activity_color_pair(session.task_id, false, name));
    }
    for (const auto& session : state.allowance_sessions) {
        const auto end = session.ended_at.value_or(state.now);
        if (session.started_at >= day_end || end <= day_start) continue;
        const auto allowance = std::find_if(state.allowances.begin(), state.allowances.end(), [&session](const auto& item) {
            return item.id == session.allowance_id;
        });
        label(session.allowance_id, true, allowance == state.allowances.end() ? "Allowance" : allowance->name,
              activity_color_pair(session.allowance_id, true));
    }
}

void paint_due_rail(int rail_width, int height) {
    int mode = 0;
    std::vector<char> buffer(static_cast<std::size_t>(rail_width + 1), '\0');
    for (int y = 16; y < height - 2; ++y) {
        std::fill(buffer.begin(), buffer.end(), '\0');
        mvinnstr(y, 0, buffer.data(), rail_width);
        const std::string line(buffer.data());
        if (line.find("DUE TODAY") != std::string::npos) {
            mode = 1; color_range(y, 1, rail_width - 2, 2, A_BOLD); continue;
        }
        if (line.find("OVERDUE") != std::string::npos) {
            mode = 2; color_range(y, 1, rail_width - 2, 14, A_BOLD); continue;
        }
        if (line.find("UPCOMING") != std::string::npos) {
            mode = 3; color_range(y, 1, rail_width - 2, 3, A_BOLD); continue;
        }
        if (mode && (line.find("! ") != std::string::npos || line.find("x ") != std::string::npos ||
                     line.find("+ ") != std::string::npos || line.find("Nothing due") != std::string::npos ||
                     line.find("Nothing overdue") != std::string::npos || line.find("Nothing upcoming") != std::string::npos))
            color_range(y, 1, rail_width - 2, mode == 1 ? 2 : mode == 2 ? 14 : 3, A_BOLD);
    }
}

void paint_colors(const planner::State& state, int width, int height) {
    if (!has_colors()) return;
    if (width >= 90) {
        const int rail = std::min(38, std::max(32, width / 3));
        color_range(0, 0, rail, 3);
        color_range(4, 1, rail - 2, 3);
        color_range(5, 1, rail - 2, (state.timer_task_id || state.active_allowance_id) ? 6 :
                    ((state.paused_task_id || state.paused_allowance_id) ? 2 : 1));
        color_range(15, 1, rail - 2, 3);
        paint_due_rail(rail, height);
        color_range(0, rail, width - rail, 1);
        color_range(height - 2, rail + 1, width - rail - 2, 1);
        if (state.view == planner::View::Menu) {
            color_range(5 + state.menu_row * 2, rail + 4, width - rail - 8, 3, A_BOLD | A_REVERSE);
        } else if (state.view == planner::View::EditTask) {
            color_range(2, rail + 2, width - rail - 4, 3);
            color_range(5 + state.edit_row * 3, rail + 2, width - rail - 4, 3, A_BOLD | A_REVERSE);
        } else if (state.view == planner::View::TaskChecklist) {
            color_range(2, rail + 2, width - rail - 4, 3);
            const auto items = selected_task_checklist(state);
            if (!items.empty()) {
                const int capacity = std::max(1, height - 8);
                const int selected = std::clamp(state.checklist_row, 0, static_cast<int>(items.size()) - 1);
                const int first = std::clamp(selected - capacity + 1, 0,
                    std::max(0, static_cast<int>(items.size()) - capacity));
                color_range(6 + selected - first, rail + 2, width - rail - 4, 3, A_BOLD | A_REVERSE);
            }
        } else if (state.view == planner::View::EditGoal) {
            color_range(2, rail + 2, width - rail - 4, 3);
            const int selected_y = state.goal_edit_row == 6 ? 12 : 5 + state.goal_edit_row;
            color_range(selected_y, rail + 2, width - rail - 4, 3, A_BOLD | A_REVERSE);
        } else if (state.view == planner::View::Tracker || state.view == planner::View::Allowances) {
            color_range(2, rail + 2, width - rail - 4, 3);
            if (state.view == planner::View::Tracker)
                paint_tracker_timeline(state, rail, width - rail, height);
            if (state.view == planner::View::Allowances && !state.today_focused && !state.allowances.empty()) {
                const int y = 5 + state.allowance_row * 2;
                color_range(y, rail + 2, width - rail - 4, 3, A_BOLD | A_REVERSE);
                color_range(y + 1, rail + 2, width - rail - 4, 3, A_BOLD | A_REVERSE);
            }
        } else if (state.view == planner::View::Goals) {
            color_range(2, rail + 2, width - rail - 4, 3);
            if (!state.goals.empty()) color_range(goal_net_y(state, height), rail + 2, width - rail - 4,
                selected_goal_net(state) >= 0 ? 5 : 14, A_BOLD);
        } else if (state.view == planner::View::Help) {
            color_range(2, rail + 2, width - rail - 4, 3);
        } else if (state.view == planner::View::Board) {
            const int main_width = width - rail;
            const int sidebar = std::min(20, main_width / 5);
            color_range(2, rail + 1, sidebar - 1, 3);
            color_range(state.all_projects ? 4 : 5 + state.project_index, rail + 1, sidebar - 1, 3,
                        state.project_focused ? A_BOLD | A_REVERSE : A_BOLD);
            const int start = rail + sidebar + 1;
            const int board_width = main_width - sidebar - 2;
            const int column_width = std::max(12, board_width / 4);
            for (int column = 0; column < 4; ++column) {
                const int x = start + column * column_width;
                color_range(4, x, std::min(column_width, width - x - 1), 2 + column);
                const auto tasks = planner::visible_tasks(state, static_cast<planner::Status>(column));
                const int capacity = std::max(1, (height - 8) / 3);
                int first = std::clamp(state.board_scroll[column], 0,
                    std::max(0, static_cast<int>(tasks.size()) - capacity));
                if (!state.project_focused && state.column == column) {
                    if (state.row < first) first = state.row;
                    else if (state.row >= first + capacity) first = state.row - capacity + 1;
                }
                for (int row = first; row < static_cast<int>(tasks.size()) && row < first + capacity; ++row) {
                    const bool selected = !state.project_focused && state.column == column && state.row == row;
                    const int pair = selected && column == 0 ? 15 : 2 + column;
                    const int attributes = selected && column != 0 ? A_BOLD | A_REVERSE : A_BOLD;
                    for (int dy = 0; dy < 3; ++dy)
                        color_range(6 + (row - first) * 3 + dy, x,
                                    std::min(column_width - 1, width - x - 1),
                                    pair, attributes);
                }
            }
        }
        return;
    }
    color_range(0, 0, width, 1);
    color_range(height - 2, 1, width - 2, 1);

    if (state.view == planner::View::Calendar) {
        color_range(2, 1, width - 2, 3);
        color_range(height - 6, 1, width - 2, state.timer_task_id ? 6 : (state.paused_task_id ? 2 : 3));
        color_range(height - 5, 1, width - 2, 3);
        color_range(height - 4, 1, width - 2, state.timer_task_id ? 6 : (state.paused_task_id ? 2 : 1));
        return;
    }
    if (state.view == planner::View::Task) {
        color_range(3, 2, width - 4, 3);
        color_range(height - 6, 1, width - 2, state.timer_task_id ? 6 : (state.paused_task_id ? 2 : 1));
        color_range(height - 4, 1, width - 2, state.timer_task_id ? 6 : (state.paused_task_id ? 2 : 1));
        return;
    }
    if (state.view == planner::View::EditTask) {
        color_range(2, 1, width - 2, 3);
        color_range(5 + state.edit_row * 3, 2, width - 4, 3, A_BOLD | A_REVERSE);
        return;
    }
    if (state.view == planner::View::TaskChecklist) {
        color_range(2, 1, width - 2, 3);
        const auto items = selected_task_checklist(state);
        if (!items.empty()) {
            const int capacity = std::max(1, height - 8);
            const int selected = std::clamp(state.checklist_row, 0, static_cast<int>(items.size()) - 1);
            const int first = std::clamp(selected - capacity + 1, 0,
                std::max(0, static_cast<int>(items.size()) - capacity));
            color_range(6 + selected - first, 2, width - 4, 3, A_BOLD | A_REVERSE);
        }
        return;
    }
    if (state.view == planner::View::EditGoal) {
        color_range(2, 1, width - 2, 3);
        const int selected_y = state.goal_edit_row == 6 ? 12 : 5 + state.goal_edit_row;
        color_range(selected_y, 2, width - 4, 3, A_BOLD | A_REVERSE);
        return;
    }
    if (state.view == planner::View::Menu) {
        color_range(2, 1, width - 2, 3);
        color_range(5 + state.menu_row * 2, 4, width - 8, 3, A_BOLD | A_REVERSE);
        return;
    }
    if (state.view == planner::View::Tracker || state.view == planner::View::Allowances ||
        state.view == planner::View::Goals || state.view == planner::View::EditGoal ||
        state.view == planner::View::Gantt ||
        state.view == planner::View::Help) {
        color_range(2, 1, width - 2, 3);
        if (state.view == planner::View::Tracker) paint_tracker_timeline(state, 0, width, height);
        if (state.view == planner::View::Goals && !state.goals.empty())
            color_range(goal_net_y(state, height), 2, width - 4,
                        selected_goal_net(state) >= 0 ? 5 : 14, A_BOLD);
        if (state.view == planner::View::Allowances && !state.allowances.empty()) {
            const int y = 5 + state.allowance_row * 2;
            color_range(y, 2, width - 4, 3, A_BOLD | A_REVERSE);
            color_range(y + 1, 2, width - 4, 3, A_BOLD | A_REVERSE);
        }
        return;
    }

    if (state.view != planner::View::Board) {
        color_range(2, 1, width - 2, 3);
        if (state.view == planner::View::Todos) {
            const int timer_y = height - 6;
            color_range(timer_y, 1, width - 2,
                        state.timer_task_id ? 6 : (state.paused_task_id ? 2 : 1));
            color_range(height - 4, 1, width - 2,
                        state.timer_task_id ? 6 : (state.paused_task_id ? 2 : 1));
        }
        return;
    }

    const bool compact = width < 72;
    const int timer_y = height - 6;
    color_range(timer_y, 1, width - 2, state.timer_task_id ? 6 : (state.paused_task_id ? 2 : 1));
    color_range(height - 4, 1, width - 2, state.timer_task_id ? 6 : (state.paused_task_id ? 2 : 1));
    if (compact) {
        color_range(4, 1, width - 2, 2 + state.column);
        const auto tasks = planner::visible_tasks(state, static_cast<planner::Status>(state.column));
        const int capacity = std::max(1, (timer_y - 5) / 2);
        int first = std::clamp(state.board_scroll[state.column], 0,
            std::max(0, static_cast<int>(tasks.size()) - capacity));
        if (state.row < first) first = state.row;
        else if (state.row >= first + capacity) first = state.row - capacity + 1;
        const int selected_y = 6 + (state.row - first) * 2;
        color_range(selected_y, 1, width - 2, state.column == 0 ? 15 : 2 + state.column,
                    state.column == 0 ? A_BOLD : A_BOLD | A_REVERSE);
        return;
    }

    const int sidebar = std::min(20, width / 5);
    color_range(2, 1, sidebar - 1, 3);
    color_range(state.all_projects ? 4 : 5 + state.project_index, 1, sidebar - 1, 3,
                state.project_focused ? A_BOLD | A_REVERSE : A_BOLD);
    const int start_x = sidebar + 1;
    const int board_width = width - start_x - 1;
    const int column_width = std::max(12, board_width / 4);
    for (int column = 0; column < 4; ++column) {
        const int x = start_x + column * column_width;
        color_range(4, x, std::min(column_width, width - x - 1), 2 + column);
        const auto tasks = planner::visible_tasks(state, static_cast<planner::Status>(column));
        const int capacity = std::max(1, (timer_y - 6) / 3);
        int first = std::clamp(state.board_scroll[column], 0,
            std::max(0, static_cast<int>(tasks.size()) - capacity));
        if (!state.project_focused && state.column == column) {
            if (state.row < first) first = state.row;
            else if (state.row >= first + capacity) first = state.row - capacity + 1;
        }
        for (int row = first; row < static_cast<int>(tasks.size()) && row < first + capacity; ++row) {
            const bool selected = !state.project_focused && state.column == column && state.row == row;
            const int pair = selected && column == 0 ? 15 : 2 + column;
            const int attributes = selected && column != 0 ? A_BOLD | A_REVERSE : A_BOLD;
            const int y = 6 + (row - first) * 3;
            color_range(y, x, std::min(column_width - 1, width - x - 1), pair, attributes);
            color_range(y + 1, x, std::min(column_width - 1, width - x - 1), pair, attributes);
            color_range(y + 2, x, std::min(column_width - 1, width - x - 1), pair, attributes);
        }
    }
}

void run_tui(planner::State& state, planner::Store& store, bool fixed_clock) {
    initscr(); raw(); noecho(); keypad(stdscr, TRUE); curs_set(0); timeout(250);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_WHITE, -1);    // chrome
        init_pair(2, COLOR_YELLOW, -1);   // backlog
        init_pair(3, COLOR_CYAN, -1);     // ready and headings
        init_pair(4, COLOR_MAGENTA, -1);  // doing
        init_pair(5, COLOR_GREEN, -1);    // done
        init_pair(6, COLOR_BLACK, COLOR_GREEN);  // high-contrast running timer
        init_pair(7, COLOR_WHITE, COLOR_RED);
        init_pair(8, COLOR_BLACK, COLOR_YELLOW);
        init_pair(9, COLOR_BLACK, COLOR_GREEN);
        init_pair(10, COLOR_BLACK, COLOR_CYAN);
        init_pair(11, COLOR_WHITE, COLOR_BLUE);
        init_pair(12, COLOR_WHITE, COLOR_MAGENTA);
        init_pair(13, COLOR_BLACK, COLOR_WHITE);
        init_pair(14, COLOR_RED, -1);
        init_pair(15, COLOR_BLACK, COLOR_YELLOW);  // readable selected Backlog card
        init_pair(16, COLOR_BLACK, COLORS >= 256 ? 180 : COLOR_YELLOW);  // sleep: light tan/brown
    }
    bool running = true;
    while (running) {
        if (!fixed_clock) state.now = planner::Clock::now();
        int height = 0, width = 0; getmaxyx(stdscr, height, width);
        const int rail_width = width >= 90 ? std::min(38, std::max(32, width / 3)) : 0;
        const int main_width = width - rail_width;
        const bool project_panel_visible = main_width >= 72;
        if (state.view == planner::View::Board)
            reveal_board_selection(state, main_width, height, width < 90);
        attr_set(A_NORMAL, 0, nullptr);
        bkgdset(static_cast<chtype>(' ') | COLOR_PAIR(0));
        erase();
        const auto rows = planner::render(state, width, height);
        for (int y = 0; y < static_cast<int>(rows.size()); ++y) {
            attr_set(A_NORMAL, 0, nullptr);
            mvaddnstr(y, 0, rows[y].c_str(), width);
        }
        paint_colors(state, width, height);
        refresh();
        const int key = getch();
        state.message.clear();
        if (key == ERR) continue;
        if (key == 'q') running = false;
        else if (key == 26) {
            if (store.undo()) { refresh_state(state, store, state.now); state.message = "Last action undone."; }
            else state.message = "Nothing to undo.";
        }
        else if (key == 25) {
            if (store.redo()) { refresh_state(state, store, state.now); state.message = "Last undone action redone."; }
            else state.message = "Nothing to redo.";
        }
        else if (key == 'M') {
            if (state.view == planner::View::Menu) state.view = state.previous_view;
            else { state.previous_view = state.view; state.view = planner::View::Menu; state.today_focused = false; }
        }
        else if (key == '?') { state.previous_view = state.view; state.view = planner::View::Help; state.today_focused = false; }
        else if (state.view == planner::View::Menu && key == 27) state.view = state.previous_view;
        else if (state.view == planner::View::Menu && (key == 'k' || key == KEY_UP))
            state.menu_row = std::max(0, state.menu_row - 1);
        else if (state.view == planner::View::Menu && (key == 'j' || key == KEY_DOWN))
            state.menu_row = std::min(9, state.menu_row + 1);
        else if (state.view == planner::View::Menu && (key == '\n' || key == KEY_ENTER)) {
            static constexpr planner::View views[]{planner::View::Board, planner::View::Todos,
                                                   planner::View::Deadlines, planner::View::Calendar,
                                                   planner::View::Tracker, planner::View::Allowances,
                                                   planner::View::Goals, planner::View::Gantt, planner::View::Settings,
                                                   planner::View::Help};
            state.view = views[state.menu_row];
        }
        else if (state.view == planner::View::Calendar && state.calendar_selecting_tracker &&
                 (key == '\n' || key == KEY_ENTER)) {
            state.tracker_date = state.calendar_date;
            state.tracker_row = -1;
            state.calendar_selecting_tracker = false;
            state.view = planner::View::Tracker;
            state.message = "Tracker date selected.";
        }
        else if (state.view == planner::View::Calendar && state.calendar_selecting_tracker && key == 27) {
            state.calendar_selecting_tracker = false;
            state.view = planner::View::Tracker;
            state.message = "Date selection cancelled.";
        }
        else if (state.view == planner::View::Calendar && state.calendar_selecting_task_due &&
                 (key == '\n' || key == KEY_ENTER)) {
            if (auto* task = planner::selected_task(state)) {
                task->due_date = planner::date_iso(state.calendar_date);
                store.checkpoint(); store.update_task(*task);
                state.calendar_selecting_task_due = false;
                state.view = planner::View::EditTask;
                refresh_state(state, store, state.now); state.message = "Task deadline set.";
            }
        }
        else if (state.view == planner::View::Calendar && state.calendar_selecting_task_due && key == 'c') {
            if (auto* task = planner::selected_task(state)) {
                task->due_date.clear(); store.checkpoint(); store.update_task(*task);
                state.calendar_selecting_task_due = false;
                state.view = planner::View::EditTask;
                refresh_state(state, store, state.now); state.message = "Task deadline cleared.";
            }
        }
        else if (state.view == planner::View::Calendar && state.calendar_selecting_task_due && key == 27) {
            state.calendar_selecting_task_due = false;
            state.view = planner::View::EditTask;
            state.message = "Deadline selection cancelled.";
        }
        else if (state.view == planner::View::Calendar &&
                 (state.calendar_rescheduling_todo_id || state.calendar_rescheduling_task_id) &&
                 (key == '\n' || key == KEY_ENTER)) {
            const std::string target_date = planner::date_iso(state.calendar_date);
            bool moved = false;
            if (state.calendar_rescheduling_todo_id) {
                const int id = *state.calendar_rescheduling_todo_id;
                const auto found = std::find_if(state.todos.begin(), state.todos.end(), [id](const auto& todo) {
                    return todo.id == id;
                });
                if (found != state.todos.end()) {
                    planner::Todo changed = *found;
                    if (changed.date != target_date) {
                        changed.date = target_date;
                        store.checkpoint(); store.update_todo(changed); moved = true;
                    }
                }
            } else {
                const int id = *state.calendar_rescheduling_task_id;
                const auto found = std::find_if(state.tasks.begin(), state.tasks.end(), [id](const auto& task) {
                    return task.id == id;
                });
                if (found != state.tasks.end()) {
                    planner::Task changed = *found;
                    if (changed.due_date != target_date) {
                        changed.due_date = target_date;
                        store.checkpoint(); store.update_task(changed); moved = true;
                    }
                }
            }
            state.calendar_rescheduling_todo_id.reset();
            state.calendar_rescheduling_task_id.reset();
            state.view = planner::View::Todos;
            if (moved) {
                refresh_state(state, store, state.now);
                state.todo_row = std::min(state.todo_row, std::max(0, todo_count(state) - 1));
                state.message = "Todo moved to " + target_date + ".";
            } else state.message = "Todo date was unchanged.";
        }
        else if (state.view == planner::View::Calendar &&
                 (state.calendar_rescheduling_todo_id || state.calendar_rescheduling_task_id) && key == 27) {
            state.calendar_rescheduling_todo_id.reset();
            state.calendar_rescheduling_task_id.reset();
            state.view = planner::View::Todos;
            state.message = "Todo reschedule cancelled.";
        }
        else if (state.view == planner::View::Calendar && state.calendar_selecting_todo &&
                 (key == '\n' || key == KEY_ENTER)) {
            state.todo_date = state.calendar_date;
            state.todo_row = 0;
            state.todo_date_focused = true;
            state.calendar_selecting_todo = false;
            state.view = planner::View::Todos;
            state.message = "Todo date selected.";
        }
        else if (state.view == planner::View::Calendar && state.calendar_selecting_todo && key == 27) {
            state.calendar_selecting_todo = false;
            state.view = planner::View::Todos;
            state.message = "Todo date selection cancelled.";
        }
        else if (state.view == planner::View::Calendar && state.calendar_selecting_session_date &&
                 (key == '\n' || key == KEY_ENTER)) {
            const auto* session = state.editing_session_id ? session_by_id(state, *state.editing_session_id) : nullptr;
            if (!session) state.message = "Tracked interval is unavailable.";
            else try {
                const bool start_field = state.calendar_selecting_session_date == 1;
                const auto old_point = start_field ? session->started_at : *session->ended_at;
                const auto changed = planner::parse_time(planner::date_iso(state.calendar_date) + "T" +
                                                         local_hhmmss(old_point));
                store.checkpoint();
                if (start_field) store.update_session_start(session->id, changed);
                else store.update_session(session->id, session->started_at, changed);
                state.calendar_selecting_session_date = 0; state.view = planner::View::EditSession;
                refresh_state(state, store, state.now); state.message = "Tracked interval date corrected.";
            } catch (const std::exception&) {
                state.message = "Invalid interval: the end must be after the start.";
            }
        }
        else if (state.view == planner::View::Calendar && state.calendar_selecting_session_date && key == 27) {
            state.calendar_selecting_session_date = 0; state.view = planner::View::EditSession;
            state.message = "Interval date selection cancelled.";
        }
        else if (key == '\t') { state.today_focused = !state.today_focused; state.project_focused = false; }
        else if (state.today_focused && (key == 'k' || key == KEY_UP))
            state.schedule_row = std::max(-1, state.schedule_row - 1);
        else if (state.today_focused && (key == 'j' || key == KEY_DOWN))
            state.schedule_row = std::min(std::max(0, today_schedule_count(state) - 1), state.schedule_row + 1);
        else if (state.today_focused && (key == 'l' || key == KEY_RIGHT)) {
            state.today_focused = false;
            state.project_focused = project_panel_visible && state.view == planner::View::Board;
        }
        else if (state.today_focused && (key == '\n' || key == KEY_ENTER)) {
            if (const auto* task = scheduled_task(state)) {
                state.previous_view = state.view;
                reveal_task(state, task->id, true); state.today_focused = false;
            }
        } else if (state.today_focused && key == 't') {
            if (const auto* task = scheduled_task(state)) {
                const int task_id = task->id;
                store.checkpoint();
                store.start_timer(task_id, state.now); store.clear_paused();
                reveal_task(state, task_id, false); refresh_state(state, store, state.now);
                state.message = "Scheduled task started.";
            }
        }
        else if (state.today_focused && key == ' ') {
            if (state.active_allowance_id) {
                store.checkpoint(); const int allowance_id = *state.active_allowance_id;
                store.stop_timer(state.now); store.remember_paused_allowance(allowance_id);
                refresh_state(state, store, state.now); state.message = "Allowance timer paused.";
            } else if (state.paused_allowance_id) {
                store.checkpoint(); store.start_allowance(*state.paused_allowance_id, state.now); store.clear_paused();
                refresh_state(state, store, state.now); state.message = "Allowance timer resumed.";
            } else if (state.timer_task_id) {
                store.checkpoint();
                const int task_id = *state.timer_task_id; store.stop_timer(state.now);
                store.remember_paused_task(task_id);
                refresh_state(state, store, state.now); state.message = "Focus timer paused.";
            } else if (state.paused_task_id) {
                store.checkpoint();
                store.start_timer(*state.paused_task_id, state.now); store.clear_paused();
                refresh_state(state, store, state.now);
                state.message = "Focus timer resumed.";
            }
        }
        else if (state.view == planner::View::Board && state.project_focused &&
                 (key == 'k' || key == KEY_UP)) {
            if (!state.all_projects && state.project_index == 0) state.all_projects = true;
            else if (!state.all_projects) --state.project_index;
            state.row = 0;
        }
        else if (state.view == planner::View::Board && state.project_focused &&
                 (key == 'j' || key == KEY_DOWN)) {
            if (state.all_projects) { state.all_projects = false; state.project_index = 0; }
            else state.project_index = std::min(std::max(0, static_cast<int>(state.projects.size()) - 1),
                                                state.project_index + 1);
            state.row = 0;
        }
        else if (state.view == planner::View::Board && state.project_focused &&
                 (key == 'h' || key == KEY_LEFT)) {
            if (rail_width > 0) {
                state.project_focused = false; state.today_focused = true; state.schedule_row = -1;
            }
            else state.message = "The Daily Calendar needs a terminal width of at least 90 columns.";
        }
        else if (state.view == planner::View::Board && state.project_focused &&
                 (key == 'l' || key == KEY_RIGHT || key == '\n' || key == KEY_ENTER)) {
            if (state.dragging && state.dragged_task_id && !state.all_projects) {
                const auto* project = planner::selected_project(state);
                if (project) {
                    store.checkpoint(); store.move_task_to_project(*state.dragged_task_id, project->id);
                    refresh_state(state, store, state.now); state.message = "Card transferred; choose its position or Space to drop.";
                }
            }
            state.project_focused = false; state.row = 0;
        }
        else if (state.view == planner::View::Tracker && (key == 'k' || key == KEY_UP))
            state.tracker_row = std::max(-1, state.tracker_row - 1);
        else if (state.view == planner::View::Tracker && (key == 'j' || key == KEY_DOWN))
            state.tracker_row = std::min(std::max(0, today_session_count(state) - 1), state.tracker_row + 1);
        else if (state.view == planner::View::Tracker && state.tracker_row < 0 &&
                 (key == 'h' || key == KEY_LEFT))
            state.tracker_date = shift_local_days(state.tracker_date, -1);
        else if (state.view == planner::View::Tracker && state.tracker_row < 0 &&
                 (key == 'l' || key == KEY_RIGHT))
            state.tracker_date = shift_local_days(state.tracker_date, 1);
        else if (state.view == planner::View::Tracker && state.tracker_row < 0 && key == 'g')
            state.tracker_date = state.now;
        else if (state.view == planner::View::Tracker && state.tracker_row < 0 &&
                 (key == '\n' || key == KEY_ENTER)) {
            state.calendar_date = state.tracker_date;
            state.calendar_selecting_tracker = true;
            state.view = planner::View::Calendar;
        }
        else if (state.view == planner::View::Tracker && key == 'e') {
            if (const auto* session = selected_tracker_session(state)) {
                state.editing_session_id = session->id;
                state.session_edit_row = 0;
                state.view = planner::View::EditSession;
            }
        }
        else if (state.view == planner::View::EditSession && (key == 'k' || key == KEY_UP))
            state.session_edit_row = std::max(0, state.session_edit_row - 1);
        else if (state.view == planner::View::EditSession && (key == 'j' || key == KEY_DOWN))
            state.session_edit_row = std::min(4, state.session_edit_row + 1);
        else if (state.view == planner::View::EditSession && (key == 'e' || key == '\n' || key == KEY_ENTER)) {
            const auto* session = state.editing_session_id ? session_by_id(state, *state.editing_session_id) : nullptr;
            if (!session) state.message = "Tracked interval is unavailable.";
            else if (state.session_edit_row == 0) {
                const auto tasks = picker_tasks(state);
                state.task_picker_row = 0;
                const auto current = std::find_if(tasks.begin(), tasks.end(), [session](const auto* task) {
                    return task->id == session->task_id;
                });
                if (current != tasks.end()) state.task_picker_row = static_cast<int>(current - tasks.begin());
                state.view = planner::View::SessionTaskPicker;
            } else if (state.session_edit_row == 1 || state.session_edit_row == 3) {
                if (state.session_edit_row == 3 && !session->ended_at)
                    state.message = "The active interval has no end date; pause or stop it first.";
                else {
                    const auto point = state.session_edit_row == 1 ? session->started_at : *session->ended_at;
                    state.calendar_date = point;
                    state.calendar_selecting_session_date = state.session_edit_row == 1 ? 1 : 2;
                    state.view = planner::View::Calendar;
                }
            } else if (state.session_edit_row == 2) {
                const std::string value = prompt("Start HH:MM (blank keeps current): ", height);
                if (!value.empty()) {
                    try {
                        const auto start = planner::parse_time(planner::date_iso(session->started_at) + "T" + value + ":00");
                        if (!session->ended_at && start > state.now)
                            throw std::invalid_argument("future active start");
                        store.checkpoint(); store.update_session_start(session->id, start);
                        refresh_state(state, store, state.now); state.message = "Tracked time corrected.";
                    } catch (const std::exception&) {
                        state.message = "Invalid interval. Use HH:MM with the end after the start.";
                    }
                }
            } else if (!session->ended_at) {
                state.message = "The active interval has no end time; pause or stop it first.";
            } else {
                const std::string value = prompt("End HH:MM (blank keeps current): ", height);
                if (!value.empty()) {
                    try {
                        const auto end = planner::parse_time(planner::date_iso(*session->ended_at) + "T" + value + ":00");
                        store.checkpoint(); store.update_session(session->id, session->started_at, end);
                        refresh_state(state, store, state.now); state.message = "Tracked time corrected.";
                    } catch (const std::exception&) {
                        state.message = "Invalid interval. Use HH:MM with the end after the start.";
                    }
                }
            }
        }
        else if (state.view == planner::View::SessionTaskPicker && (key == 'k' || key == KEY_UP))
            state.task_picker_row = std::max(0, state.task_picker_row - 1);
        else if (state.view == planner::View::SessionTaskPicker && (key == 'j' || key == KEY_DOWN))
            state.task_picker_row = std::min(std::max(0, static_cast<int>(picker_tasks(state).size()) - 1),
                                             state.task_picker_row + 1);
        else if (state.view == planner::View::SessionTaskPicker && (key == '\n' || key == KEY_ENTER)) {
            const auto tasks = picker_tasks(state);
            if (state.editing_session_id && !tasks.empty()) {
                const auto* task = tasks[std::clamp(state.task_picker_row, 0, static_cast<int>(tasks.size()) - 1)];
                const std::string task_title = task->title;
                store.checkpoint(); store.update_session_task(*state.editing_session_id, task->id);
                refresh_state(state, store, state.now); state.view = planner::View::EditSession;
                state.message = "Tracked interval moved to " + task_title + ".";
            }
        }
        else if (state.view == planner::View::Tracker && key == 'n') {
            if (const auto* task = planner::selected_task(state)) {
                const int task_id = task->id;
                const std::string date = planner::date_iso(state.tracker_date);
                const std::string start = prompt("Manual start HH:MM: ", height);
                const std::string end = prompt("Manual end HH:MM: ", height);
                try {
                    store.checkpoint();
                    store.add_session(task_id, planner::parse_time(date + "T" + start + ":00"),
                        planner::parse_time(date + "T" + end + ":00"));
                    refresh_state(state, store, state.now); state.message = "Manual interval added.";
                } catch (const std::exception&) {
                    state.message = "Invalid interval. Select a Board task first and use HH:MM.";
                }
            } else state.message = "Return to the Board and select a task before adding an interval.";
        }
        else if (state.view == planner::View::Allowances && (key == 'k' || key == KEY_UP))
            state.allowance_row = std::max(0, state.allowance_row - 1);
        else if (state.view == planner::View::Allowances && (key == 'j' || key == KEY_DOWN))
            state.allowance_row = std::min(std::max(0, static_cast<int>(state.allowances.size()) - 1),
                                           state.allowance_row + 1);
        else if (state.view == planner::View::Allowances && (key == 'h' || key == KEY_LEFT)) {
            if (rail_width > 0) { state.today_focused = true; state.schedule_row = -1; }
            else state.message = "The Today panel needs a terminal width of at least 90 columns.";
        }
        else if (state.view == planner::View::Allowances && key == 'n') {
            const auto name = prompt("Allowance name: ", height);
            const auto budget = prompt("Budget minutes: ", height);
            const auto period = prompt("Period daily/weekly [daily]: ", height);
            try {
                if (name.empty()) throw std::invalid_argument("empty name");
                store.checkpoint();
                store.add_allowance(name, std::stoi(budget), period.empty() ? "daily" : period);
                refresh_state(state, store, state.now); state.allowance_row = state.allowances.size() - 1;
                state.message = "Allowance created.";
            } catch (const std::exception&) { state.message = "Invalid allowance; use a positive budget and daily/weekly."; }
        }
        else if (state.view == planner::View::Allowances && key == 'e' && !state.allowances.empty()) {
            auto& allowance = state.allowances[std::clamp(state.allowance_row, 0,
                static_cast<int>(state.allowances.size()) - 1)];
            const auto name = prompt("Allowance name (blank keeps current): ", height);
            const auto budget = prompt("Budget minutes (blank keeps current): ", height);
            const auto period = prompt("Period daily/weekly (blank keeps current): ", height);
            try {
                if (!name.empty()) allowance.name = name;
                if (!budget.empty()) allowance.budget_minutes = std::stoi(budget);
                if (!period.empty()) allowance.period = period;
                if (allowance.budget_minutes <= 0 || (allowance.period != "daily" && allowance.period != "weekly"))
                    throw std::invalid_argument("invalid allowance");
                store.checkpoint(); store.update_allowance(allowance);
                refresh_state(state, store, state.now); state.message = "Allowance updated.";
            } catch (const std::exception&) { refresh_state(state, store, state.now); state.message = "Invalid allowance values."; }
        }
        else if (state.view == planner::View::Allowances && key == 't' && !state.allowances.empty()) {
            const auto& allowance = state.allowances[std::clamp(state.allowance_row, 0,
                static_cast<int>(state.allowances.size()) - 1)];
            store.checkpoint(); store.start_allowance(allowance.id, state.now);
            store.clear_paused();
            refresh_state(state, store, state.now); state.message = "Allowance timer started.";
        }
        else if (state.view == planner::View::Allowances && key == KEY_DC && !state.allowances.empty()) {
            const auto& allowance = state.allowances[std::clamp(state.allowance_row, 0,
                static_cast<int>(state.allowances.size()) - 1)];
            if (state.active_allowance_id == allowance.id) state.message = "Stop this allowance before deleting it.";
            else if (confirm("Delete allowance '" + allowance.name + "'?", height)) {
                try { store.checkpoint(); store.delete_allowance(allowance.id); refresh_state(state, store, state.now);
                      state.message = "Allowance deleted."; }
                catch (const std::exception& error) { state.message = error.what(); }
            }
        }
        else if (state.view == planner::View::Gantt && (key == 'k' || key == KEY_UP))
            state.gantt_row = std::max(0, state.gantt_row - 1);
        else if (state.view == planner::View::Gantt && (key == 'j' || key == KEY_DOWN))
            state.gantt_row = std::min(std::max(0, gantt_item_count(state) - 1), state.gantt_row + 1);
        else if (state.view == planner::View::Gantt && (key == 'h' || key == KEY_LEFT))
            state.gantt_date = shift_gantt_period(state.gantt_date, state.gantt_scale, -1);
        else if (state.view == planner::View::Gantt && (key == 'l' || key == KEY_RIGHT))
            state.gantt_date = shift_gantt_period(state.gantt_date, state.gantt_scale, 1);
        else if (state.view == planner::View::Gantt && (key == 'w' || key == 'm' || key == 'y')) {
            state.gantt_scale = key == 'w' ? 0 : key == 'm' ? 1 : 2;
            state.gantt_date = state.now;
        }
        else if (state.view == planner::View::Gantt && key == 'g') state.gantt_date = state.now;
        else if (state.view == planner::View::Gantt && key == 'f') {
            state.gantt_filter = (state.gantt_filter + 1) % 3;
            state.gantt_row = 0;
        }
        else if (state.view == planner::View::Gantt && key == 'e') {
            if (auto* project = selected_gantt_project(state)) {
                const auto start = prompt("Planned start YYYY-MM-DD (blank keeps current): ", height);
                const auto end = prompt("Planned end YYYY-MM-DD (blank keeps current): ", height);
                planner::Project changed = *project;
                try {
                    if (!start.empty()) { planner::parse_time(start + "T12:00:00"); changed.plan_start = start; }
                    if (!end.empty()) { planner::parse_time(end + "T12:00:00"); changed.plan_end = end; }
                    if (changed.plan_start.empty() || changed.plan_end.empty() || changed.plan_end < changed.plan_start)
                        throw std::invalid_argument("invalid range");
                    store.checkpoint(); store.update_project(changed); refresh_state(state, store, state.now);
                    state.message = "Project planning range updated.";
                } catch (const std::exception&) { state.message = "Invalid range; enter a start and an end on or after it."; }
            } else if (auto* goal = selected_gantt_goal(state)) {
                state.editing_goal_id = goal->id; state.goal_edit_row = 0;
                state.previous_view = planner::View::Gantt; state.view = planner::View::EditGoal;
            }
        }
        else if (state.view == planner::View::Gantt && (key == '\n' || key == KEY_ENTER)) {
            if (auto* project = selected_gantt_project(state)) {
                const int id = project->id;
                const auto found = std::find_if(state.projects.begin(), state.projects.end(),
                    [id](const auto& item) { return item.id == id; });
                state.project_index = found == state.projects.end() ? 0 : static_cast<int>(found - state.projects.begin());
                state.all_projects = false; state.view = planner::View::Board;
            } else if (auto* goal = selected_gantt_goal(state)) {
                const int id = goal->id;
                const auto found = std::find_if(state.goals.begin(), state.goals.end(),
                    [id](const auto& item) { return item.id == id; });
                state.goal_row = found == state.goals.end() ? 0 : static_cast<int>(found - state.goals.begin());
                state.view = planner::View::Goals;
            }
        }
        else if (state.view == planner::View::Gantt && key == KEY_DC) {
            if (auto* project = selected_gantt_project(state)) {
                if (confirm("Clear planning range for '" + project->name + "'?", height)) {
                    planner::Project changed = *project; changed.plan_start.clear(); changed.plan_end.clear();
                    store.checkpoint(); store.update_project(changed); refresh_state(state, store, state.now);
                    state.message = "Project planning range cleared.";
                }
            } else if (selected_gantt_goal(state))
                state.message = "Goal runtime is derived from its goal settings; press e to edit it.";
        }
        else if (state.view == planner::View::Goals && (key == 'k' || key == KEY_UP))
            state.goal_row = std::max(0, state.goal_row - 1);
        else if (state.view == planner::View::Goals && (key == 'j' || key == KEY_DOWN))
            state.goal_row = std::min(std::max(0, static_cast<int>(state.goals.size()) - 1), state.goal_row + 1);
        else if (state.view == planner::View::Goals && key == 'n') {
            const auto title = prompt("Goal name: ", height);
            const auto unit = prompt("Unit (for example pages): ", height);
            const auto target = prompt("Final goal (optional total): ", height);
            const auto daily = prompt("Daily goal: ", height);
            const auto deadline = prompt("Final due date (optional YYYY-MM-DD): ", height);
            try {
                const double amount = target.empty() ? 0 : std::stod(target);
                const double daily_amount = std::stod(daily);
                if (!deadline.empty()) planner::parse_time(deadline + "T12:00:00");
                if (title.empty() || unit.empty() || !std::isfinite(amount) || amount < 0 ||
                    !std::isfinite(daily_amount) || daily_amount <= 0 ||
                    (!deadline.empty() && deadline < planner::date_iso(state.now)))
                    throw std::invalid_argument("invalid goal");
                store.checkpoint();
                store.add_goal(title, unit, amount, planner::date_iso(state.now), deadline, daily_amount);
                refresh_state(state, store, state.now); state.goal_row = static_cast<int>(state.goals.size()) - 1;
                state.message = "Goal created and added to Daily Todos.";
            } catch (const std::exception&) { state.message = "Invalid goal; a positive daily goal is required; final goal and due date are optional."; }
        }
        else if (state.view == planner::View::Goals && key == 'e' && !state.goals.empty()) {
            const auto& goal = state.goals[std::clamp(state.goal_row, 0, static_cast<int>(state.goals.size()) - 1)];
            state.editing_goal_id = goal.id;
            state.goal_edit_row = 0;
            state.previous_view = planner::View::Goals;
            state.view = planner::View::EditGoal;
        }
        else if (state.view == planner::View::EditGoal && (key == 'k' || key == KEY_UP))
            state.goal_edit_row = std::max(0, state.goal_edit_row - 1);
        else if (state.view == planner::View::EditGoal && (key == 'j' || key == KEY_DOWN))
            state.goal_edit_row = std::min(6, state.goal_edit_row + 1);
        else if (state.view == planner::View::EditGoal &&
                 (key == 'e' || key == '\n' || key == KEY_ENTER)) {
            const auto found = std::find_if(state.goals.begin(), state.goals.end(), [&state](const auto& goal) {
                return state.editing_goal_id && goal.id == *state.editing_goal_id;
            });
            if (found == state.goals.end()) state.message = "Goal is unavailable.";
            else {
                planner::Goal changed = *found;
                if (state.goal_edit_row == 6) {
                    const auto value = edit_comment(changed.comment, height, width);
                    if (value) {
                        changed.comment = *value;
                        store.checkpoint(); store.update_goal(changed); refresh_state(state, store, state.now);
                        state.message = "Goal comment updated.";
                    } else state.message = "Comment edit cancelled.";
                    continue;
                }
                std::string value;
                if (state.goal_edit_row == 0) value = prompt("Goal name (blank keeps current): ", height);
                else if (state.goal_edit_row == 1) value = prompt("Unit (blank keeps current): ", height);
                else if (state.goal_edit_row == 2) value = prompt("Final goal (blank keeps, '-' clears): ", height);
                else if (state.goal_edit_row == 3) value = prompt("Daily goal (blank keeps current): ", height);
                else if (state.goal_edit_row == 4) value = prompt("Start date YYYY-MM-DD (blank keeps current): ", height);
                else value = prompt("Final due YYYY-MM-DD (blank keeps, '-' clears): ", height);
                if (!value.empty()) {
                    try {
                        if (state.goal_edit_row == 0) changed.title = value;
                        else if (state.goal_edit_row == 1) changed.unit = value;
                        else if (state.goal_edit_row == 2) changed.target = value == "-" ? 0 : std::stod(value);
                        else if (state.goal_edit_row == 3) changed.daily_goal = std::stod(value);
                        else if (state.goal_edit_row == 4) {
                            planner::parse_time(value + "T12:00:00"); changed.start_date = value;
                        } else {
                            if (value == "-") changed.target_date.clear();
                            else { planner::parse_time(value + "T12:00:00"); changed.target_date = value; }
                        }
                        if (changed.title.empty() || changed.unit.empty() || !std::isfinite(changed.target) ||
                            changed.target < 0 || !std::isfinite(changed.daily_goal) || changed.daily_goal <= 0 ||
                            (!changed.target_date.empty() && changed.target_date < changed.start_date))
                            throw std::invalid_argument("invalid goal");
                        store.checkpoint(); store.update_goal(changed); refresh_state(state, store, state.now);
                        state.message = "Goal field updated.";
                    } catch (const std::exception&) { state.message = "Invalid value; goal was not changed."; }
                }
            }
        }
        else if (state.view == planner::View::Goals && key == '+' && !state.goals.empty()) {
            const auto& goal = state.goals[std::clamp(state.goal_row, 0, static_cast<int>(state.goals.size()) - 1)];
            const auto amount = prompt("Amount to add: ", height);
            const auto note = prompt("Note (optional): ", height);
            try {
                const double value = std::stod(amount);
                if (!std::isfinite(value) || value == 0) throw std::invalid_argument("zero");
                store.checkpoint(); store.add_goal_entry(goal.id, planner::date_iso(state.now), value, note);
                refresh_state(state, store, state.now); state.message = "Goal datapoint recorded.";
            } catch (const std::exception&) { state.message = "Invalid datapoint amount."; }
        }
        else if (state.view == planner::View::Goals && key == KEY_DC && !state.goals.empty()) {
            const auto& goal = state.goals[std::clamp(state.goal_row, 0, static_cast<int>(state.goals.size()) - 1)];
            if (confirm("Delete goal and all datapoints '" + goal.title + "'?", height)) {
                store.checkpoint(); store.delete_goal(goal.id); refresh_state(state, store, state.now);
                state.goal_row = std::min(state.goal_row, std::max(0, static_cast<int>(state.goals.size()) - 1));
                state.message = "Goal deleted.";
            }
        }
        else if (state.view == planner::View::Todos && (key == 'k' || key == KEY_UP)) {
            if (!state.todo_date_focused && state.todo_row == 0) state.todo_date_focused = true;
            else if (!state.todo_date_focused) --state.todo_row;
        }
        else if (state.view == planner::View::Todos && (key == 'j' || key == KEY_DOWN)) {
            if (state.todo_date_focused) state.todo_date_focused = false;
            else if (todo_count(state) > 0)
                state.todo_row = std::min(todo_count(state) - 1, state.todo_row + 1);
        }
        else if (state.view == planner::View::Todos && (key == 'h' || key == KEY_LEFT)) {
            if (state.todo_date_focused) state.todo_date = shift_local_days(state.todo_date, -1);
            else { state.todo_list = std::max(0, state.todo_list - 1); state.todo_row = 0; }
        }
        else if (state.view == planner::View::Todos && (key == 'l' || key == KEY_RIGHT)) {
            if (state.todo_date_focused) state.todo_date = shift_local_days(state.todo_date, 1);
            else { state.todo_list = std::min(2, state.todo_list + 1); state.todo_row = 0; }
        }
        else if (state.view == planner::View::Todos && key == '[') {
            state.todo_date = shift_local_days(state.todo_date, -1); state.todo_row = 0;
        }
        else if (state.view == planner::View::Todos && key == ']') {
            state.todo_date = shift_local_days(state.todo_date, 1); state.todo_row = 0;
        }
        else if (state.view == planner::View::Todos && key == 'g') {
            state.todo_date = state.now; state.todo_row = 0;
        }
        else if (state.view == planner::View::Todos && key == 'n') {
            const auto title = prompt("New daily todo: ", height);
            if (!title.empty()) {
                static const std::array<const char*, 3> lists{"today", "morning", "evening"};
                store.checkpoint(); store.add_todo(title, planner::date_iso(state.todo_date), lists[state.todo_list]);
                refresh_state(state, store, state.now); state.todo_row = std::max(0, stored_todo_count(state) - 1);
                state.message = state.todo_list == 0 ? "One-day todo created." : "Daily recurring todo created.";
            }
        }
        else if (state.view == planner::View::Todos && key == ' ') {
            if (auto* todo = selected_todo(state)) {
                const std::string date = planner::date_iso(state.todo_date);
                const bool completed = !todo_completed_on(state, *todo, date);
                store.checkpoint();
                if (todo->recurring) store.set_todo_completed(todo->id, date, completed);
                else { todo->completed = completed; store.update_todo(*todo); }
                refresh_state(state, store, state.now);
                state.message = completed ? "Todo completed." : "Todo reopened.";
            } else if (auto* task = selected_due_todo_task(state)) {
                const bool completed = task->status != planner::Status::Done;
                store.checkpoint(); store.move_task(task->id,
                    completed ? planner::Status::Done : planner::Status::Ready, 1000000);
                refresh_state(state, store, state.now);
                state.message = completed ? "Due task completed." : "Due task reopened in Ready.";
            } else if (auto* goal = selected_daily_goal(state)) {
                const auto amount = prompt("Goal amount for " + planner::date_iso(state.todo_date) + ": ", height);
                const auto note = prompt("Note (optional): ", height);
                try {
                    const double value = std::stod(amount);
                    if (!std::isfinite(value) || value == 0) throw std::invalid_argument("zero");
                    store.checkpoint(); store.add_goal_entry(goal->id, planner::date_iso(state.todo_date), value, note);
                    refresh_state(state, store, state.now); state.message = "Goal datapoint recorded.";
                } catch (const std::exception&) { state.message = "Invalid datapoint amount."; }
            }
        }
        else if (state.view == planner::View::Todos && key == 'u') {
            if (auto* todo = selected_todo(state)) {
                const std::string date = planner::date_iso(state.todo_date);
                const bool completed = todo_completed_on(state, *todo, date);
                if (completed) {
                    store.checkpoint();
                    if (todo->recurring) store.set_todo_completed(todo->id, date, false);
                    else { todo->completed = false; store.update_todo(*todo); }
                    refresh_state(state, store, state.now); state.message = "Todo unset.";
                } else state.message = "Todo is already unset.";
            } else if (auto* goal = selected_daily_goal(state)) {
                const std::string date = planner::date_iso(state.todo_date);
                const bool has_entries = std::any_of(state.goal_entries.begin(), state.goal_entries.end(),
                    [goal, &date](const auto& entry) { return entry.goal_id == goal->id && entry.date == date; });
                if (has_entries) {
                    store.checkpoint(); store.clear_goal_entries_on(goal->id, date);
                    refresh_state(state, store, state.now); state.message = "Daily goal entries cleared.";
                } else state.message = "Goal is already unset for this date.";
            }
        }
        else if (state.view == planner::View::Todos && key == 'd') {
            if (state.todo_date_focused) state.message = "Move down to select a todo before changing its date.";
            else if (auto* todo = selected_todo(state)) {
                if (todo->recurring)
                    state.message = "Daily recurring todos cannot be moved to a single date.";
                else {
                    state.calendar_date = state.todo_date;
                    state.calendar_rescheduling_task_id.reset();
                    state.calendar_rescheduling_todo_id = todo->id;
                    state.view = planner::View::Calendar;
                }
            } else if (auto* task = selected_due_todo_task(state)) {
                state.calendar_date = state.todo_date;
                state.calendar_rescheduling_todo_id.reset();
                state.calendar_rescheduling_task_id = task->id;
                state.view = planner::View::Calendar;
            } else if (selected_daily_goal(state))
                state.message = "Goal dates are controlled from Edit Goal.";
            else state.message = "Select a todo before changing its date.";
        }
        else if (state.view == planner::View::Todos && key == 'b') {
            if (state.todo_date_focused) state.message = "Move down to select a todo before bumping it.";
            else {
                const std::string target_date = planner::date_iso(shift_local_days(state.todo_date, 1));
                bool moved = false;
                if (auto* todo = selected_todo(state)) {
                    if (todo->recurring)
                        state.message = "Daily recurring todos cannot be bumped to a single date.";
                    else {
                        planner::Todo changed = *todo; changed.date = target_date;
                        store.checkpoint(); store.update_todo(changed); moved = true;
                    }
                } else if (auto* task = selected_due_todo_task(state)) {
                    planner::Task changed = *task; changed.due_date = target_date;
                    store.checkpoint(); store.update_task(changed); moved = true;
                } else if (selected_daily_goal(state))
                    state.message = "Goal dates are controlled from Edit Goal.";
                else state.message = "Select a todo before bumping it.";
                if (moved) {
                    refresh_state(state, store, state.now);
                    state.todo_row = std::min(state.todo_row, std::max(0, todo_count(state) - 1));
                    state.message = "Todo bumped to " + target_date + ".";
                }
            }
        }
        else if (state.view == planner::View::Todos && key == 'e') {
            if (auto* todo = selected_todo(state)) {
                const auto title = prompt("Todo title: ", height);
                if (!title.empty()) {
                    const std::string date = planner::date_iso(state.todo_date);
                    const char scope = todo->recurring ? todo_scope(height, "Rename") : 'a';
                    if (scope == 't' || scope == 'f' || scope == 'a') {
                        store.checkpoint();
                        if (scope == 't') store.rename_todo_on(todo->id, date, title);
                        else if (scope == 'f') store.split_todo_from(todo->id, date, title);
                        else store.rename_todo_all(todo->id, title);
                        refresh_state(state, store, state.now);
                        state.message = scope == 't' ? "Todo renamed for this date." :
                            scope == 'f' ? "Todo renamed from this date onward." : "Todo renamed for all dates.";
                    } else state.message = "Rename cancelled.";
                }
            } else if (auto* task = selected_due_todo_task(state)) {
                const int task_id = task->id;
                reveal_task(state, task_id, false);
                state.previous_view = planner::View::Todos;
                state.edit_row = 0; state.view = planner::View::EditTask;
            } else if (auto* goal = selected_daily_goal(state)) {
                const int id = goal->id;
                const auto found = std::find_if(state.goals.begin(), state.goals.end(),
                    [id](const auto& item) { return item.id == id; });
                state.goal_row = found == state.goals.end() ? 0 : static_cast<int>(found - state.goals.begin());
                state.view = planner::View::Goals;
            }
        }
        else if (state.view == planner::View::Todos && key == KEY_DC) {
            if (auto* todo = selected_todo(state)) {
                const std::string date = planner::date_iso(state.todo_date);
                const char scope = todo->recurring ? todo_scope(height, "Delete") :
                    (confirm("Delete todo '" + todo->title + "'?", height) ? 'a' : '\0');
                if (scope == 't' || scope == 'f' || scope == 'a') {
                    store.checkpoint();
                    if (scope == 't') store.hide_todo_on(todo->id, date);
                    else if (scope == 'f') store.end_todo_before(todo->id, date);
                    else store.delete_todo(todo->id);
                    refresh_state(state, store, state.now);
                    state.todo_row = std::min(state.todo_row, std::max(0, todo_count(state) - 1));
                    state.message = scope == 't' ? "Todo removed for this date." :
                        scope == 'f' ? "Future recurrence removed; history preserved." : "Todo deleted for all dates.";
                } else state.message = "Delete cancelled.";
            } else if (selected_due_todo_task(state))
                state.message = "This is a Kanban task. Press e to edit it or clear its deadline.";
            else if (selected_daily_goal(state))
                state.message = "This is a goal. Press e to open it in Goals.";
        }
        else if (state.view == planner::View::Todos && (key == '\n' || key == KEY_ENTER)) {
            state.calendar_date = state.todo_date;
            state.calendar_selecting_todo = true;
            state.view = planner::View::Calendar;
        }
        else if (state.view == planner::View::Deadlines && (key == 'k' || key == KEY_UP))
            state.deadline_row = std::max(0, state.deadline_row - 1);
        else if (state.view == planner::View::Deadlines && (key == 'j' || key == KEY_DOWN))
            state.deadline_row = std::min(std::max(0, static_cast<int>(deadline_tasks(state).size()) - 1),
                                          state.deadline_row + 1);
        else if (state.view == planner::View::Deadlines &&
                 (key == 'e' || key == '\n' || key == KEY_ENTER)) {
            const auto tasks = deadline_tasks(state);
            if (!tasks.empty()) {
                const int id = tasks[std::clamp(state.deadline_row, 0, static_cast<int>(tasks.size()) - 1)]->id;
                reveal_task(state, id, key != 'e');
                state.previous_view = planner::View::Deadlines;
                if (key == 'e') { state.edit_row = 0; state.view = planner::View::EditTask; }
            }
        }
        else if (state.view == planner::View::Settings &&
                 (key == 'e' || key == '\n' || key == KEY_ENTER)) {
            const auto timezone = prompt("Timezone (for example Europe/London): ", height);
            if (!timezone.empty()) {
                try {
                    store.checkpoint(); store.set_timezone(timezone);
                    refresh_state(state, store, state.now); state.message = "Timezone changed to " + timezone + ".";
                } catch (const std::exception&) {
                    state.message = "Unknown timezone. Use an IANA name such as Europe/London or UTC.";
                }
            }
        }
        else if (state.view == planner::View::Board && key == ' ') {
            if (state.project_focused && state.dragging && state.dragged_task_id && !state.all_projects) {
                const auto* project = planner::selected_project(state);
                if (project) {
                    store.checkpoint(); store.move_task_to_project(*state.dragged_task_id, project->id);
                    state.dragging = false; state.dragged_task_id.reset();
                    refresh_state(state, store, state.now); state.message = "Card transferred and dropped.";
                }
            }
            else if (state.project_focused) state.message = "Press Right to return to Kanban cards.";
            else if (state.dragging) {
                state.dragging = false; state.dragged_task_id.reset(); state.message = "Card dropped.";
            } else if (const auto* task = planner::selected_task(state)) {
                state.dragging = true; state.dragged_task_id = task->id; state.message = "Card picked up; use arrows, then Space to drop.";
            }
        }
        else if (state.view == planner::View::Board && state.dragging &&
                 (key == 'h' || key == KEY_LEFT || key == 'l' || key == KEY_RIGHT ||
                  key == 'j' || key == KEY_DOWN || key == 'k' || key == KEY_UP)) {
            const int task_id = state.dragged_task_id.value_or(0);
            if ((key == 'h' || key == KEY_LEFT) && state.column == 0 && project_panel_visible) {
                const auto task = std::find_if(state.tasks.begin(), state.tasks.end(), [task_id](const auto& item) {
                    return item.id == task_id;
                });
                if (task != state.tasks.end()) {
                    const auto project = std::find_if(state.projects.begin(), state.projects.end(), [task](const auto& item) {
                        return item.id == task->project_id;
                    });
                    if (project != state.projects.end()) state.project_index = static_cast<int>(project - state.projects.begin());
                }
                state.all_projects = false; state.project_focused = true;
                state.message = "Choose a destination project, then Space to transfer and drop.";
                continue;
            }
            int target_column = state.column;
            int target_row = state.row;
            if (key == 'h' || key == KEY_LEFT) { target_column = std::max(0, state.column - 1); target_row = 0; }
            else if (key == 'l' || key == KEY_RIGHT) { target_column = std::min(3, state.column + 1); target_row = 0; }
            else if (key == 'k' || key == KEY_UP) target_row = std::max(0, state.row - 1);
            else target_row = state.row + 1;
            store.checkpoint();
            store.move_task(task_id, static_cast<planner::Status>(target_column), target_row);
            state.column = target_column; state.row = target_row;
            refresh_state(state, store, state.now); normalize(state);
            state.message = "Moving card; press Space to drop.";
        }
        else if (key == 27) {
            if (state.view == planner::View::EditTask) state.view = planner::View::Task;
            else if (state.view == planner::View::TaskChecklist) state.view = planner::View::Task;
            else if (state.view == planner::View::EditGoal) {
                state.view = state.previous_view == planner::View::Gantt ? planner::View::Gantt : planner::View::Goals;
                state.editing_goal_id.reset();
            }
            else if (state.view == planner::View::SessionTaskPicker) state.view = planner::View::EditSession;
            else if (state.view == planner::View::EditSession) { state.view = planner::View::Tracker; state.editing_session_id.reset(); }
            else if (state.view == planner::View::Task) state.view = state.previous_view;
            else { state.previous_view = state.view; state.view = planner::View::Menu; state.today_focused = false; }
        }
        else if (state.view == planner::View::EditTask && (key == 'k' || key == KEY_UP))
            state.edit_row = std::max(0, state.edit_row - 1);
        else if (state.view == planner::View::EditTask && (key == 'j' || key == KEY_DOWN))
            state.edit_row = std::min(5, state.edit_row + 1);
        else if (state.view == planner::View::EditTask && (key == '\n' || key == KEY_ENTER)) {
            if (auto* task = planner::selected_task(state)) {
                bool changed = false;
                try {
                    if (state.edit_row == 0) {
                        const auto value = prompt("Title: ", height);
                        if (!value.empty()) { task->title = value; changed = true; }
                    } else if (state.edit_row == 1) {
                        task->notes = prompt("Notes (blank clears): ", height); changed = true;
                    } else if (state.edit_row == 2) {
                        state.calendar_date = state.now;
                        if (!task->due_date.empty()) {
                            try { state.calendar_date = planner::parse_time(task->due_date + "T12:00:00"); }
                            catch (const std::exception&) {
                                state.message = "Existing deadline is invalid; choose a replacement or press c to clear it.";
                            }
                        }
                        state.calendar_selecting_task_due = true;
                        state.view = planner::View::Calendar;
                    } else if (state.edit_row == 3) {
                        task->tags = prompt("Tags, comma separated (blank clears): ", height); changed = true;
                    } else if (state.edit_row == 4) {
                        const auto value = prompt("Estimate minutes (0 clears): ", height);
                        task->estimate_minutes = std::stoi(value); changed = true;
                    } else {
                        const auto value = prompt("Progress done/target (example 3/8): ", height);
                        const auto slash = value.find('/');
                        if (slash == std::string::npos) throw std::invalid_argument("missing slash");
                        const int done = std::stoi(value.substr(0, slash));
                        const int target = std::stoi(value.substr(slash + 1));
                        if (done < 0 || target < 0)
                            throw std::invalid_argument("negative progress");
                        task->progress_done = done;
                        task->progress_target = target;
                        changed = true;
                    }
                    if (changed) {
                        store.checkpoint(); store.update_task(*task); refresh_state(state, store, state.now);
                        state.message = "Task details updated.";
                    }
                } catch (const std::exception&) {
                    state.message = "Invalid value; task was not changed.";
                }
            }
        }
        else if (state.view == planner::View::Calendar && (key == 'h' || key == KEY_LEFT))
            state.calendar_date = shift_local_days(state.calendar_date, -1);
        else if (state.view == planner::View::Calendar && (key == 'l' || key == KEY_RIGHT))
            state.calendar_date = shift_local_days(state.calendar_date, 1);
        else if (state.view == planner::View::Calendar && (key == 'k' || key == KEY_UP))
            state.calendar_date = shift_local_days(state.calendar_date, -7);
        else if (state.view == planner::View::Calendar && (key == 'j' || key == KEY_DOWN))
            state.calendar_date = shift_local_days(state.calendar_date, 7);
        else if (state.view == planner::View::Calendar && key == 'g') state.calendar_date = state.now;
        else if (state.view == planner::View::TaskChecklist && (key == 'k' || key == KEY_UP))
            state.checklist_row = std::max(0, state.checklist_row - 1);
        else if (state.view == planner::View::TaskChecklist && (key == 'j' || key == KEY_DOWN)) {
            const auto items = selected_task_checklist(state);
            state.checklist_row = items.empty() ? 0 :
                std::min(static_cast<int>(items.size()) - 1, state.checklist_row + 1);
        }
        else if (state.view == planner::View::TaskChecklist && key == 'n') {
            if (const auto* task = planner::selected_task(state)) {
                const std::string title = prompt("New checklist item: ", height);
                if (!title.empty()) {
                    store.checkpoint(); store.add_checklist_item(task->id, title);
                    refresh_state(state, store, state.now);
                    state.checklist_row = std::max(0,
                        static_cast<int>(selected_task_checklist(state).size()) - 1);
                    state.message = "Checklist item added.";
                }
            }
        }
        else if (state.view == planner::View::TaskChecklist && key == ' ') {
            if (auto* item = selected_checklist_item(state)) {
                planner::ChecklistItem changed = *item; changed.completed = !changed.completed;
                store.checkpoint(); store.update_checklist_item(changed);
                refresh_state(state, store, state.now);
                state.message = changed.completed ? "Checklist item completed." : "Checklist item reopened.";
            }
        }
        else if (state.view == planner::View::TaskChecklist && key == 'e') {
            if (auto* item = selected_checklist_item(state)) {
                const std::string title = prompt("Checklist item: ", height);
                if (!title.empty()) {
                    planner::ChecklistItem changed = *item; changed.title = title;
                    store.checkpoint(); store.update_checklist_item(changed);
                    refresh_state(state, store, state.now); state.message = "Checklist item renamed.";
                }
            }
        }
        else if (state.view == planner::View::TaskChecklist && (key == 'J' || key == 'K')) {
            const auto items = selected_task_checklist(state);
            if (!items.empty()) {
                const int target = std::clamp(state.checklist_row + (key == 'J' ? 1 : -1),
                                              0, static_cast<int>(items.size()) - 1);
                if (target != state.checklist_row) {
                    const int id = items[std::clamp(state.checklist_row, 0,
                        static_cast<int>(items.size()) - 1)]->id;
                    store.checkpoint(); store.move_checklist_item(id, target);
                    state.checklist_row = target;
                    refresh_state(state, store, state.now); state.message = "Checklist item reordered.";
                }
            }
        }
        else if (state.view == planner::View::TaskChecklist && key == KEY_DC) {
            if (const auto* item = selected_checklist_item(state)) {
                const int id = item->id;
                if (confirm("Delete checklist item '" + item->title + "'?", height)) {
                    store.checkpoint(); store.delete_checklist_item(id);
                    refresh_state(state, store, state.now);
                    state.checklist_row = std::min(state.checklist_row,
                        std::max(0, static_cast<int>(selected_task_checklist(state).size()) - 1));
                    state.message = "Checklist item deleted.";
                }
            }
        }
        else if (key == 'p' && !project_panel_visible && !state.projects.empty()) {
            if (state.all_projects) { state.all_projects = false; state.project_index = 0; }
            else if (state.project_index + 1 >= static_cast<int>(state.projects.size())) state.all_projects = true;
            else ++state.project_index;
            state.row = 0;
        } else if (key == 'P') {
            const auto name = prompt("New project: ", height);
            if (!name.empty()) { store.checkpoint(); store.add_project(name); refresh_state(state, store, state.now);
                state.project_index = state.projects.size() - 1; state.all_projects = false; }
        } else if ((state.view == planner::View::Board || state.view == planner::View::Task) && (key == 'h' || key == KEY_LEFT)) {
            if (state.view == planner::View::Board && state.column == 0 && project_panel_visible)
                state.project_focused = true;
            else state.column = std::max(0, state.column - 1);
            state.row = 0;
        } else if ((state.view == planner::View::Board || state.view == planner::View::Task) && (key == 'l' || key == KEY_RIGHT)) {
            state.column = std::min(3, state.column + 1); state.row = 0;
        } else if ((state.view == planner::View::Board || state.view == planner::View::Task) && (key == 'k' || key == KEY_UP)) {
            state.row = std::max(0, state.row - 1);
        } else if ((state.view == planner::View::Board || state.view == planner::View::Task) && (key == 'j' || key == KEY_DOWN)) {
            ++state.row; normalize(state);
        } else if (state.view == planner::View::Board && (key == '\n' || key == KEY_ENTER)) {
            if (planner::selected_task(state)) { state.previous_view = planner::View::Board; state.view = planner::View::Task; }
        } else if (state.view == planner::View::Board && state.project_focused && key == 'e') {
            if (state.all_projects) state.message = "ALL PROJECTS is a virtual board and cannot be renamed.";
            else if (auto* project = planner::selected_project(state)) {
                const auto name = prompt("Project name: ", height);
                if (!name.empty()) {
                    project->name = name; store.checkpoint(); store.update_project(*project);
                    refresh_state(state, store, state.now); state.message = "Project renamed.";
                }
            }
        } else if (state.view == planner::View::Board && key == 'n') {
            const auto* project = planner::selected_project(state);
            if (state.all_projects) state.message = "Select a real project before creating a task.";
            else if (project) {
                const auto title = prompt("New task: ", height);
                if (!title.empty()) { store.checkpoint(); store.add_task(project->id, title, static_cast<planner::Status>(state.column)); refresh_state(state, store, state.now); }
            }
        } else if ((state.view == planner::View::Board || state.view == planner::View::Task) && key == 'm') {
            if (auto* task = planner::selected_task(state)) {
                const auto target = planner::next_status(task->status);
                const int position = static_cast<int>(planner::visible_tasks(state, target).size());
                store.checkpoint(); store.move_task(task->id, target, position);
                state.column = static_cast<int>(target); state.row = position;
                refresh_state(state, store, state.now);
            }
        } else if (state.view == planner::View::Task && key == 'c') {
            if (planner::selected_task(state)) {
                state.checklist_row = 0;
                state.view = planner::View::TaskChecklist;
            }
        } else if ((state.view == planner::View::Board || state.view == planner::View::Task) && key == 'e') {
            if (planner::selected_task(state)) {
                if (state.view == planner::View::Board) state.previous_view = planner::View::Board;
                state.edit_row = 0; state.view = planner::View::EditTask;
            }
        } else if ((state.view == planner::View::Board || state.view == planner::View::Task) && key == 'u') {
            if (state.all_projects) state.message = "ALL PROJECTS has no single progress unit.";
            else if (auto* project = planner::selected_project(state)) {
                const auto unit = prompt("Project progress unit (points, pages, applications...): ", height);
                if (!unit.empty()) {
                    project->unit_name = unit; store.checkpoint(); store.update_project(*project);
                    refresh_state(state, store, state.now); state.message = "Project unit updated.";
                }
            }
        } else if ((state.view == planner::View::Board || state.view == planner::View::Task) && key == 'a') {
            if (const auto* task = planner::selected_task(state)) {
                if (state.timer_task_id == task->id || state.paused_task_id == task->id)
                    state.message = "Stop and clear the task timer before archiving it.";
                else if (confirm("Archive '" + task->title + "'?", height)) {
                    const bool from_task = state.view == planner::View::Task;
                    store.checkpoint(); store.archive_task(task->id); refresh_state(state, store, state.now);
                    if (from_task) state.view = state.previous_view;
                    state.message = "Task archived. Tracking history was preserved.";
                }
            }
        } else if ((state.view == planner::View::Board || state.view == planner::View::Task) && key == KEY_DC) {
            if (const auto* task = planner::selected_task(state)) {
                if (state.timer_task_id == task->id || state.paused_task_id == task->id)
                    state.message = "Stop and clear the task timer before deleting it.";
                else if (confirm("Permanently delete '" + task->title + "'?", height)) {
                    const bool from_task = state.view == planner::View::Task;
                    try {
                        store.checkpoint(); store.delete_task(task->id); refresh_state(state, store, state.now);
                        if (from_task) state.view = state.previous_view;
                        state.message = "Task permanently deleted.";
                    } catch (const std::exception& error) { state.message = error.what(); }
                }
            }
        } else if (state.view == planner::View::Task && key == 'z') {
            if (const auto* task = planner::selected_task(state)) {
                std::string date = prompt("Schedule date YYYY-MM-DD (blank for today): ", height);
                if (date.empty()) date = planner::date_iso(state.now);
                const std::string start = prompt("Start time HH:MM: ", height);
                const std::string minutes_text = prompt("Duration in minutes: ", height);
                try {
                    const int minutes = std::stoi(minutes_text);
                    if (start.size() != 5 || start[2] != ':' || date.size() != 10 || minutes <= 0)
                        throw std::invalid_argument("invalid schedule");
                    store.checkpoint(); store.add_schedule(task->id, date, start, minutes);
                    refresh_state(state, store, state.now); state.message = "Task added to schedule.";
                } catch (const std::exception&) {
                    state.message = "Invalid schedule. Use YYYY-MM-DD, HH:MM, and positive minutes.";
                }
            }
        } else if ((state.view == planner::View::Board || state.view == planner::View::Task) && key == 't') {
            if (const auto* task = planner::selected_task(state)) {
                store.checkpoint(); store.start_timer(task->id, state.now); store.clear_paused();
                refresh_state(state, store, state.now); state.message = "Focus timer started.";
            }
        } else if (key == ' ' && state.view != planner::View::Board) {
            if (state.active_allowance_id) {
                store.checkpoint(); const int allowance_id = *state.active_allowance_id;
                store.stop_timer(state.now); store.remember_paused_allowance(allowance_id);
                refresh_state(state, store, state.now);
                state.message = "Allowance timer paused.";
            } else if (state.paused_allowance_id) {
                const int allowance_id = *state.paused_allowance_id;
                store.checkpoint(); store.start_allowance(allowance_id, state.now); store.clear_paused();
                refresh_state(state, store, state.now);
                state.message = "Allowance timer resumed.";
            } else if (state.timer_task_id) {
                store.checkpoint(); const int task_id = *state.timer_task_id; store.stop_timer(state.now);
                store.remember_paused_task(task_id);
                refresh_state(state, store, state.now); state.message = "Focus timer paused.";
            } else if (state.paused_task_id) {
                store.checkpoint(); store.start_timer(*state.paused_task_id, state.now); store.clear_paused();
                refresh_state(state, store, state.now); state.message = "Focus timer resumed.";
            }
        } else if (key == 's' && (state.timer_task_id || state.paused_task_id ||
                                  state.active_allowance_id || state.paused_allowance_id)) {
            store.checkpoint(); if (state.timer_task_id) store.stop_timer(state.now);
            if (state.active_allowance_id) store.stop_timer(state.now);
            store.clear_paused();
            refresh_state(state, store, state.now); state.message = "Focus session saved and cleared.";
        }
        normalize(state);
    }
    endwin();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options config = options(argc, argv);
        const std::string database = config.demo && config.data.empty() ? ":memory:" :
                                     (config.data.empty() ? default_path() : config.data);
        if (database != ":memory:") {
            const auto parent = std::filesystem::path(database).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
        }
        planner::Store store(database);
        store.initialize();
        if (config.demo) store.seed_demo();
        planner::set_app_timezone(store.timezone());
        const planner::TimePoint now = config.fake_now.empty() ? planner::Clock::now() : planner::parse_time(config.fake_now);
        planner::State state = store.load(now);
        if (config.render_view == "calendar") state.view = planner::View::Calendar;
        else if (config.render_view == "todos") state.view = planner::View::Todos;
        else if (config.render_view == "tracker") state.view = planner::View::Tracker;
        else if (config.render_view == "deadlines") state.view = planner::View::Deadlines;
        else if (config.render_view == "allowances") state.view = planner::View::Allowances;
        else if (config.render_view == "goals") state.view = planner::View::Goals;
        else if (config.render_view == "gantt") state.view = planner::View::Gantt;
        else if (config.render_view == "settings") state.view = planner::View::Settings;
        else if (config.render_view == "menu") state.view = planner::View::Menu;
        else if (config.render_view == "help") state.view = planner::View::Help;
        else if (!config.render_view.empty() && config.render_view != "board")
            throw std::runtime_error("Render view must be board, todos, deadlines, calendar, tracker, allowances, goals, gantt, settings, menu, or help");
        if (config.dump_state) {
            std::cout << "projects=" << state.projects.size() << " tasks=" << state.tasks.size()
                      << " timer=" << (state.timer_task_id ? std::to_string(*state.timer_task_id) : "none") << '\n';
            return 0;
        }
        if (config.plain || !config.render_view.empty()) {
            std::cout << planner::render_text(state, config.width, config.height);
            return 0;
        }
        run_tui(state, store, !config.fake_now.empty());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "planner: " << error.what() << '\n';
        return 1;
    }
}
