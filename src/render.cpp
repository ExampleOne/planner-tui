#include "render.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <tuple>

namespace planner {
namespace {

class Canvas {
  public:
    Canvas(int width, int height) : width_(std::max(20, width)), height_(std::max(8, height)),
                                    rows_(height_, std::string(width_, ' ')) {}
    void text(int x, int y, const std::string& value) {
        if (y < 0 || y >= height_ || x >= width_) return;
        int source = x < 0 ? -x : 0;
        x = std::max(0, x);
        for (; source < static_cast<int>(value.size()) && x < width_; ++source, ++x)
            rows_[y][x] = value[source];
    }
    void hline(int x, int y, int length, char value = '-') {
        if (y < 0 || y >= height_) return;
        for (int i = 0; i < length; ++i) if (x + i >= 0 && x + i < width_) rows_[y][x + i] = value;
    }
    void vline(int x, int y, int length, char value = '|') {
        if (x < 0 || x >= width_) return;
        for (int i = 0; i < length; ++i) if (y + i >= 0 && y + i < height_) rows_[y + i][x] = value;
    }
    void box(int x, int y, int width, int height) {
        if (width < 2 || height < 2) return;
        hline(x + 1, y, width - 2); hline(x + 1, y + height - 1, width - 2);
        vline(x, y + 1, height - 2); vline(x + width - 1, y + 1, height - 2);
        text(x, y, "+"); text(x + width - 1, y, "+");
        text(x, y + height - 1, "+"); text(x + width - 1, y + height - 1, "+");
    }
    void blit(int x, int y, const std::vector<std::string>& source) {
        for (int row = 0; row < static_cast<int>(source.size()); ++row)
            text(x, y + row, source[row]);
    }
    std::vector<std::string> take() { return rows_; }
  private:
    int width_, height_;
    std::vector<std::string> rows_;
};

std::string clip(const std::string& value, int width) {
    if (width <= 0) return {};
    if (static_cast<int>(value.size()) <= width) return value;
    if (width <= 3) return value.substr(0, width);
    return value.substr(0, width - 3) + "...";
}

std::vector<std::string> wrap_lines(const std::string& value, int width) {
    std::vector<std::string> lines;
    if (width <= 0) return lines;
    std::string line;
    for (const char ch : value) {
        if (ch == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(ch);
            if (static_cast<int>(line.size()) == width) {
                lines.push_back(line);
                line.clear();
            }
        }
    }
    if (!line.empty() || lines.empty() || (!value.empty() && value.back() == '\n')) lines.push_back(line);
    return lines;
}

bool is_sleep_name(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value.find("sleep") != std::string::npos;
}

const Project* project_by_id(const State& state, int id);

std::string progress(const State& state, const Task& task) {
    if (task.progress_target <= 0 && task.progress_done <= 0) return {};
    const auto* project = project_by_id(state, task.project_id);
    return std::to_string(task.progress_done) + "/" + std::to_string(task.progress_target) + " " +
           (project ? project->unit_name : "points");
}

const Task* task_by_id(const State& state, int id) {
    const auto it = std::find_if(state.tasks.begin(), state.tasks.end(), [id](const Task& task) {
        return task.id == id;
    });
    return it == state.tasks.end() ? nullptr : &*it;
}

std::pair<int, int> checklist_counts(const State& state, int task_id) {
    int completed = 0, total = 0;
    for (const auto& item : state.checklist_items) {
        if (item.task_id != task_id) continue;
        ++total;
        if (item.completed) ++completed;
    }
    return {completed, total};
}

std::string checklist_badge(const State& state, int task_id) {
    const auto [completed, total] = checklist_counts(state, task_id);
    return total == 0 ? std::string{} : "[" + std::to_string(completed) + "/" + std::to_string(total) + "]";
}

const Project* project_by_id(const State& state, int id) {
    const auto it = std::find_if(state.projects.begin(), state.projects.end(), [id](const Project& project) {
        return project.id == id;
    });
    return it == state.projects.end() ? nullptr : &*it;
}

const Allowance* allowance_by_id(const State& state, int id) {
    const auto it = std::find_if(state.allowances.begin(), state.allowances.end(), [id](const Allowance& allowance) {
        return allowance.id == id;
    });
    return it == state.allowances.end() ? nullptr : &*it;
}

long long allowance_used(const State& state, const Allowance& allowance) {
    const std::time_t now_value = Clock::to_time_t(state.now);
    std::tm now_tm{};
    localtime_r(&now_value, &now_tm);
    TimePoint period_start;
    if (allowance.period == "weekly") {
        const int monday_offset = (now_tm.tm_wday + 6) % 7;
        now_tm.tm_hour = now_tm.tm_min = now_tm.tm_sec = 0;
        now_tm.tm_mday -= monday_offset; now_tm.tm_isdst = -1;
        period_start = Clock::from_time_t(mktime(&now_tm));
    } else {
        now_tm.tm_hour = now_tm.tm_min = now_tm.tm_sec = 0;
        now_tm.tm_isdst = -1;
        period_start = Clock::from_time_t(mktime(&now_tm));
    }
    long long seconds = 0;
    for (const auto& session : state.allowance_sessions) {
        if (session.allowance_id != allowance.id || session.started_at < period_start) continue;
        const TimePoint end = session.ended_at.value_or(state.now);
        seconds += std::max<long long>(0,
            std::chrono::duration_cast<std::chrono::seconds>(end - session.started_at).count());
    }
    return seconds;
}

struct TimeStats { long long total{}, today{}, week{}, session{}; };

std::pair<TimePoint, TimePoint> period_starts(TimePoint now) {
    const std::time_t value = Clock::to_time_t(now);
    std::tm tm{}; localtime_r(&value, &tm);
    const int monday_offset = (tm.tm_wday + 6) % 7;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    const TimePoint today = Clock::from_time_t(mktime(&tm));
    tm.tm_mday -= monday_offset; tm.tm_isdst = -1;
    return {today, Clock::from_time_t(mktime(&tm))};
}

std::pair<TimePoint, TimePoint> local_day_bounds(TimePoint point) {
    const std::time_t value = Clock::to_time_t(point);
    std::tm tm{}; localtime_r(&value, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0; tm.tm_isdst = -1;
    const auto start = Clock::from_time_t(mktime(&tm));
    tm.tm_mday += 1; tm.tm_isdst = -1;
    return {start, Clock::from_time_t(mktime(&tm))};
}

long long overlap_seconds(TimePoint start, TimePoint end, TimePoint boundary) {
    start = std::max(start, boundary);
    return std::max<long long>(0, std::chrono::duration_cast<std::chrono::seconds>(end - start).count());
}

TimeStats task_stats(const State& state, std::optional<int> task_id = std::nullopt,
                     std::optional<int> project_id = std::nullopt) {
    const auto [today, week] = period_starts(state.now);
    TimeStats result;
    TimePoint latest{};
    for (const auto& session : state.sessions) {
        const auto* task = task_by_id(state, session.task_id);
        if ((task_id && session.task_id != *task_id) ||
            (project_id && (!task || task->project_id != *project_id))) continue;
        const TimePoint end = session.ended_at.value_or(state.now);
        const long long seconds = overlap_seconds(session.started_at, end, session.started_at);
        result.total += seconds;
        result.today += overlap_seconds(session.started_at, end, today);
        result.week += overlap_seconds(session.started_at, end, week);
        if (session.started_at >= latest) { latest = session.started_at; result.session = seconds; }
    }
    return result;
}

TimeStats allowance_stats(const State& state, int allowance_id) {
    const auto [today, week] = period_starts(state.now);
    TimeStats result;
    TimePoint latest{};
    for (const auto& session : state.allowance_sessions) {
        if (session.allowance_id != allowance_id) continue;
        const TimePoint end = session.ended_at.value_or(state.now);
        const long long seconds = overlap_seconds(session.started_at, end, session.started_at);
        result.total += seconds;
        result.today += overlap_seconds(session.started_at, end, today);
        result.week += overlap_seconds(session.started_at, end, week);
        if (session.started_at >= latest) { latest = session.started_at; result.session = seconds; }
    }
    return result;
}

void frame(Canvas& canvas, int width, int height, const State& state, const std::string& title) {
    canvas.box(0, 0, width, height);
    canvas.text(2, 0, " " + title + " ");
    canvas.text(std::max(2, width - 30), 0, " [M Views] ");
    canvas.text(std::max(2, width - 20), 0, " " + date_human(state.now) + " ");
}

std::string clock_hhmm(TimePoint point, bool seconds = false) {
    const std::time_t value = Clock::to_time_t(point);
    std::tm tm{};
    localtime_r(&value, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, seconds ? "%H:%M:%S" : "%H:%M");
    return out.str();
}

void today_rail(Canvas& canvas, const State& state, int width, int height) {
    canvas.box(0, 0, width, height);
    canvas.text(2, 0, state.today_focused ? " TODAY [ACTIVE] " : " TODAY ");
    canvas.text(2, 2, date_human(state.now));
    canvas.text(std::max(2, width - 11), 2, clock_hhmm(state.now, true));
    canvas.text(2, 4, state.today_focused && state.schedule_row < 0 ? "@ CURRENT ACTIVITY" : "ACTIVITY");

    const Task* focus_task = nullptr;
    std::string activity = "OFF";
    if (state.timer_task_id) {
        focus_task = task_by_id(state, *state.timer_task_id);
        activity = "RUNNING";
    } else if (state.paused_task_id) {
        focus_task = task_by_id(state, *state.paused_task_id);
        activity = "PAUSED";
    } else if (state.active_allowance_id || state.paused_allowance_id) {
        const int id = state.active_allowance_id.value_or(*state.paused_allowance_id);
        const auto* allowance = allowance_by_id(state, id);
        activity = state.active_allowance_id ? "ALLOWANCE RUNNING" : "ALLOWANCE PAUSED";
        canvas.text(2, 5, std::string(state.active_allowance_id ? "* " : "  ") + activity);
        if (allowance) {
            const long long used = allowance_used(state, *allowance);
            canvas.text(2, 6, clip(allowance->name, width - 4));
            canvas.text(2, 7, duration(used) + " / " + std::to_string(allowance->budget_minutes) + "m");
            const auto stats = allowance_stats(state, allowance->id);
            canvas.text(2, 8, "Total " + duration(stats.total) + "  Today " + duration(stats.today));
            canvas.text(2, 9, "Week  " + duration(stats.week) + "  Session " + duration(stats.session));
        }
    }
    if (!state.active_allowance_id && !state.paused_allowance_id)
        canvas.text(2, 5, std::string(state.timer_task_id ? "* " : "  ") + activity);
    if (focus_task) {
        const auto* project = project_by_id(state, focus_task->project_id);
        canvas.text(2, 6, clip(focus_task->title, width - 4));
        canvas.text(2, 7, clip(project ? project->name : "Unknown project", width - 4));
        const auto task_time = task_stats(state, focus_task->id);
        const auto project_time = task_stats(state, std::nullopt, focus_task->project_id);
        canvas.text(2, 8, "TASK     total   " + duration(task_time.total));
        canvas.text(4, 9, "today " + duration(task_time.today) + "  week " + duration(task_time.week));
        canvas.text(4, 10, "session " + duration(task_time.session));
        canvas.text(2, 11, "PROJECT  total   " + duration(project_time.total));
        canvas.text(4, 12, "today " + duration(project_time.today) + "  week " + duration(project_time.week));
        canvas.text(4, 13, "session " + duration(project_time.session));
    } else if (!state.active_allowance_id && !state.paused_allowance_id) {
        canvas.text(2, 6, "No focus task");
        canvas.text(2, 7, "Select a task and press t");
    }

    canvas.hline(1, 15, width - 2);
    canvas.text(2, 15, " DAILY CALENDAR ");
    const std::string today = date_iso(state.now);
    const std::string now = clock_hhmm(state.now);
    int y = 17;
    int index = 0;
    bool now_drawn = false;
    for (const auto& block : state.schedule) {
        if (block.date != today || y >= height - 11) continue;
        if (!now_drawn && block.start_time > now && y < height - 11) {
            canvas.text(2, y++, "------ " + now + " NOW ------");
            now_drawn = true;
        }
        const auto* task = task_by_id(state, block.task_id);
        const bool selected = state.today_focused && state.schedule_row == index;
        canvas.text(2, y++, std::string(selected ? "> " : "  ") + block.start_time + "  " +
                              clip(task ? task->title : "Unknown task", width - 12));
        if (y < height - 11)
            canvas.text(4, y++, std::to_string(block.duration_minutes) + "m" +
                                  (task && state.timer_task_id == task->id ? "  * active" : ""));
        ++index;
    }
    if (!now_drawn && y < height - 11) canvas.text(2, y++, "------ " + now + " NOW ------");
    if (index == 0 && y < height - 11) canvas.text(2, y++, "Nothing scheduled today.");

    if (y < height - 11) {
        canvas.hline(1, y, width - 2);
        canvas.text(2, y++, " DUE TODAY ");
        int due_count = 0;
        for (const auto& task : state.tasks) {
            if (task.archived || task.due_date != today || y >= height - 10) continue;
            canvas.text(2, y++, std::string(task.status == Status::Done ? "x " : "! ") + clip(task.title, width - 6));
            ++due_count;
        }
        if (due_count == 0 && y < height - 10) canvas.text(2, y++, "Nothing due today.");
    }
    if (y < height - 7) {
        canvas.hline(1, y, width - 2);
        canvas.text(2, y++, " OVERDUE ");
        int overdue_count = 0;
        for (const auto& task : state.tasks) {
            if (task.archived || task.status == Status::Done || task.due_date.empty() ||
                task.due_date >= today || y >= height - 6) continue;
            canvas.text(2, y++, "! " + clip(task.title, width - 6));
            ++overdue_count;
        }
        if (overdue_count == 0 && y < height - 6) canvas.text(2, y++, "Nothing overdue.");
    }
    if (y < height - 3) {
        canvas.hline(1, y, width - 2);
        canvas.text(2, y++, " UPCOMING ");
        std::vector<const Task*> upcoming;
        for (const auto& task : state.tasks)
            if (!task.archived && task.status != Status::Done && task.due_date > today)
                upcoming.push_back(&task);
        std::sort(upcoming.begin(), upcoming.end(), [](const Task* a, const Task* b) {
            return a->due_date != b->due_date ? a->due_date < b->due_date : a->title < b->title;
        });
        int shown = 0;
        for (const auto* task : upcoming) {
            if (y >= height - 2 || shown == 3) break;
            canvas.text(2, y++, "+ " + task->due_date.substr(5) + " " + clip(task->title, width - 13));
            ++shown;
        }
        if (shown == 0 && y < height - 2) canvas.text(2, y, "Nothing upcoming.");
    }
    canvas.text(2, height - 2, state.today_focused ? "j/k select  t start  s stop  Right projects" :
                                                    "Left from Projects or Tab: focus");
}

void focus_panel(Canvas& canvas, const State& state, int width, int height) {
    const int top = height - 6;
    canvas.hline(1, top, width - 2);
    const Task* focus_task = nullptr;
    std::string status = "STOPPED";
    if (state.active_allowance_id || state.paused_allowance_id) {
        const int id = state.active_allowance_id.value_or(*state.paused_allowance_id);
        const auto* allowance = allowance_by_id(state, id);
        const std::string mode = state.active_allowance_id ? "RUNNING" : "PAUSED";
        canvas.text(2, top + 1, "ALLOWANCE  " + (allowance ? allowance->name : "Unknown"));
        if (allowance) { const auto stats = allowance_stats(state, allowance->id);
            canvas.text(2, top + 2, "TIME       total " + duration(stats.total) + "  today " + duration(stats.today) +
                "  week " + duration(stats.week) + "  session " + duration(stats.session) + "  [" + mode + "]"); }
        canvas.text(2, top + 3, state.active_allowance_id ? "Space pause  s stop" : "Space resume  s clear");
        return;
    }
    if (state.timer_task_id) {
        focus_task = task_by_id(state, *state.timer_task_id);
        status = "RUNNING";
    } else if (state.paused_task_id) {
        focus_task = task_by_id(state, *state.paused_task_id);
        status = "PAUSED";
    }
    const Project* focus_project = focus_task ? project_by_id(state, focus_task->project_id) : selected_project(state);
    canvas.text(2, top, " FOCUS ");
    canvas.text(2, top + 1, "PROJECT  " + clip(focus_project ? focus_project->name : "No project", width - 12));
    if (focus_task) {
        const auto stats = task_stats(state, focus_task->id);
        const std::string controls = status == "RUNNING" ? "Space pause  s stop" : "Space resume  s clear";
        const int reserved = static_cast<int>(status.size() + controls.size()) + 25;
        canvas.text(2, top + 2, "TASK     " + clip(focus_task->title, std::max(8, width - reserved)));
        const std::string right = "total " + duration(stats.total) + " today " + duration(stats.today) +
            " week " + duration(stats.week) + " session " + duration(stats.session) + "  [" + status + "]  " + controls;
        canvas.text(std::max(2, width - static_cast<int>(right.size()) - 2), top + 2, right);
    } else {
        canvas.text(2, top + 2, "TASK     No focus task                    Select a task and press t");
    }
    if (!state.message.empty()) canvas.text(2, top + 3, clip(state.message, width - 4));
    else canvas.text(2, top + 3, status == "PAUSED" ? "Timer is paused; elapsed time is not increasing." :
                                               "Only one focus timer can run at a time.");
}

void board(Canvas& canvas, const State& state, int width, int height, bool show_focus = true) {
    frame(canvas, width, height, state, "Planner / Board");
    const int footer = height - 2;
    const int panel = show_focus ? height - 6 : height - 2;
    const bool compact = width < 72;
    const int sidebar = compact ? 0 : std::min(20, width / 5);
    if (!compact) {
        canvas.vline(sidebar, 1, panel - 1);
        canvas.text(2, 2, state.project_focused ? "@ PROJECTS" : "PROJECTS");
        canvas.text(2, 4, state.all_projects ? (state.project_focused ? "@ ALL PROJECTS" : "> ALL PROJECTS") :
                                               "  ALL PROJECTS");
        for (int i = 0; i < static_cast<int>(state.projects.size()) && 5 + i < panel; ++i) {
            const std::string marker = !state.all_projects && i == state.project_index ?
                                       (state.project_focused ? "@ " : "> ") : "  ";
            canvas.text(2, 5 + i, marker +
                        clip(state.projects[i].name, sidebar - 4));
        }
        canvas.text(2, std::min(panel - 1, 6 + static_cast<int>(state.projects.size())), "P new project");
    }
    const int start_x = sidebar + 1;
    const int board_width = width - start_x - 1;
    const Project* project = selected_project(state);
    const std::string project_title = state.all_projects ? "ALL PROJECTS" :
        (project ? project->name : "No projects - press P to create one");
    const auto stats = task_stats(state, std::nullopt,
        state.all_projects || !project ? std::nullopt : std::optional<int>{project->id});
    canvas.text(start_x + 2, 2, clip(project_title + "  | total " + duration(stats.total), board_width - 4));
    canvas.text(start_x + 2, 3, clip("today " + duration(stats.today) + "  week " + duration(stats.week) +
        "  session " + duration(stats.session), board_width - 4));

    if (compact) {
        const Status status = static_cast<Status>(std::clamp(state.column, 0, 3));
        canvas.text(2, 4, status_name(status) + "  [column " + std::to_string(state.column + 1) + " of 4]");
        const auto tasks = visible_tasks(state, status);
        const int capacity = std::max(1, (panel - 5) / 2);
        int first = std::clamp(state.board_scroll[state.column], 0,
                               std::max(0, static_cast<int>(tasks.size()) - capacity));
        if (state.row < first) first = state.row;
        else if (state.row >= first + capacity) first = state.row - capacity + 1;
        for (int i = first; i < static_cast<int>(tasks.size()) && i < first + capacity; ++i) {
            const int y = 6 + (i - first) * 2;
            const std::string marker = state.dragged_task_id == tasks[i]->id ? "@ " : (i == state.row ? "> " : "  ");
            canvas.text(2, y, marker + clip(tasks[i]->title, width - 6));
            const auto* source = project_by_id(state, tasks[i]->project_id);
            std::string detail = state.all_projects ? (source ? source->name : "Unknown project") :
                (!progress(state, *tasks[i]).empty() ? progress(state, *tasks[i]) :
                 (!tasks[i]->due_date.empty() ? "due " + tasks[i]->due_date : ""));
            const std::string checklist = checklist_badge(state, tasks[i]->id);
            if (!checklist.empty()) detail += (detail.empty() ? "" : "  ") + checklist;
            if (!detail.empty()) canvas.text(4, y + 1, clip(detail, width - 6));
        }
        if (first > 0) canvas.text(width - 3, 4, "^");
        if (first + capacity < static_cast<int>(tasks.size())) canvas.text(width - 3, panel - 1, "v");
    } else {
        const int column_width = std::max(12, board_width / 4);
        for (int column = 0; column < 4; ++column) {
            const int x = start_x + column * column_width;
            const bool selected_column = state.column == column && !state.project_focused;
            canvas.text(x + 1, 4, std::string(selected_column ? "> " : "  ") +
                        status_name(static_cast<Status>(column)));
            const auto tasks = visible_tasks(state, static_cast<Status>(column));
            const int capacity = std::max(1, (panel - 6) / 3);
            int first = std::clamp(state.board_scroll[column], 0,
                                   std::max(0, static_cast<int>(tasks.size()) - capacity));
            if (selected_column) {
                if (state.row < first) first = state.row;
                else if (state.row >= first + capacity) first = state.row - capacity + 1;
            }
            for (int i = first; i < static_cast<int>(tasks.size()) && i < first + capacity; ++i) {
                const int y = 6 + (i - first) * 3;
                const int card_width = std::min(column_width - 1, width - x - 1);
                canvas.box(x, y, card_width, 3);
                const std::string marker = state.dragged_task_id == tasks[i]->id ? "@" :
                                           (selected_column && state.row == i ? ">" : " ");
                std::string task_progress = progress(state, *tasks[i]);
                if (state.all_projects) {
                    const auto* source = project_by_id(state, tasks[i]->project_id);
                    task_progress = source ? "[" + clip(source->name, 6) + "]" : "[?]";
                }
                const std::string checklist = checklist_badge(state, tasks[i]->id);
                if (!checklist.empty()) task_progress += (task_progress.empty() ? "" : " ") + checklist;
                const int title_width = card_width - 3 - (task_progress.empty() ? 0 : static_cast<int>(task_progress.size()) + 1);
                canvas.text(x + 1, y + 1, marker + clip(tasks[i]->title, std::max(4, title_width)));
                if (!task_progress.empty())
                    canvas.text(x + card_width - static_cast<int>(task_progress.size()) - 1, y + 1, task_progress);
            }
            if (first > 0) canvas.text(x + column_width - 2, 4, "^");
            if (first + capacity < static_cast<int>(tasks.size()))
                canvas.text(x + column_width - 2, panel - 1, "v");
        }
    }
    if (show_focus) focus_panel(canvas, state, width, height);
    canvas.text(2, footer, compact ? "h/l column  j/k task  p projects  ? help" :
        (state.project_focused ? (state.dragging ? "Up/Down destination  Space transfer+drop  Right transfer+cards" :
         "Left agenda  Up/Down project  e rename  u unit  Right cards") :
        (state.dragging ? "ARROWS move card  Space drop" :
        "Arrows navigate panels/cards  Space pick up  e edit  n task  M Views")));
}

void calendar(Canvas& canvas, const State& state, int width, int height, bool show_focus = true) {
    frame(canvas, width, height, state,
          state.calendar_selecting_tracker ? "Planner / Select Tracker Date" :
          (state.calendar_selecting_task_due ? "Planner / Select Task Deadline" :
          (state.calendar_rescheduling_todo_id || state.calendar_rescheduling_task_id ?
           "Planner / Reschedule Todo" :
          (state.calendar_selecting_todo ? "Planner / Select Todo Date" :
          (state.calendar_selecting_session_date == 1 ? "Planner / Select Interval Start Date" :
          (state.calendar_selecting_session_date == 2 ? "Planner / Select Interval End Date" : "Planner / Calendar"))))));
    std::time_t value = Clock::to_time_t(state.calendar_date);
    std::tm current{};
    localtime_r(&value, &current);
    std::tm first = current;
    first.tm_mday = 1;
    first.tm_isdst = -1;
    mktime(&first);
    const int days = [] (int year, int month) {
        static constexpr std::array<int, 12> values{31,28,31,30,31,30,31,31,30,31,30,31};
        if (month != 1) return values[month];
        return ((year + 1900) % 4 == 0 && ((year + 1900) % 100 != 0 || (year + 1900) % 400 == 0)) ? 29 : 28;
    }(current.tm_year, current.tm_mon);
    std::ostringstream month;
    month << std::put_time(&current, "%B %Y");
    canvas.text(std::max(2, width / 2 - static_cast<int>(month.str().size()) / 2), 2, month.str());
    const int cell = std::max(5, (width - 4) / 7);
    static const std::array<const char*, 7> names{"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
    for (int i = 0; i < 7; ++i) canvas.text(2 + i * cell, 4, names[i]);
    const int monday_offset = (first.tm_wday + 6) % 7;
    const int week_count = (monday_offset + days + 6) / 7;
    const int week_stride = height < 27 ? 1 : 2;
    for (int day = 1; day <= days; ++day) {
        const int position = monday_offset + day - 1;
        const int x = 2 + (position % 7) * cell;
        const int y = 6 + (position / 7) * week_stride;
        std::ostringstream number;
        number << (day == current.tm_mday ? "[" : " ") << std::setw(2) << day
               << (day == current.tm_mday ? "]" : " ");
        canvas.text(x, y, number.str());
        std::ostringstream iso;
        iso << std::put_time(&current, "%Y-%m-") << std::setfill('0') << std::setw(2) << day;
        int count = 0;
        for (const auto& task : state.tasks) if (task.due_date == iso.str()) ++count;
        for (const auto& block : state.schedule) if (block.date == iso.str()) ++count;
        if (count) canvas.text(x + 5, y, std::to_string(count) + " items");
    }
    const int panel = show_focus ? height - 6 : height - 2;
    const int agenda = std::min(panel - 2, 8 + (week_count - 1) * week_stride);
    canvas.hline(1, agenda, width - 2);
    const std::string selected_date = date_iso(state.calendar_date);
    canvas.text(2, agenda, " " + selected_date + " schedule ");
    int row = agenda + 2;
    for (const auto& block : state.schedule) {
        if (block.date != selected_date || row >= panel) continue;
        const auto* task = task_by_id(state, block.task_id);
        const std::string title = task ? task->title : "Unknown task";
        canvas.text(3, row++, block.start_time + "  " + clip(title, width - 24) + "  " +
                              std::to_string(block.duration_minutes) + "m");
    }
    for (const auto& task : state.tasks) {
        if (task.due_date == selected_date && row < panel)
            canvas.text(3, row++, "Due  " + clip(task.title, width - 10));
    }
    if (row == agenda + 2) canvas.text(3, row, "Nothing due today.");
    if (show_focus) focus_panel(canvas, state, width, height);
    canvas.text(2, height - 2, state.calendar_selecting_tracker ?
        "h/l day  j/k week  g today  Enter choose  Esc cancel" :
        (state.calendar_selecting_task_due ?
        "h/l day  j/k week  g today  Enter set deadline  c clear  Esc cancel" :
        (state.calendar_rescheduling_todo_id || state.calendar_rescheduling_task_id ?
        "h/l day  j/k week  g today  Enter move todo  Esc cancel" :
        (state.calendar_selecting_todo ?
        "h/l day  j/k week  g today  Enter choose  Esc cancel" :
        (state.calendar_selecting_session_date ?
        "h/l day  j/k week  g today  Enter choose date  Esc cancel" :
        "h/l day  j/k week  g today  M Views  Space pause/resume  q quit")))));
}

void task_detail(Canvas& canvas, const State& state, int width, int height, bool show_focus = true) {
    frame(canvas, width, height, state, "Planner / Task");
    const Task* task = selected_task(state);
    if (!task) {
        canvas.text(3, 3, "No task selected.");
    } else {
        canvas.text(3, 3, clip(task->title, width - 6));
        canvas.text(std::max(3, width - 14), 3, status_name(task->status));
        canvas.hline(2, 4, width - 4);
        const auto* project = project_by_id(state, task->project_id);
        canvas.text(3, 6, "Due       " + (task->due_date.empty() ? std::string("Not set") : task->due_date));
        canvas.text(3, 8, "Tags      " + (task->tags.empty() ? std::string("None") : task->tags));
        canvas.text(3, 10, "Estimate  " + (task->estimate_minutes > 0 ?
            std::to_string(task->estimate_minutes) + " minutes" : std::string("Not set")));
        canvas.text(3, 12, "Progress  " + std::to_string(task->progress_done) + "/" +
                                  std::to_string(task->progress_target) + " " +
                                  (project ? project->unit_name : "points"));
        const auto stats = task_stats(state, task->id);
        canvas.text(3, 14, "Time      total " + duration(stats.total) + "   today " + duration(stats.today));
        canvas.text(3, 15, "          week  " + duration(stats.week) + "   session " + duration(stats.session));
        const auto [checked, checklist_total] = checklist_counts(state, task->id);
        canvas.text(3, 17, "Checklist " + std::to_string(checked) + "/" + std::to_string(checklist_total) +
                           " complete  (c open)");
        canvas.text(3, 19, "Notes  " + clip(task->notes.empty() ? "No notes yet." : task->notes, width - 13));
    }
    if (show_focus) focus_panel(canvas, state, width, height);
    canvas.text(2, height - 2, "c checklist  e details  z schedule  a archive  Delete remove  t timer  Esc return");
}

void task_checklist(Canvas& canvas, const State& state, int width, int height) {
    frame(canvas, width, height, state, "Planner / Task Checklist");
    const Task* task = selected_task(state);
    if (!task) {
        canvas.text(3, 3, "No task selected.");
        canvas.text(2, height - 2, "Esc return");
        return;
    }
    const auto [completed, total] = checklist_counts(state, task->id);
    canvas.text(3, 2, "TASK CHECKLIST");
    canvas.text(3, 3, clip(task->title, width - 28));
    const std::string summary = std::to_string(completed) + "/" + std::to_string(total) + " complete";
    canvas.text(std::max(3, width - static_cast<int>(summary.size()) - 3), 3, summary);
    canvas.hline(2, 4, width - 4);

    std::vector<const ChecklistItem*> items;
    for (const auto& item : state.checklist_items)
        if (item.task_id == task->id) items.push_back(&item);
    const int capacity = std::max(1, height - 8);
    const int selected = items.empty() ? 0 :
        std::clamp(state.checklist_row, 0, static_cast<int>(items.size()) - 1);
    const int first = std::clamp(selected - capacity + 1, 0,
        std::max(0, static_cast<int>(items.size()) - capacity));
    int y = 6;
    for (int i = first; i < static_cast<int>(items.size()) && i < first + capacity; ++i) {
        canvas.text(3, y++, std::string(i == selected ? "> " : "  ") +
            (items[i]->completed ? "[x] " : "[ ] ") + clip(items[i]->title, width - 11));
    }
    if (items.empty()) canvas.text(3, 6, "No checklist items yet. Press n to add one.");
    if (first > 0) canvas.text(width - 3, 5, "^");
    if (first + capacity < static_cast<int>(items.size())) canvas.text(width - 3, height - 3, "v");
    canvas.text(2, height - 2,
        "j/k select  n new  Space check  e rename  J/K reorder  Delete remove  Esc task");
}

void edit_task(Canvas& canvas, const State& state, int width, int height) {
    frame(canvas, width, height, state, "Planner / Edit Task");
    const Task* task = selected_task(state);
    if (!task) { canvas.text(3, 3, "No task selected."); return; }
    const auto* project = project_by_id(state, task->project_id);
    const auto stats = task_stats(state, task->id);
    canvas.text(3, 2, "EDIT TASK DETAILS");
    canvas.text(3, 3, "Time  total " + duration(stats.total) + "  today " + duration(stats.today));
    canvas.text(3, 4, "      week  " + duration(stats.week) + "  session " + duration(stats.session));
    const std::array<std::pair<std::string, std::string>, 6> fields{{
        {"Title", task->title}, {"Notes", task->notes.empty() ? "No notes" : task->notes},
        {"Due date", task->due_date.empty() ? "Not set" : task->due_date},
        {"Tags", task->tags.empty() ? "None" : task->tags},
        {"Estimate", task->estimate_minutes ? std::to_string(task->estimate_minutes) + " minutes" : "Not set"},
        {"Progress", std::to_string(task->progress_done) + "/" + std::to_string(task->progress_target) + " " +
                     (project ? project->unit_name : "points")}}};
    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
        const int y = 5 + i * 3;
        canvas.text(3, y, std::string(i == state.edit_row ? "> " : "  ") + fields[i].first);
        canvas.text(7, y + 1, clip(fields[i].second, width - 11));
    }
    canvas.text(2, height - 2, "j/k field  Enter edit  Ctrl+Z undo  Ctrl+Y redo  Esc return");
}

void tracker(Canvas& canvas, const State& state, int width, int height, bool show_focus = true) {
    frame(canvas, width, height, state, "Planner / Tracker");
    const std::string today = date_iso(state.tracker_date);
    canvas.text(3, 2, std::string(state.tracker_row < 0 ? "> " : "  ") + "TRACKED DATE  " + today);
    canvas.text(3, 3, "ID    TIME           DURATION    PROJECT / TASK");
    canvas.hline(2, 4, width - 4);
    const auto [day_start, day_end] = local_day_bounds(state.tracker_date);
    const int bar_x = 3, bar_width = std::max(8, width - 6);
    const int bar_y = show_focus ? height - 8 : height - 4;
    const int limit = std::max(6, bar_y - 5);
    const auto day_seconds = std::max<long long>(1,
        std::chrono::duration_cast<std::chrono::seconds>(day_end - day_start).count());
    auto draw_segment = [&](TimePoint raw_start, TimePoint raw_end, char marker) {
        if (raw_start >= day_end || raw_end <= day_start) return;
        const auto start = std::max(raw_start, day_start), end = std::min(raw_end, day_end);
        const auto from = std::chrono::duration_cast<std::chrono::seconds>(start - day_start).count();
        const auto to = std::chrono::duration_cast<std::chrono::seconds>(end - day_start).count();
        const int x1 = std::clamp(static_cast<int>(from * bar_width / day_seconds), 0, bar_width - 1);
        const int x2 = std::clamp(static_cast<int>((to * bar_width + day_seconds - 1) / day_seconds), x1 + 1, bar_width);
        canvas.text(bar_x + x1, bar_y, std::string(x2 - x1, marker));
    };
    int y = 5;
    int index = 0;
    long long total = 0;
    for (const auto& session : state.sessions) {
        const TimePoint actual_end = session.ended_at.value_or(state.now);
        if (session.started_at >= day_end || actual_end <= day_start) continue;
        const auto* task = task_by_id(state, session.task_id);
        const auto* project = task ? project_by_id(state, task->project_id) : nullptr;
        const TimePoint start = std::max(session.started_at, day_start);
        const TimePoint end = std::min(actual_end, day_end);
        const long long seconds = std::max<long long>(0,
            std::chrono::duration_cast<std::chrono::seconds>(end - start).count());
        total += seconds;
        if (y >= limit) { ++index; continue; }
        const bool live_now = !session.ended_at && end == state.now;
        const std::string times = clock_hhmm(start) + "-" + (live_now ? "NOW  " : clock_hhmm(end));
        std::ostringstream line;
        line << (index == state.tracker_row ? "> " : "  ") << std::setw(4) << session.id << "  "
             << times << "  " << duration(seconds) << "  "
             << (project ? project->name : "Unknown") << " / " << (task ? task->title : "Unknown task");
        canvas.text(2, y++, clip(line.str(), width - 4));
        ++index;
    }
    for (const auto& session : state.allowance_sessions) {
        const TimePoint actual_end = session.ended_at.value_or(state.now);
        if (session.started_at >= day_end || actual_end <= day_start) continue;
        const auto* allowance = allowance_by_id(state, session.allowance_id);
        const TimePoint start = std::max(session.started_at, day_start);
        const TimePoint end = std::min(actual_end, day_end);
        const long long seconds = std::max<long long>(0,
            std::chrono::duration_cast<std::chrono::seconds>(end - start).count());
        total += seconds;
        if (y >= limit) { ++index; continue; }
        const bool live_now = !session.ended_at && end == state.now;
        const std::string times = clock_hhmm(start) + "-" + (live_now ? "NOW  " : clock_hhmm(end));
        std::ostringstream line;
        line << "  A" << std::setw(3) << session.id << "  " << times << "  " << duration(seconds)
             << "  Allowance / " << (allowance ? allowance->name : "Unknown");
        canvas.text(2, y++, clip(line.str(), width - 4));
        ++index;
    }
    if (index == 0) canvas.text(3, 5, "No time overlaps this day.");
    canvas.hline(2, std::min(y + 1, bar_y - 4), width - 4);
    canvas.text(3, std::min(y + 2, bar_y - 3), "TOTAL  " + duration(total));

    int legend_x = 3;
    std::vector<int> seen_tasks, seen_allowances;
    auto legend = [&](int id, bool allowance, const std::string& name) {
        auto& seen = allowance ? seen_allowances : seen_tasks;
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) return;
        seen.push_back(id);
        const std::string label = "[] " + clip(name, 14);
        if (legend_x + static_cast<int>(label.size()) >= width - 2) return;
        canvas.text(legend_x, bar_y - 2, label);
        legend_x += static_cast<int>(label.size()) + 2;
    };
    for (const auto& session : state.sessions) {
        const auto end = session.ended_at.value_or(state.now);
        if (session.started_at >= day_end || end <= day_start) continue;
        const auto* task = task_by_id(state, session.task_id);
        legend(session.task_id, false, task ? task->title : "Unknown task");
    }
    for (const auto& session : state.allowance_sessions) {
        const auto end = session.ended_at.value_or(state.now);
        if (session.started_at >= day_end || end <= day_start) continue;
        const auto* allowance = allowance_by_id(state, session.allowance_id);
        legend(session.allowance_id, true, allowance ? allowance->name : "Allowance");
    }
    canvas.text(bar_x, bar_y - 1, "00:00");
    canvas.text(bar_x + bar_width / 4, bar_y - 1, "06:00");
    canvas.text(bar_x + bar_width / 2, bar_y - 1, "12:00");
    canvas.text(bar_x + bar_width * 3 / 4, bar_y - 1, "18:00");
    canvas.text(std::max(bar_x, bar_x + bar_width - 5), bar_y - 1, "24:00");
    canvas.text(bar_x, bar_y, std::string(bar_width, '.'));
    for (const auto& session : state.sessions)
        draw_segment(session.started_at, session.ended_at.value_or(state.now),
                     ([&] { const auto* task = task_by_id(state, session.task_id);
                            return task && is_sleep_name(task->title) ? 'z' : '='; })());
    for (const auto& session : state.allowance_sessions)
        draw_segment(session.started_at, session.ended_at.value_or(state.now), '~');
    if (show_focus) focus_panel(canvas, state, width, height);
    canvas.text(2, height - 2, state.tracker_row < 0 ?
        "h/l or Left/Right day  Enter calendar  g today  Down intervals  M Views" :
        "j/k select (Up to date)  e edit interval  n manual interval  M Views  q quit");
}

void edit_session(Canvas& canvas, const State& state, int width, int height) {
    frame(canvas, width, height, state, "Planner / Edit Tracked Interval");
    const auto found = std::find_if(state.sessions.begin(), state.sessions.end(), [&state](const auto& session) {
        return state.editing_session_id && session.id == *state.editing_session_id;
    });
    if (found == state.sessions.end()) { canvas.text(3, 3, "Tracked interval not found."); return; }
    const auto* task = task_by_id(state, found->task_id);
    const auto* project = task ? project_by_id(state, task->project_id) : nullptr;
    canvas.text(3, 2, "EDIT TRACKED INTERVAL  #" + std::to_string(found->id));
    canvas.hline(2, 4, width - 4);
    const std::array<std::pair<std::string, std::string>, 5> fields{{
        {"Task", (project ? project->name + " / " : std::string{}) + (task ? task->title : "Unknown task")},
        {"Start date", date_iso(found->started_at)},
        {"Start time", clock_hhmm(found->started_at, true)},
        {"End date", found->ended_at ? date_iso(*found->ended_at) : "RUNNING"},
        {"End time", found->ended_at ? clock_hhmm(*found->ended_at, true) : "RUNNING"}
    }};
    for (int i = 0; i < 5; ++i) {
        const int y = 6 + i * 2;
        canvas.text(3, y, std::string(i == state.session_edit_row ? "> " : "  ") +
                    fields[i].first + "  " + clip(fields[i].second, width - 20));
    }
    canvas.text(3, 17, "Dates open the calendar; times accept HH:MM. Blank time keeps its value.");
    canvas.text(2, height - 2, "j/k field  e/Enter change  Esc tracker  Ctrl+Z undo  Ctrl+Y redo");
}

void session_task_picker(Canvas& canvas, const State& state, int width, int height) {
    frame(canvas, width, height, state, "Planner / Choose Task");
    canvas.text(3, 2, "MOVE TRACKED INTERVAL TO TASK");
    canvas.hline(2, 4, width - 4);
    std::vector<const Task*> tasks;
    for (const auto& task : state.tasks) if (!task.archived) tasks.push_back(&task);
    std::sort(tasks.begin(), tasks.end(), [&state](const auto* left, const auto* right) {
        auto order = [&state](int id) {
            const auto found = std::find_if(state.projects.begin(), state.projects.end(),
                [id](const Project& project) { return project.id == id; });
            return found == state.projects.end() ? static_cast<int>(state.projects.size()) :
                                                   static_cast<int>(found - state.projects.begin());
        };
        return std::tuple{order(left->project_id), static_cast<int>(left->status), left->position, left->id} <
               std::tuple{order(right->project_id), static_cast<int>(right->status), right->position, right->id};
    });
    struct Line { std::string text; int task_index{-1}; };
    std::vector<Line> lines;
    int previous_project = -1;
    Status previous_status = Status::Backlog;
    bool have_status = false;
    for (int i = 0; i < static_cast<int>(tasks.size()); ++i) {
        const auto* task = tasks[i];
        if (task->project_id != previous_project) {
            const auto* project = project_by_id(state, task->project_id);
            lines.push_back({"[-] " + (project ? project->name : "Unknown project"), -1});
            previous_project = task->project_id;
            have_status = false;
        }
        if (!have_status || task->status != previous_status) {
            lines.push_back({"  [-] " + status_name(task->status), -1});
            previous_status = task->status;
            have_status = true;
        }
        lines.push_back({"      " + task->title, i});
    }
    int selected_line = 0;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i)
        if (lines[i].task_index == state.task_picker_row) { selected_line = i; break; }
    const int visible = std::max(1, height - 8);
    const int first = std::max(0, std::min(selected_line - visible / 2,
                                           static_cast<int>(lines.size()) - visible));
    for (int i = first; i < static_cast<int>(lines.size()) && i < first + visible; ++i) {
        const bool selected = lines[i].task_index == state.task_picker_row;
        canvas.text(3, 5 + i - first, std::string(selected ? "> " : "  ") +
            clip(lines[i].text, width - 8));
    }
    if (tasks.empty()) canvas.text(3, 6, "No tasks available.");
    if (first > 0) canvas.text(width - 5, 3, "^");
    if (first + visible < static_cast<int>(lines.size())) canvas.text(width - 5, height - 3, "v");
    canvas.text(2, height - 2, "j/k select  Enter choose  Esc cancel");
}

void allowances(Canvas& canvas, const State& state, int width, int height, bool show_focus = true) {
    frame(canvas, width, height, state, "Planner / Allowances");
    canvas.text(3, 2, "FIXED TIME BUDGETS");
    canvas.text(3, 3, "Allowance          Period   This period   Budget    Remaining");
    canvas.hline(2, 4, width - 4);
    int y = 5;
    for (int i = 0; i < static_cast<int>(state.allowances.size()) && y < height - (show_focus ? 7 : 3); ++i) {
        const auto& allowance = state.allowances[i];
        const long long used = allowance_used(state, allowance);
        const long long budget = static_cast<long long>(allowance.budget_minutes) * 60;
        const long long remaining = budget - used;
        const auto stats = allowance_stats(state, allowance.id);
        std::ostringstream line;
        line << (i == state.allowance_row ? "> " : "  ") << std::left << std::setw(19)
             << clip(allowance.name, 18) << std::setw(9) << allowance.period << std::right
             << std::setw(10) << duration(used) << "  " << std::setw(5) << allowance.budget_minutes << "m  ";
        if (remaining >= 0) line << std::setw(10) << duration(remaining);
        else line << std::setw(10) << ("OVER " + duration(-remaining));
        canvas.text(2, y++, clip(line.str(), width - 4));
        if (y < height - (show_focus ? 7 : 3))
            canvas.text(6, y++, "Time: total " + duration(stats.total) + "  today " + duration(stats.today) +
                "  week " + duration(stats.week) + "  session " + duration(stats.session));
    }
    if (state.allowances.empty()) canvas.text(3, 6, "No allowances yet. Press n to create one.");
    if (show_focus) focus_panel(canvas, state, width, height);
    canvas.text(2, height - 2, "j/k select  Left activity  n new  e edit  t start  Space pause  s stop  Delete remove");
}

double goal_total(const State& state, int goal_id, const std::string& through = "9999-12-31");
std::string number(double value);
struct GoalMetrics { double total{}, current_goal{}, net{}; long long elapsed_days{1}; };
GoalMetrics goal_metrics(const State& state, const Goal& goal, const std::string& date);

void todos(Canvas& canvas, const State& state, int width, int height, bool show_focus = true) {
    frame(canvas, width, height, state, "Planner / Daily Todos");
    const std::string day = date_iso(state.todo_date);
    static const std::array<const char*, 3> list_names{"TODAY ONLY", "MORNING - DAILY", "EVENING - DAILY"};
    static const std::array<const char*, 3> list_keys{"today", "morning", "evening"};
    const int active_list = std::clamp(state.todo_list, 0, 2);
    canvas.text(3, 2, std::string(state.todo_date_focused ? "> " : "  ") +
        "DAILY TODOS  " + date_human(state.todo_date));
    int tab_x = 3;
    for (int i = 0; i < 3; ++i) {
        const std::string tab = std::string(i == active_list ? "[" : " ") + list_names[i] +
                                (i == active_list ? "]" : " ");
        canvas.text(tab_x, 3, tab); tab_x += static_cast<int>(tab.size()) + 2;
    }
    canvas.hline(2, 4, width - 4);
    int y = 5, index = 0, completed = 0;
    for (const auto& todo : state.todos) {
        if (todo.list_name != list_keys[active_list] ||
            !todo_visible_on(state, todo, day) ||
            y >= height - (show_focus ? 7 : 3)) continue;
        const bool selected = index == state.todo_row;
        const bool done = todo.recurring ? std::any_of(state.todo_completions.begin(), state.todo_completions.end(),
            [&todo, &day](const auto& completion) { return completion.todo_id == todo.id && completion.date == day; }) :
            todo.completed;
        canvas.text(3, y++, std::string(selected ? "> " : "  ") +
            (done ? "[x] " : "[ ] ") + clip(todo_title_on(state, todo, day), width - 11));
        completed += done ? 1 : 0;
        ++index;
    }
    if (active_list == 0) {
        for (const auto& task : state.tasks) {
            if (task.archived || task.due_date != day || y >= height - (show_focus ? 7 : 3)) continue;
            const bool selected = index == state.todo_row;
            const bool done = task.status == Status::Done;
            const auto* project = project_by_id(state, task.project_id);
            canvas.text(3, y++, std::string(selected ? "> " : "  ") + (done ? "[x] " : "[ ] ") +
                clip(task.title, width - 18) + "  {task}" +
                (project ? " " + clip(project->name, 12) : std::string{}));
            completed += done ? 1 : 0;
            ++index;
        }
        for (const auto& goal : state.goals) {
            if (goal.start_date > day || (!goal.target_date.empty() && goal.target_date < day) ||
                y >= height - (show_focus ? 7 : 3)) continue;
            const bool selected = index == state.todo_row;
            double daily_total = 0;
            for (const auto& entry : state.goal_entries)
                if (entry.goal_id == goal.id && entry.date == day) daily_total += entry.amount;
            const bool filled = goal.daily_goal > 0 && daily_total >= goal.daily_goal;
            const auto metrics = goal_metrics(state, goal, day);
            canvas.text(3, y++, std::string(selected ? "> " : "  ") + (filled ? "[x] " : "[ ] ") +
                clip(goal.title, std::max(8, width - 36)) + "  {goal} " + number(daily_total) + "/" +
                number(goal.daily_goal) + " " + goal.unit + " today");
            if (y < height - (show_focus ? 7 : 3))
                canvas.text(8, y++, "total " + number(metrics.total) + " | current goal " +
                    number(metrics.current_goal) + " | net " + (metrics.net >= 0 ? "+" : "") + number(metrics.net));
            completed += filled ? 1 : 0;
            ++index;
        }
    }
    if (index == 0) { canvas.text(3, 6, "No todos for this day. Press n to add one."); y = 7; }
    canvas.text(3, std::min(y + 1, height - (show_focus ? 7 : 3)),
                std::to_string(completed) + "/" + std::to_string(index) + " complete");
    if (show_focus) focus_panel(canvas, state, width, height);
    canvas.text(2, height - 2, state.todo_date_focused ?
        "h/l or Left/Right day  Enter calendar  g today  Down todos  [/] day" :
        "j/k item  h/l list  n new  Space done  d date  b tomorrow  e edit  Del remove");
}

std::vector<const Task*> deadline_tasks(const State& state) {
    std::vector<const Task*> result;
    for (const auto& task : state.tasks)
        if (!task.archived && !task.due_date.empty()) result.push_back(&task);
    std::sort(result.begin(), result.end(), [](const Task* a, const Task* b) {
        return a->due_date != b->due_date ? a->due_date < b->due_date : a->title < b->title;
    });
    return result;
}

void deadlines(Canvas& canvas, const State& state, int width, int height) {
    frame(canvas, width, height, state, "Planner / Deadlines");
    canvas.text(3, 2, "ALL DEADLINES");
    canvas.text(3, 4, "DATE         WHEN       PROJECT / TASK");
    canvas.hline(2, 5, width - 4);
    const auto tasks = deadline_tasks(state);
    const std::string today = date_iso(state.now);
    const int visible = std::max(1, height - 9);
    const int selected = std::clamp(state.deadline_row, 0, std::max(0, static_cast<int>(tasks.size()) - 1));
    const int first = std::max(0, std::min(selected - visible / 2, static_cast<int>(tasks.size()) - visible));
    for (int i = first; i < static_cast<int>(tasks.size()) && i < first + visible; ++i) {
        const auto* task = tasks[i];
        const auto* project = project_by_id(state, task->project_id);
        std::string when = task->status == Status::Done ? "DONE" :
                           task->due_date < today ? "OVERDUE" : task->due_date == today ? "TODAY" : "UPCOMING";
        canvas.text(3, 6 + i - first, std::string(i == selected ? "> " : "  ") + task->due_date + "  " +
                    clip(when, 9) + "  " + clip(project ? project->name : "Unknown", 14) + " / " +
                    clip(task->title, std::max(8, width - 47)));
    }
    if (tasks.empty()) canvas.text(3, 7, "No task deadlines yet. Edit a task to set one.");
    canvas.text(2, height - 2, "j/k select  Enter open  e edit  M Views");
}

double goal_total(const State& state, int goal_id, const std::string& through) {
    double total = 0;
    for (const auto& entry : state.goal_entries)
        if (entry.goal_id == goal_id && entry.date <= through) total += entry.amount;
    return total;
}

std::string number(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(value == std::floor(value) ? 0 : 1) << value;
    return out.str();
}

GoalMetrics goal_metrics(const State& state, const Goal& goal, const std::string& date) {
    GoalMetrics result;
    result.total = goal_total(state, goal.id, date);
    try {
        result.elapsed_days = std::max<long long>(1, std::chrono::duration_cast<std::chrono::hours>(
            parse_time(date + "T12:00:00") - parse_time(goal.start_date + "T12:00:00")).count() / 24 + 1);
    } catch (const std::exception&) {}
    const double expected = goal.daily_goal * result.elapsed_days;
    result.current_goal = goal.target > 0 ? std::min(goal.target, expected) : expected;
    result.net = result.total - result.current_goal;
    return result;
}

void goals(Canvas& canvas, const State& state, int width, int height) {
    frame(canvas, width, height, state, "Planner / Goals");
    canvas.text(3, 2, "GOALS  cumulative progress and target pace");
    canvas.hline(2, 4, width - 4);
    if (state.goals.empty()) {
        canvas.text(3, 6, "No goals yet. Press n to create one.");
    } else {
        const int selected = std::clamp(state.goal_row, 0, static_cast<int>(state.goals.size()) - 1);
        int y = 6;
        const std::string today = date_iso(state.now);
        for (int i = 0; i < static_cast<int>(state.goals.size()) && y < height - 10; ++i) {
            const auto& goal = state.goals[i];
            const auto metrics = goal_metrics(state, goal, today);
            canvas.text(3, y++, std::string(i == selected ? "> " : "  ") +
                clip(goal.title, std::max(10, width - 60)) + "  total " + number(metrics.total) +
                "  goal " + number(metrics.current_goal) + "  net " +
                (metrics.net >= 0 ? "+" : "") + number(metrics.net) + "  daily " + number(goal.daily_goal));
        }
        const auto& goal = state.goals[selected];
        const auto current = goal_metrics(state, goal, today);
        const double total = current.total;
        const bool has_final_goal = goal.target > 0;
        const double remaining = has_final_goal ? std::max(0.0, goal.target - total) : 0;
        long long days_left = 0;
        if (!goal.target_date.empty()) try {
            days_left = std::max<long long>(0, std::chrono::duration_cast<std::chrono::hours>(
                parse_time(goal.target_date + "T12:00:00") - parse_time(today + "T12:00:00")).count() / 24 + 1);
        } catch (const std::exception&) {}
        const long long elapsed_days = current.elapsed_days;
        const double daily_goal = goal.daily_goal > 0 ? goal.daily_goal :
            (days_left > 0 ? goal.target / std::max<long long>(1, elapsed_days + days_left - 1) : 0);
        const double current_goal = current.current_goal;
        const double net = current.net;
        const double actual_pace = total / elapsed_days;
        std::string projection = has_final_goal && actual_pace > 0 ? date_iso(parse_time(today + "T12:00:00") +
            std::chrono::hours(24 * static_cast<long long>(std::ceil(remaining / actual_pace)))) : "not available";
        const int bar_width = std::max(10, std::min(50, width - 24));
        const int filled = has_final_goal ? std::clamp(static_cast<int>(bar_width * total / goal.target), 0, bar_width) : 0;
        const int detail_y = std::max(y + 1, height - 10);
        canvas.text(3, detail_y, has_final_goal ?
            "[" + std::string(filled, '=') + std::string(bar_width - filled, '.') + "] " +
                number(100.0 * total / goal.target) + "%" :
            "DAILY GOAL - no final total");
        canvas.text(3, detail_y + 2, "Daily goal " + number(daily_goal) + " " + goal.unit +
            "  |  Current total " + number(total));
        canvas.text(3, detail_y + 3, "Current goal " + number(current_goal) + "  |  NET " +
            (net >= 0 ? "+" : "") + number(net));
        canvas.text(3, detail_y + 4, (has_final_goal ? "Remaining " + number(remaining) + " " + goal.unit :
            "No final goal") + "  |  Actual pace " + number(actual_pace) + "/day");
        canvas.text(3, detail_y + 5, "Projected finish " + projection +
            (goal.target_date.empty() ? "  |  no final due date" : "  |  due " + goal.target_date));
        std::vector<const GoalEntry*> entries;
        for (const auto& entry : state.goal_entries) if (entry.goal_id == goal.id) entries.push_back(&entry);
        std::string recent = "Recent: ";
        const int first = std::max(0, static_cast<int>(entries.size()) - 3);
        for (int i = first; i < static_cast<int>(entries.size()); ++i) {
            if (i > first) recent += "  ";
            recent += entries[i]->date + " +" + number(entries[i]->amount);
            if (!entries[i]->note.empty()) recent += " " + entries[i]->note;
        }
        if (entries.empty()) recent += "no datapoints yet";
        canvas.text(3, detail_y + 6, clip(recent, width - 6));
    }
    canvas.text(2, height - 2, "j/k select  n new  + add datapoint  e edit  Delete remove  M Views");
}

void edit_goal(Canvas& canvas, const State& state, int width, int height) {
    frame(canvas, width, height, state, "Planner / Edit Goal");
    const auto found = std::find_if(state.goals.begin(), state.goals.end(), [&state](const Goal& goal) {
        return state.editing_goal_id && goal.id == *state.editing_goal_id;
    });
    if (found == state.goals.end()) { canvas.text(3, 3, "Goal not found."); return; }
    canvas.text(3, 2, "EDIT GOAL  #" + std::to_string(found->id));
    canvas.hline(2, 4, width - 4);
    const std::array<std::pair<std::string, std::string>, 6> fields{{
        {"Name", found->title},
        {"Unit", found->unit},
        {"Final goal", found->target > 0 ? number(found->target) : "none"},
        {"Daily goal", number(found->daily_goal)},
        {"Start date", found->start_date},
        {"Final due", found->target_date.empty() ? "none" : found->target_date}
    }};
    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
        const int y = 5 + i;
        canvas.text(3, y, std::string(i == state.goal_edit_row ? "> " : "  ") +
            fields[i].first + "  " + clip(fields[i].second, width - 22));
    }
    constexpr int comment_y = 12;
    const int comment_height = std::max(3, height - comment_y - 3);
    const int comment_width = std::max(8, width - 6);
    canvas.box(3, comment_y, comment_width, comment_height);
    canvas.text(5, comment_y, std::string(state.goal_edit_row == 6 ? "> Comment " : " Comment "));
    const std::string shown = found->comment.empty() ? "No comment yet. Press Enter to write." : found->comment;
    const auto lines = wrap_lines(shown, std::max(1, comment_width - 4));
    for (int i = 0; i < static_cast<int>(lines.size()) && i < comment_height - 2; ++i)
        canvas.text(5, comment_y + 1 + i, lines[i]);
    canvas.text(3, height - 3, "Select a field and press Enter. Use '-' to clear Final goal or Final due.");
    canvas.text(2, height - 2, state.goal_edit_row == 6 ?
        "Enter write comment  Ctrl+S save  Esc cancel/return  j/k field" :
        "j/k field  e/Enter change  Esc goals  Ctrl+Z undo  Ctrl+Y redo");
}

TimePoint local_date_point(const std::string& date) {
    return parse_time(date + "T12:00:00");
}

std::pair<TimePoint, TimePoint> gantt_period(const State& state) {
    std::time_t raw = Clock::to_time_t(state.gantt_date);
    std::tm tm{}; localtime_r(&raw, &tm);
    tm.tm_hour = 12; tm.tm_min = tm.tm_sec = 0; tm.tm_isdst = -1;
    if (state.gantt_scale == 0) {
        tm.tm_mday -= (tm.tm_wday + 6) % 7;
        const TimePoint start = Clock::from_time_t(mktime(&tm));
        tm.tm_mday += 7;
        return {start, Clock::from_time_t(mktime(&tm))};
    }
    if (state.gantt_scale == 1) {
        tm.tm_mday = 1;
        const TimePoint start = Clock::from_time_t(mktime(&tm));
        ++tm.tm_mon;
        return {start, Clock::from_time_t(mktime(&tm))};
    }
    tm.tm_mon = 0; tm.tm_mday = 1;
    const TimePoint start = Clock::from_time_t(mktime(&tm));
    ++tm.tm_year;
    return {start, Clock::from_time_t(mktime(&tm))};
}

void gantt(Canvas& canvas, const State& state, int width, int height) {
    frame(canvas, width, height, state, "Planner / Gantt");
    const auto [period_start, period_end] = gantt_period(state);
    const std::string scale = state.gantt_scale == 0 ? "WEEK" : state.gantt_scale == 1 ? "MONTH" : "YEAR";
    const std::string filter = state.gantt_filter == 1 ? "Projects" : state.gantt_filter == 2 ? "Goals" : "All";
    canvas.text(3, 2, scale + "  " + date_iso(period_start) + " to " + date_iso(period_end - std::chrono::hours(24)) +
        "   Scale [w]eek [m]onth [y]ear   Filter: " + filter);
    const int label_width = std::clamp(width / 3, 20, 28);
    const int chart_x = label_width + 2;
    const int chart_width = std::max(8, width - chart_x - 2);
    canvas.hline(2, 4, width - 4);
    std::string ticks(chart_width, ' ');
    const int cells = state.gantt_scale == 2 ? 12 : std::max(1, static_cast<int>(std::llround(
        std::chrono::duration_cast<std::chrono::hours>(period_end - period_start).count() / 24.0)));
    for (int i = 0; i < cells; ++i) {
        const int x = std::min(chart_width - 1, i * chart_width / cells);
        if (state.gantt_scale == 2) {
            static const char months[] = "JFMAMJJASOND";
            ticks[x] = months[i];
        } else if (i % (state.gantt_scale == 0 ? 1 : 5) == 0) {
            const std::string date = date_iso(period_start + std::chrono::hours(24 * i));
            const int day = std::stoi(date.substr(8, 2));
            ticks[x] = static_cast<char>('0' + (day / 10) % 10);
            if (x + 1 < chart_width) ticks[x + 1] = static_cast<char>('0' + day % 10);
        }
    }
    canvas.text(chart_x, 5, ticks);
    canvas.hline(2, 6, width - 4);

    struct Row { bool goal{}; int id{}; std::string title, start, end, projected; bool open{}; };
    std::vector<Row> rows;
    if (state.gantt_filter != 2) for (const auto& project : state.projects)
        rows.push_back({false, project.id, project.name, project.plan_start, project.plan_end, {}, false});
    if (state.gantt_filter != 1) for (const auto& goal : state.goals) {
        double total = goal_total(state, goal.id);
        long long elapsed = 1;
        try { elapsed = std::max<long long>(1, std::chrono::duration_cast<std::chrono::hours>(
            local_date_point(date_iso(state.now)) - local_date_point(goal.start_date)).count() / 24 + 1); }
        catch (const std::exception&) {}
        const double pace = total / elapsed;
        std::string projected;
        if (goal.target > 0 && total >= goal.target) projected = date_iso(state.now);
        else if (goal.target > 0 && pace > 0)
            projected = date_iso(state.now + std::chrono::hours(24 * static_cast<long long>(
                std::ceil((goal.target - total) / pace))));
        std::string inferred;
        if (goal.target > 0 && goal.daily_goal > 0) try {
            const long long days = std::max<long long>(1,
                static_cast<long long>(std::ceil(goal.target / goal.daily_goal)));
            inferred = date_iso(local_date_point(goal.start_date) + std::chrono::hours(24 * (days - 1)));
        } catch (const std::exception&) {}
        const std::string end = !goal.target_date.empty() ? goal.target_date : inferred;
        const std::string marker = !projected.empty() ? projected : inferred;
        rows.push_back({true, goal.id, goal.title, goal.start_date, end, marker, end.empty()});
    }
    const int selected = rows.empty() ? 0 : std::clamp(state.gantt_row, 0, static_cast<int>(rows.size()) - 1);
    const auto span = std::max<long long>(1, std::chrono::duration_cast<std::chrono::seconds>(period_end - period_start).count());
    const int visible = std::max(1, height - 11);
    const int first = std::max(0, std::min(selected - visible / 2, static_cast<int>(rows.size()) - visible));
    for (int i = first; i < static_cast<int>(rows.size()) && i < first + visible; ++i) {
        const int y = 7 + i - first;
        const auto& row = rows[i];
        canvas.text(3, y, std::string(i == selected ? "> " : "  ") + (row.goal ? "G " : "P ") +
            clip(row.title, label_width - 7));
        std::string bar(chart_width, '.');
        TimePoint start = period_start, end = row.open ? period_end : period_start;
        if (!row.start.empty()) try { start = local_date_point(row.start); } catch (const std::exception&) {}
        if (!row.end.empty()) try { end = local_date_point(row.end) + std::chrono::hours(24); } catch (const std::exception&) {}
        if (!row.start.empty() && (!row.end.empty() || row.open) && end > period_start && start < period_end) {
            const int from = std::clamp(static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                std::max(start, period_start) - period_start).count() * chart_width / span), 0, chart_width - 1);
            const int to = std::clamp(static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                std::min(end, period_end) - period_start).count() * chart_width / span), from + 1, chart_width);
            std::fill(bar.begin() + from, bar.begin() + to, row.goal ? '=' : '#');
        }
        if (!row.projected.empty()) try {
            const auto point = local_date_point(row.projected);
            if (point >= period_start && point < period_end) {
                const int marker = std::clamp(static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                    point - period_start).count() * chart_width / span), 0, chart_width - 1);
                bar[marker] = '*';
            }
        } catch (const std::exception&) {}
        if (row.open) bar.back() = '>';
        const auto today = local_date_point(date_iso(state.now));
        if (today >= period_start && today < period_end) {
            const int marker = std::clamp(static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                today - period_start).count() * chart_width / span), 0, chart_width - 1);
            if (bar[marker] == '.') bar[marker] = '|';
        }
        canvas.text(chart_x, y, bar);
    }
    if (rows.empty()) canvas.text(3, 8, "No scheduled rows. Select Projects and press e to assign a planning range.");
    canvas.text(3, height - 3, "Legend: # project  = goal runtime  * projected finish  | today  > open-ended");
    canvas.text(2, height - 2, "j/k select  h/l period  w/m/y scale  f filter  e dates  Enter open  Delete clear  g today");
}

void settings(Canvas& canvas, const State& state, int width, int height) {
    frame(canvas, width, height, state, "Planner / Settings");
    canvas.text(3, 2, "SETTINGS");
    canvas.hline(2, 4, width - 4);
    canvas.text(3, 6, "> Timezone    " + state.timezone);
    canvas.text(3, 8, "Use an IANA timezone such as Europe/London, America/New_York, or UTC.");
    canvas.text(3, 10, "Tracked timestamps remain unchanged; display dates and day/week boundaries use this zone.");
    canvas.text(2, height - 2, "e or Enter edit timezone  M Views  q quit");
}

void menu(Canvas& canvas, const State& state, int width, int height) {
    frame(canvas, width, height, state, "Planner / Views");
    canvas.text(3, 2, "CHOOSE A VIEW");
    static const std::array<const char*, 10> views{"Board", "Daily Todos", "Deadlines", "Calendar", "Tracker", "Allowances", "Goals", "Gantt", "Settings", "Help"};
    for (int i = 0; i < static_cast<int>(views.size()); ++i)
        canvas.text(5, 5 + i * 2, std::string(i == state.menu_row ? "> " : "  ") + views[i]);
    canvas.text(3, height - 2, "j/k select  Enter open  Esc return");
}

void help(Canvas& canvas, const State& state, int width, int height) {
    frame(canvas, width, height, state, "Planner / Help");
    const std::array<const char*, 10> lines{
        "M       open Views", "Esc     Views, or return from Task", "Tab     focus Today rail",
        "t       start selected task", "Space   pick/drop card on Board; pause elsewhere",
        "Arrows  navigate or move a picked-up card", "s       stop focus timer",
        "Enter   open task", "Ctrl+Z/Y undo/redo", "q       quit (timer keeps running)"};
    for (int i = 0; i < static_cast<int>(lines.size()) && 3 + i * 2 < height - 2; ++i)
        canvas.text(3, 3 + i * 2, lines[i]);
    canvas.text(3, height - 2, "M Views  Esc open Views");
}

}  // namespace

std::vector<std::string> render(const State& state, int width, int height) {
    if (width >= 90) {
        Canvas canvas(width, height);
        const int rail_width = std::min(38, std::max(32, width / 3));
        today_rail(canvas, state, rail_width, height);
        Canvas main(width - rail_width, height);
        if (state.view == View::Calendar) calendar(main, state, width - rail_width, height, false);
        else if (state.view == View::Task) task_detail(main, state, width - rail_width, height, false);
        else if (state.view == View::EditTask) edit_task(main, state, width - rail_width, height);
        else if (state.view == View::TaskChecklist) task_checklist(main, state, width - rail_width, height);
        else if (state.view == View::Tracker) tracker(main, state, width - rail_width, height, false);
        else if (state.view == View::EditSession) edit_session(main, state, width - rail_width, height);
        else if (state.view == View::SessionTaskPicker) session_task_picker(main, state, width - rail_width, height);
        else if (state.view == View::Allowances) allowances(main, state, width - rail_width, height, false);
        else if (state.view == View::Todos) todos(main, state, width - rail_width, height, false);
        else if (state.view == View::Deadlines) deadlines(main, state, width - rail_width, height);
        else if (state.view == View::Goals) goals(main, state, width - rail_width, height);
        else if (state.view == View::EditGoal) edit_goal(main, state, width - rail_width, height);
        else if (state.view == View::Gantt) gantt(main, state, width - rail_width, height);
        else if (state.view == View::Settings) settings(main, state, width - rail_width, height);
        else if (state.view == View::Menu) menu(main, state, width - rail_width, height);
        else if (state.view == View::Help) help(main, state, width - rail_width, height);
        else board(main, state, width - rail_width, height, false);
        canvas.blit(rail_width, 0, main.take());
        return canvas.take();
    }
    Canvas canvas(width, height);
    if (state.view == View::Calendar) calendar(canvas, state, width, height);
    else if (state.view == View::Task) task_detail(canvas, state, width, height);
    else if (state.view == View::EditTask) edit_task(canvas, state, width, height);
    else if (state.view == View::TaskChecklist) task_checklist(canvas, state, width, height);
    else if (state.view == View::Tracker) tracker(canvas, state, width, height);
    else if (state.view == View::EditSession) edit_session(canvas, state, width, height);
    else if (state.view == View::SessionTaskPicker) session_task_picker(canvas, state, width, height);
    else if (state.view == View::Allowances) allowances(canvas, state, width, height);
    else if (state.view == View::Todos) todos(canvas, state, width, height);
    else if (state.view == View::Deadlines) deadlines(canvas, state, width, height);
    else if (state.view == View::Goals) goals(canvas, state, width, height);
    else if (state.view == View::EditGoal) edit_goal(canvas, state, width, height);
    else if (state.view == View::Gantt) gantt(canvas, state, width, height);
    else if (state.view == View::Settings) settings(canvas, state, width, height);
    else if (state.view == View::Menu) menu(canvas, state, width, height);
    else if (state.view == View::Help) help(canvas, state, width, height);
    else board(canvas, state, width, height);
    return canvas.take();
}

std::string render_text(const State& state, int width, int height) {
    auto rows = render(state, width, height);
    std::ostringstream output;
    for (auto& row : rows) {
        while (!row.empty() && row.back() == ' ') row.pop_back();
        output << row << '\n';
    }
    return output.str();
}

}  // namespace planner
