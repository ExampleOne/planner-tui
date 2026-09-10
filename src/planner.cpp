#include "planner.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <sqlite3.h>

namespace planner {
namespace {

long long epoch(TimePoint point) {
    return std::chrono::duration_cast<std::chrono::seconds>(point.time_since_epoch()).count();
}

void check(int code, sqlite3* db, const char* operation) {
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW) {
        throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(db));
    }
}

void exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int code = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (code != SQLITE_OK) {
        std::string message = error ? error : "SQLite error";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

bool has_column(sqlite3* db, const char* table, const char* column) {
    sqlite3_stmt* statement = nullptr;
    const std::string sql = "PRAGMA table_info(" + std::string(table) + ")";
    check(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr), db, "inspect schema");
    bool found = false;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        if (name && std::string(name) == column) { found = true; break; }
    }
    sqlite3_finalize(statement);
    return found;
}

Status status_from(const char* value) {
    const std::string text = value ? value : "backlog";
    if (text == "ready") return Status::Ready;
    if (text == "doing") return Status::Doing;
    if (text == "done") return Status::Done;
    return Status::Backlog;
}

std::string status_db(Status status) {
    std::string result = status_name(status);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

}  // namespace

std::string status_name(Status status) {
    switch (status) {
        case Status::Backlog: return "BACKLOG";
        case Status::Ready: return "READY";
        case Status::Doing: return "DOING";
        case Status::Done: return "DONE";
    }
    return "BACKLOG";
}

Status next_status(Status status) {
    return static_cast<Status>((static_cast<int>(status) + 1) % 4);
}

const Project* selected_project(const State& state) {
    if (state.projects.empty()) return nullptr;
    const int index = std::clamp(state.project_index, 0, static_cast<int>(state.projects.size()) - 1);
    return &state.projects[index];
}

Project* selected_project(State& state) {
    const Project* project = selected_project(static_cast<const State&>(state));
    if (!project) return nullptr;
    return &*std::find_if(state.projects.begin(), state.projects.end(), [project](const Project& candidate) {
        return candidate.id == project->id;
    });
}

std::vector<const Task*> visible_tasks(const State& state, Status status) {
    std::vector<const Task*> result;
    if (state.all_projects) {
        for (const auto& task : state.tasks)
            if (task.status == status && !task.archived) result.push_back(&task);
        return result;
    }
    const auto* project = selected_project(state);
    if (!project) return result;
    for (const auto& task : state.tasks) {
        if (task.project_id == project->id && task.status == status && !task.archived) result.push_back(&task);
    }
    return result;
}

const Task* selected_task(const State& state) {
    const auto tasks = visible_tasks(state, static_cast<Status>(std::clamp(state.column, 0, 3)));
    if (tasks.empty()) return nullptr;
    return tasks[std::clamp(state.row, 0, static_cast<int>(tasks.size()) - 1)];
}

Task* selected_task(State& state) {
    const Task* task = selected_task(static_cast<const State&>(state));
    if (!task) return nullptr;
    return &*std::find_if(state.tasks.begin(), state.tasks.end(), [task](const Task& candidate) {
        return candidate.id == task->id;
    });
}

bool todo_visible_on(const State& state, const Todo& todo, const std::string& date) {
    if (todo.recurring) {
        if (todo.date > date || (!todo.end_date.empty() && todo.end_date < date)) return false;
    } else if (todo.date != date) return false;
    const auto exception = std::find_if(state.todo_exceptions.begin(), state.todo_exceptions.end(),
        [&todo, &date](const TodoException& item) { return item.todo_id == todo.id && item.date == date; });
    return exception == state.todo_exceptions.end() || !exception->hidden;
}

std::string todo_title_on(const State& state, const Todo& todo, const std::string& date) {
    const auto exception = std::find_if(state.todo_exceptions.begin(), state.todo_exceptions.end(),
        [&todo, &date](const TodoException& item) { return item.todo_id == todo.id && item.date == date; });
    return exception != state.todo_exceptions.end() && !exception->title.empty() ? exception->title : todo.title;
}

long long active_seconds(const State& state, int task_id) {
    if (!state.timer_task_id || *state.timer_task_id != task_id || !state.timer_started_at) return 0;
    return std::max<long long>(0, std::chrono::duration_cast<std::chrono::seconds>(
        state.now - *state.timer_started_at).count());
}

std::string duration(long long seconds) {
    seconds = std::max<long long>(0, seconds);
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << seconds / 3600 << ':'
        << std::setw(2) << (seconds / 60) % 60 << ':' << std::setw(2) << seconds % 60;
    return out.str();
}

std::string date_iso(TimePoint point) {
    const std::time_t value = Clock::to_time_t(point);
    std::tm tm{};
    localtime_r(&value, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d");
    return out.str();
}

std::string date_human(TimePoint point) {
    const std::time_t value = Clock::to_time_t(point);
    std::tm tm{};
    localtime_r(&value, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%a %d %b %Y");
    return out.str();
}

TimePoint parse_time(const std::string& value) {
    std::tm tm{};
    std::istringstream in(value);
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (in.fail()) throw std::runtime_error("Invalid time; expected YYYY-MM-DDTHH:MM:SS");
    tm.tm_isdst = -1;
    return Clock::from_time_t(mktime(&tm));
}

void set_app_timezone(const std::string& timezone) {
    if (timezone.empty() || timezone.find("..") != std::string::npos || timezone.front() == '/' ||
        (timezone != "UTC" && !std::filesystem::exists(std::filesystem::path("/usr/share/zoneinfo") / timezone)))
        throw std::runtime_error("Unknown timezone: " + timezone);
    setenv("TZ", timezone.c_str(), 1);
    tzset();
}

struct Store::Impl {
    sqlite3* db{};
    std::vector<sqlite3*> undo_stack;
    std::vector<sqlite3*> redo_stack;
};

Store::Store(const std::string& path) : impl_(new Impl) {
    check(sqlite3_open(path.c_str(), &impl_->db), impl_->db, "open database");
    sqlite3_busy_timeout(impl_->db, 2000);
}

Store::~Store() {
    if (impl_) {
        for (auto* snapshot : impl_->undo_stack) sqlite3_close(snapshot);
        for (auto* snapshot : impl_->redo_stack) sqlite3_close(snapshot);
        if (impl_->db) sqlite3_close(impl_->db);
        delete impl_;
    }
}

namespace {

sqlite3* snapshot_database(sqlite3* source) {
    sqlite3* snapshot = nullptr;
    check(sqlite3_open(":memory:", &snapshot), snapshot, "open history snapshot");
    sqlite3_backup* backup = sqlite3_backup_init(snapshot, "main", source, "main");
    if (!backup) {
        const std::string error = sqlite3_errmsg(snapshot);
        sqlite3_close(snapshot);
        throw std::runtime_error("create history snapshot: " + error);
    }
    const int code = sqlite3_backup_step(backup, -1);
    sqlite3_backup_finish(backup);
    check(code == SQLITE_DONE ? SQLITE_OK : code, snapshot, "save history snapshot");
    return snapshot;
}

void restore_database(sqlite3* destination, sqlite3* snapshot) {
    sqlite3_backup* backup = sqlite3_backup_init(destination, "main", snapshot, "main");
    if (!backup) throw std::runtime_error("restore history snapshot: " + std::string(sqlite3_errmsg(destination)));
    const int code = sqlite3_backup_step(backup, -1);
    sqlite3_backup_finish(backup);
    check(code == SQLITE_DONE ? SQLITE_OK : code, destination, "restore history snapshot");
}

void clear_snapshots(std::vector<sqlite3*>& snapshots) {
    for (auto* snapshot : snapshots) sqlite3_close(snapshot);
    snapshots.clear();
}

void push_bounded(std::vector<sqlite3*>& snapshots, sqlite3* snapshot) {
    constexpr std::size_t limit = 50;
    if (snapshots.size() == limit) {
        sqlite3_close(snapshots.front());
        snapshots.erase(snapshots.begin());
    }
    snapshots.push_back(snapshot);
}

}  // namespace

void Store::checkpoint() {
    push_bounded(impl_->undo_stack, snapshot_database(impl_->db));
    clear_snapshots(impl_->redo_stack);
}

bool Store::undo() {
    if (impl_->undo_stack.empty()) return false;
    push_bounded(impl_->redo_stack, snapshot_database(impl_->db));
    sqlite3* snapshot = impl_->undo_stack.back();
    impl_->undo_stack.pop_back();
    restore_database(impl_->db, snapshot);
    sqlite3_close(snapshot);
    return true;
}

bool Store::redo() {
    if (impl_->redo_stack.empty()) return false;
    push_bounded(impl_->undo_stack, snapshot_database(impl_->db));
    sqlite3* snapshot = impl_->redo_stack.back();
    impl_->redo_stack.pop_back();
    restore_database(impl_->db, snapshot);
    sqlite3_close(snapshot);
    return true;
}

void Store::initialize() {
    exec(impl_->db, R"sql(
        PRAGMA foreign_keys = ON;
        PRAGMA journal_mode = WAL;
        CREATE TABLE IF NOT EXISTS projects (
            id INTEGER PRIMARY KEY, name TEXT NOT NULL, position INTEGER NOT NULL DEFAULT 0,
            unit_name TEXT NOT NULL DEFAULT 'points', plan_start TEXT NOT NULL DEFAULT '',
            plan_end TEXT NOT NULL DEFAULT ''
        );
        CREATE TABLE IF NOT EXISTS tasks (
            id INTEGER PRIMARY KEY, project_id INTEGER NOT NULL REFERENCES projects(id),
            title TEXT NOT NULL, notes TEXT NOT NULL DEFAULT '', due_date TEXT NOT NULL DEFAULT '',
            tags TEXT NOT NULL DEFAULT '', estimate_minutes INTEGER NOT NULL DEFAULT 0,
            status TEXT NOT NULL DEFAULT 'backlog', position INTEGER NOT NULL DEFAULT 0,
            progress_done INTEGER NOT NULL DEFAULT 0, progress_target INTEGER NOT NULL DEFAULT 0,
            archived INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS task_checklist_items (
            id INTEGER PRIMARY KEY,
            task_id INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
            title TEXT NOT NULL,
            completed INTEGER NOT NULL DEFAULT 0,
            position INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS sessions (
            id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
            started_at INTEGER NOT NULL, ended_at INTEGER
        );
        CREATE TABLE IF NOT EXISTS schedule_blocks (
            id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
            date TEXT NOT NULL, start_time TEXT NOT NULL, duration_minutes INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS allowances (
            id INTEGER PRIMARY KEY, name TEXT NOT NULL, budget_minutes INTEGER NOT NULL,
            period TEXT NOT NULL DEFAULT 'daily'
        );
        CREATE TABLE IF NOT EXISTS allowance_sessions (
            id INTEGER PRIMARY KEY, allowance_id INTEGER NOT NULL REFERENCES allowances(id),
            started_at INTEGER NOT NULL, ended_at INTEGER
        );
        CREATE TABLE IF NOT EXISTS runtime_state (
            id INTEGER PRIMARY KEY CHECK(id=1), paused_task_id INTEGER, paused_allowance_id INTEGER
        );
        INSERT OR IGNORE INTO runtime_state(id) VALUES(1);
        CREATE TABLE IF NOT EXISTS todos (
            id INTEGER PRIMARY KEY, title TEXT NOT NULL, date TEXT NOT NULL,
            completed INTEGER NOT NULL DEFAULT 0, position INTEGER NOT NULL DEFAULT 0,
            list_name TEXT NOT NULL DEFAULT 'today', recurring INTEGER NOT NULL DEFAULT 0,
            end_date TEXT NOT NULL DEFAULT '', series_id INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS todo_completions (
            todo_id INTEGER NOT NULL REFERENCES todos(id) ON DELETE CASCADE,
            date TEXT NOT NULL, PRIMARY KEY(todo_id,date)
        );
        CREATE TABLE IF NOT EXISTS todo_exceptions (
            todo_id INTEGER NOT NULL REFERENCES todos(id) ON DELETE CASCADE,
            date TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', hidden INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY(todo_id,date)
        );
        CREATE TABLE IF NOT EXISTS goals (
            id INTEGER PRIMARY KEY, title TEXT NOT NULL, unit TEXT NOT NULL DEFAULT 'units',
            target REAL NOT NULL, daily_goal REAL NOT NULL DEFAULT 0,
            start_date TEXT NOT NULL, target_date TEXT NOT NULL,
            comment TEXT NOT NULL DEFAULT ''
        );
        CREATE TABLE IF NOT EXISTS goal_entries (
            id INTEGER PRIMARY KEY, goal_id INTEGER NOT NULL REFERENCES goals(id) ON DELETE CASCADE,
            date TEXT NOT NULL, amount REAL NOT NULL, note TEXT NOT NULL DEFAULT ''
        );
        CREATE TABLE IF NOT EXISTS app_settings (
            key TEXT PRIMARY KEY, value TEXT NOT NULL
        );
        INSERT OR IGNORE INTO app_settings(key,value) VALUES('timezone','UTC');
    )sql");
    if (!has_column(impl_->db, "projects", "unit_name"))
        exec(impl_->db, "ALTER TABLE projects ADD COLUMN unit_name TEXT NOT NULL DEFAULT 'points'");
    if (!has_column(impl_->db, "projects", "plan_start"))
        exec(impl_->db, "ALTER TABLE projects ADD COLUMN plan_start TEXT NOT NULL DEFAULT ''");
    if (!has_column(impl_->db, "projects", "plan_end"))
        exec(impl_->db, "ALTER TABLE projects ADD COLUMN plan_end TEXT NOT NULL DEFAULT ''");
    if (!has_column(impl_->db, "tasks", "progress_done"))
        exec(impl_->db, "ALTER TABLE tasks ADD COLUMN progress_done INTEGER NOT NULL DEFAULT 0");
    if (!has_column(impl_->db, "tasks", "progress_target"))
        exec(impl_->db, "ALTER TABLE tasks ADD COLUMN progress_target INTEGER NOT NULL DEFAULT 0");
    if (!has_column(impl_->db, "tasks", "archived"))
        exec(impl_->db, "ALTER TABLE tasks ADD COLUMN archived INTEGER NOT NULL DEFAULT 0");
    if (!has_column(impl_->db, "tasks", "tags"))
        exec(impl_->db, "ALTER TABLE tasks ADD COLUMN tags TEXT NOT NULL DEFAULT ''");
    if (!has_column(impl_->db, "tasks", "estimate_minutes"))
        exec(impl_->db, "ALTER TABLE tasks ADD COLUMN estimate_minutes INTEGER NOT NULL DEFAULT 0");
    if (!has_column(impl_->db, "todos", "list_name"))
        exec(impl_->db, "ALTER TABLE todos ADD COLUMN list_name TEXT NOT NULL DEFAULT 'today'");
    if (!has_column(impl_->db, "todos", "recurring"))
        exec(impl_->db, "ALTER TABLE todos ADD COLUMN recurring INTEGER NOT NULL DEFAULT 0");
    if (!has_column(impl_->db, "todos", "end_date"))
        exec(impl_->db, "ALTER TABLE todos ADD COLUMN end_date TEXT NOT NULL DEFAULT ''");
    if (!has_column(impl_->db, "todos", "series_id"))
        exec(impl_->db, "ALTER TABLE todos ADD COLUMN series_id INTEGER NOT NULL DEFAULT 0");
    if (!has_column(impl_->db, "goals", "daily_goal"))
        exec(impl_->db, "ALTER TABLE goals ADD COLUMN daily_goal REAL NOT NULL DEFAULT 0");
    if (!has_column(impl_->db, "goals", "comment"))
        exec(impl_->db, "ALTER TABLE goals ADD COLUMN comment TEXT NOT NULL DEFAULT ''");
    exec(impl_->db, "UPDATE goals SET daily_goal=target/MAX(1,julianday(target_date)-julianday(start_date)+1) "
                    "WHERE daily_goal<=0 AND target_date<>''");
    exec(impl_->db, "UPDATE todos SET series_id=id WHERE recurring=1 AND series_id=0");
}

State Store::load(TimePoint now) {
    State state;
    state.now = now;
    state.calendar_date = now;
    state.todo_date = now;
    state.tracker_date = now;
    state.gantt_date = now;
    state.timezone = timezone();
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "SELECT id,name,unit_name,plan_start,plan_end FROM projects ORDER BY position,id", -1,
                             &statement, nullptr), impl_->db, "load projects");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        state.projects.push_back({sqlite3_column_int(statement, 0),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 3)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 4))});
    }
    sqlite3_finalize(statement);

    check(sqlite3_prepare_v2(impl_->db,
        "SELECT id,title,date,completed,position,list_name,recurring,end_date,series_id FROM todos ORDER BY date,position,id", -1,
        &statement, nullptr), impl_->db, "load todos");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        state.todos.push_back({sqlite3_column_int(statement, 0),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)),
            sqlite3_column_int(statement, 3) != 0, sqlite3_column_int(statement, 4),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 5)),
            sqlite3_column_int(statement, 6) != 0,
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 7)), sqlite3_column_int(statement, 8)});
    }
    sqlite3_finalize(statement);
    check(sqlite3_prepare_v2(impl_->db,
        "SELECT todo_id,date,title,hidden FROM todo_exceptions ORDER BY date,todo_id", -1,
        &statement, nullptr), impl_->db, "load todo exceptions");
    while (sqlite3_step(statement) == SQLITE_ROW)
        state.todo_exceptions.push_back({sqlite3_column_int(statement, 0),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)),
            sqlite3_column_int(statement, 3) != 0});
    sqlite3_finalize(statement);
    check(sqlite3_prepare_v2(impl_->db,
        "SELECT id,title,unit,target,daily_goal,start_date,target_date,comment FROM goals ORDER BY id", -1,
        &statement, nullptr), impl_->db, "load goals");
    while (sqlite3_step(statement) == SQLITE_ROW)
        state.goals.push_back({sqlite3_column_int(statement, 0),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)), sqlite3_column_double(statement, 3),
            sqlite3_column_double(statement, 4),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 5)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 6)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 7))});
    sqlite3_finalize(statement);
    check(sqlite3_prepare_v2(impl_->db,
        "SELECT id,goal_id,date,amount,note FROM goal_entries ORDER BY date,id", -1,
        &statement, nullptr), impl_->db, "load goal entries");
    while (sqlite3_step(statement) == SQLITE_ROW)
        state.goal_entries.push_back({sqlite3_column_int(statement, 0), sqlite3_column_int(statement, 1),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)), sqlite3_column_double(statement, 3),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 4))});
    sqlite3_finalize(statement);
    check(sqlite3_prepare_v2(impl_->db,
        "SELECT todo_id,date FROM todo_completions ORDER BY date,todo_id", -1,
        &statement, nullptr), impl_->db, "load todo completions");
    while (sqlite3_step(statement) == SQLITE_ROW)
        state.todo_completions.push_back({sqlite3_column_int(statement, 0),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1))});
    sqlite3_finalize(statement);
    check(sqlite3_prepare_v2(impl_->db,
        "SELECT paused_task_id,paused_allowance_id FROM runtime_state WHERE id=1", -1,
        &statement, nullptr), impl_->db, "load paused activity");
    if (sqlite3_step(statement) == SQLITE_ROW) {
        if (sqlite3_column_type(statement, 0) != SQLITE_NULL)
            state.paused_task_id = sqlite3_column_int(statement, 0);
        if (sqlite3_column_type(statement, 1) != SQLITE_NULL)
            state.paused_allowance_id = sqlite3_column_int(statement, 1);
    }
    sqlite3_finalize(statement);

    check(sqlite3_prepare_v2(impl_->db,
        "SELECT id,name,budget_minutes,period FROM allowances ORDER BY id", -1, &statement, nullptr),
        impl_->db, "load allowances");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        state.allowances.push_back({sqlite3_column_int(statement, 0),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
            sqlite3_column_int(statement, 2),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 3))});
    }
    sqlite3_finalize(statement);

    check(sqlite3_prepare_v2(impl_->db,
        "SELECT id,allowance_id,started_at,ended_at FROM allowance_sessions ORDER BY started_at,id",
        -1, &statement, nullptr), impl_->db, "load allowance sessions");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        AllowanceSession session;
        session.id = sqlite3_column_int(statement, 0);
        session.allowance_id = sqlite3_column_int(statement, 1);
        session.started_at = TimePoint{std::chrono::seconds(sqlite3_column_int64(statement, 2))};
        if (sqlite3_column_type(statement, 3) != SQLITE_NULL)
            session.ended_at = TimePoint{std::chrono::seconds(sqlite3_column_int64(statement, 3))};
        state.allowance_sessions.push_back(session);
    }
    sqlite3_finalize(statement);

    const char* task_sql = R"sql(
        SELECT t.id,t.project_id,t.title,t.notes,t.due_date,t.tags,t.estimate_minutes,t.status,t.position,
               COALESCE(SUM(CASE WHEN s.ended_at IS NOT NULL THEN s.ended_at-s.started_at ELSE 0 END),0),
               t.progress_done,t.progress_target,t.archived
        FROM tasks t LEFT JOIN sessions s ON s.task_id=t.id
        GROUP BY t.id ORDER BY t.position,t.id
    )sql";
    check(sqlite3_prepare_v2(impl_->db, task_sql, -1, &statement, nullptr), impl_->db, "load tasks");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        state.tasks.push_back({
            sqlite3_column_int(statement, 0), sqlite3_column_int(statement, 1),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 3)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 4)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 5)), sqlite3_column_int(statement, 6),
            status_from(reinterpret_cast<const char*>(sqlite3_column_text(statement, 7))),
            sqlite3_column_int(statement, 8), sqlite3_column_int64(statement, 9),
            sqlite3_column_int(statement, 10), sqlite3_column_int(statement, 11),
            sqlite3_column_int(statement, 12) != 0});
    }
    sqlite3_finalize(statement);

    check(sqlite3_prepare_v2(impl_->db,
        "SELECT id,task_id,title,completed,position FROM task_checklist_items ORDER BY task_id,position,id",
        -1, &statement, nullptr), impl_->db, "load task checklist items");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        state.checklist_items.push_back({sqlite3_column_int(statement, 0), sqlite3_column_int(statement, 1),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)),
            sqlite3_column_int(statement, 3) != 0, sqlite3_column_int(statement, 4)});
    }
    sqlite3_finalize(statement);

    check(sqlite3_prepare_v2(impl_->db,
        "SELECT id,task_id,started_at,ended_at FROM sessions ORDER BY started_at,id", -1,
        &statement, nullptr), impl_->db, "load sessions");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        Session session;
        session.id = sqlite3_column_int(statement, 0);
        session.task_id = sqlite3_column_int(statement, 1);
        session.started_at = TimePoint{std::chrono::seconds(sqlite3_column_int64(statement, 2))};
        if (sqlite3_column_type(statement, 3) != SQLITE_NULL)
            session.ended_at = TimePoint{std::chrono::seconds(sqlite3_column_int64(statement, 3))};
        state.sessions.push_back(session);
    }
    sqlite3_finalize(statement);

    check(sqlite3_prepare_v2(impl_->db,
        "SELECT id,task_id,date,start_time,duration_minutes FROM schedule_blocks ORDER BY date,start_time,id",
        -1, &statement, nullptr), impl_->db, "load schedule");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        state.schedule.push_back({sqlite3_column_int(statement, 0), sqlite3_column_int(statement, 1),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 3)),
            sqlite3_column_int(statement, 4)});
    }
    sqlite3_finalize(statement);

    check(sqlite3_prepare_v2(impl_->db,
        "SELECT task_id,started_at FROM sessions WHERE ended_at IS NULL ORDER BY id DESC LIMIT 1",
        -1, &statement, nullptr), impl_->db, "load timer");
    if (sqlite3_step(statement) == SQLITE_ROW) {
        state.timer_task_id = sqlite3_column_int(statement, 0);
        state.timer_started_at = TimePoint{std::chrono::seconds(sqlite3_column_int64(statement, 1))};
    }
    sqlite3_finalize(statement);
    check(sqlite3_prepare_v2(impl_->db,
        "SELECT allowance_id,started_at FROM allowance_sessions WHERE ended_at IS NULL ORDER BY id DESC LIMIT 1",
        -1, &statement, nullptr), impl_->db, "load allowance timer");
    if (sqlite3_step(statement) == SQLITE_ROW) {
        state.active_allowance_id = sqlite3_column_int(statement, 0);
        state.allowance_started_at = TimePoint{std::chrono::seconds(sqlite3_column_int64(statement, 1))};
    }
    sqlite3_finalize(statement);
    return state;
}

int Store::add_project(const std::string& name) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO projects(name,position) VALUES(?,(SELECT COALESCE(MAX(position),-1)+1 FROM projects))",
        -1, &statement, nullptr), impl_->db, "prepare project");
    sqlite3_bind_text(statement, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement), impl_->db, "add project");
    sqlite3_finalize(statement);
    return static_cast<int>(sqlite3_last_insert_rowid(impl_->db));
}

int Store::add_task(int project_id, const std::string& title, Status status) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO tasks(project_id,title,status,position) VALUES(?,?,?,"
        "(SELECT COALESCE(MAX(position),-1)+1 FROM tasks WHERE project_id=? AND status=?))",
        -1, &statement, nullptr), impl_->db, "prepare task");
    const std::string status_value = status_db(status);
    sqlite3_bind_int(statement, 1, project_id);
    sqlite3_bind_text(statement, 2, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, status_value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4, project_id);
    sqlite3_bind_text(statement, 5, status_value.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement), impl_->db, "add task");
    sqlite3_finalize(statement);
    return static_cast<int>(sqlite3_last_insert_rowid(impl_->db));
}

void Store::update_project(const Project& project) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE projects SET name=?,unit_name=?,plan_start=?,plan_end=? WHERE id=?", -1,
                             &statement, nullptr), impl_->db, "prepare update project");
    sqlite3_bind_text(statement, 1, project.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, project.unit_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, project.plan_start.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, project.plan_end.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 5, project.id);
    check(sqlite3_step(statement), impl_->db, "update project");
    sqlite3_finalize(statement);
}

void Store::update_task(const Task& task) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE tasks SET title=?,notes=?,due_date=?,tags=?,estimate_minutes=?,status=?,"
        "progress_done=?,progress_target=? WHERE id=?",
        -1, &statement, nullptr),
        impl_->db, "prepare update task");
    const std::string status_value = status_db(task.status);
    sqlite3_bind_text(statement, 1, task.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, task.notes.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, task.due_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, task.tags.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 5, task.estimate_minutes);
    sqlite3_bind_text(statement, 6, status_value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 7, task.progress_done);
    sqlite3_bind_int(statement, 8, task.progress_target);
    sqlite3_bind_int(statement, 9, task.id);
    check(sqlite3_step(statement), impl_->db, "update task");
    sqlite3_finalize(statement);
}

int Store::add_checklist_item(int task_id, const std::string& title) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO task_checklist_items(task_id,title,position) VALUES(?,?,"
        "(SELECT COALESCE(MAX(position),-1)+1 FROM task_checklist_items WHERE task_id=?))",
        -1, &statement, nullptr), impl_->db, "prepare checklist item");
    sqlite3_bind_int(statement, 1, task_id);
    sqlite3_bind_text(statement, 2, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 3, task_id);
    check(sqlite3_step(statement), impl_->db, "add checklist item");
    sqlite3_finalize(statement);
    return static_cast<int>(sqlite3_last_insert_rowid(impl_->db));
}

void Store::update_checklist_item(const ChecklistItem& item) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE task_checklist_items SET title=?,completed=?,position=? WHERE id=?", -1,
        &statement, nullptr), impl_->db, "prepare checklist item update");
    sqlite3_bind_text(statement, 1, item.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, item.completed ? 1 : 0);
    sqlite3_bind_int(statement, 3, item.position);
    sqlite3_bind_int(statement, 4, item.id);
    check(sqlite3_step(statement), impl_->db, "update checklist item");
    sqlite3_finalize(statement);
}

void Store::move_checklist_item(int item_id, int target_position) {
    exec(impl_->db, "BEGIN IMMEDIATE");
    try {
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(impl_->db, "SELECT task_id FROM task_checklist_items WHERE id=?", -1,
            &statement, nullptr), impl_->db, "find moving checklist item");
        sqlite3_bind_int(statement, 1, item_id);
        if (sqlite3_step(statement) != SQLITE_ROW) {
            sqlite3_finalize(statement);
            throw std::runtime_error("Checklist item to move was not found");
        }
        const int task_id = sqlite3_column_int(statement, 0);
        sqlite3_finalize(statement);

        std::vector<int> ids;
        check(sqlite3_prepare_v2(impl_->db,
            "SELECT id FROM task_checklist_items WHERE task_id=? ORDER BY position,id", -1,
            &statement, nullptr), impl_->db, "load checklist order");
        sqlite3_bind_int(statement, 1, task_id);
        while (sqlite3_step(statement) == SQLITE_ROW) ids.push_back(sqlite3_column_int(statement, 0));
        sqlite3_finalize(statement);
        ids.erase(std::remove(ids.begin(), ids.end(), item_id), ids.end());
        target_position = std::clamp(target_position, 0, static_cast<int>(ids.size()));
        ids.insert(ids.begin() + target_position, item_id);
        for (int position = 0; position < static_cast<int>(ids.size()); ++position) {
            check(sqlite3_prepare_v2(impl_->db,
                "UPDATE task_checklist_items SET position=? WHERE id=?", -1,
                &statement, nullptr), impl_->db, "write checklist order");
            sqlite3_bind_int(statement, 1, position);
            sqlite3_bind_int(statement, 2, ids[position]);
            check(sqlite3_step(statement), impl_->db, "move checklist item");
            sqlite3_finalize(statement);
        }
        exec(impl_->db, "COMMIT");
    } catch (...) {
        exec(impl_->db, "ROLLBACK");
        throw;
    }
}

void Store::delete_checklist_item(int item_id) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db, "DELETE FROM task_checklist_items WHERE id=?", -1,
        &statement, nullptr), impl_->db, "prepare checklist item deletion");
    sqlite3_bind_int(statement, 1, item_id);
    check(sqlite3_step(statement), impl_->db, "delete checklist item");
    sqlite3_finalize(statement);
}

void Store::move_task(int task_id, Status target_status, int target_position) {
    exec(impl_->db, "BEGIN IMMEDIATE");
    try {
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(impl_->db, "SELECT project_id,status FROM tasks WHERE id=?", -1,
                                 &statement, nullptr), impl_->db, "find moving task");
        sqlite3_bind_int(statement, 1, task_id);
        if (sqlite3_step(statement) != SQLITE_ROW) {
            sqlite3_finalize(statement);
            throw std::runtime_error("Task to move was not found");
        }
        const int project_id = sqlite3_column_int(statement, 0);
        const Status old_status = status_from(reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)));
        sqlite3_finalize(statement);

        auto ids_for = [&](Status status) {
            std::vector<int> ids;
            sqlite3_stmt* query = nullptr;
            const std::string status_value = status_db(status);
            check(sqlite3_prepare_v2(impl_->db,
                "SELECT id FROM tasks WHERE project_id=? AND status=? ORDER BY position,id", -1,
                &query, nullptr), impl_->db, "load card order");
            sqlite3_bind_int(query, 1, project_id);
            sqlite3_bind_text(query, 2, status_value.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(query) == SQLITE_ROW) ids.push_back(sqlite3_column_int(query, 0));
            sqlite3_finalize(query);
            ids.erase(std::remove(ids.begin(), ids.end(), task_id), ids.end());
            return ids;
        };
        auto old_ids = ids_for(old_status);
        auto target_ids = old_status == target_status ? old_ids : ids_for(target_status);
        target_position = std::clamp(target_position, 0, static_cast<int>(target_ids.size()));
        target_ids.insert(target_ids.begin() + target_position, task_id);

        auto write_order = [&](const std::vector<int>& ids, Status status) {
            const std::string status_value = status_db(status);
            for (int position = 0; position < static_cast<int>(ids.size()); ++position) {
                sqlite3_stmt* update = nullptr;
                check(sqlite3_prepare_v2(impl_->db,
                    "UPDATE tasks SET status=?,position=? WHERE id=?", -1, &update, nullptr),
                    impl_->db, "write card order");
                sqlite3_bind_text(update, 1, status_value.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(update, 2, position);
                sqlite3_bind_int(update, 3, ids[position]);
                check(sqlite3_step(update), impl_->db, "move card");
                sqlite3_finalize(update);
            }
        };
        if (old_status != target_status) write_order(old_ids, old_status);
        write_order(target_ids, target_status);
        exec(impl_->db, "COMMIT");
    } catch (...) {
        exec(impl_->db, "ROLLBACK");
        throw;
    }
}

void Store::move_task_to_project(int task_id, int project_id) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE tasks SET project_id=?,position=(SELECT COALESCE(MAX(position),-1)+1 FROM tasks "
        "WHERE project_id=? AND status=(SELECT status FROM tasks WHERE id=?)) WHERE id=?", -1,
        &statement, nullptr), impl_->db, "prepare project transfer");
    sqlite3_bind_int(statement, 1, project_id);
    sqlite3_bind_int(statement, 2, project_id);
    sqlite3_bind_int(statement, 3, task_id);
    sqlite3_bind_int(statement, 4, task_id);
    check(sqlite3_step(statement), impl_->db, "transfer task to project");
    sqlite3_finalize(statement);
}

void Store::archive_task(int task_id) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db, "UPDATE tasks SET archived=1 WHERE id=?", -1,
                             &statement, nullptr), impl_->db, "prepare archive task");
    sqlite3_bind_int(statement, 1, task_id);
    check(sqlite3_step(statement), impl_->db, "archive task");
    sqlite3_finalize(statement);
}

void Store::delete_task(int task_id) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "SELECT (SELECT COUNT(*) FROM sessions WHERE task_id=?)+"
        "(SELECT COUNT(*) FROM schedule_blocks WHERE task_id=?)", -1, &statement, nullptr),
        impl_->db, "check task history");
    sqlite3_bind_int(statement, 1, task_id);
    sqlite3_bind_int(statement, 2, task_id);
    const bool has_history = sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) > 0;
    sqlite3_finalize(statement);
    if (has_history) throw std::runtime_error("Task has tracking or schedule history; archive it instead");
    check(sqlite3_prepare_v2(impl_->db, "DELETE FROM tasks WHERE id=?", -1, &statement, nullptr),
          impl_->db, "prepare delete task");
    sqlite3_bind_int(statement, 1, task_id);
    check(sqlite3_step(statement), impl_->db, "delete task");
    sqlite3_finalize(statement);
}

int Store::add_schedule(int task_id, const std::string& date, const std::string& start_time,
                        int duration_minutes) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO schedule_blocks(task_id,date,start_time,duration_minutes) VALUES(?,?,?,?)",
        -1, &statement, nullptr), impl_->db, "prepare schedule block");
    sqlite3_bind_int(statement, 1, task_id);
    sqlite3_bind_text(statement, 2, date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, start_time.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4, duration_minutes);
    check(sqlite3_step(statement), impl_->db, "add schedule block");
    sqlite3_finalize(statement);
    return static_cast<int>(sqlite3_last_insert_rowid(impl_->db));
}

void Store::start_timer(int task_id, TimePoint now) {
    stop_timer(now);
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO sessions(task_id,started_at) VALUES(?,?)", -1, &statement, nullptr),
        impl_->db, "prepare start timer");
    sqlite3_bind_int(statement, 1, task_id);
    sqlite3_bind_int64(statement, 2, epoch(now));
    check(sqlite3_step(statement), impl_->db, "start timer");
    sqlite3_finalize(statement);
}

void Store::stop_timer(TimePoint now) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE sessions SET ended_at=? WHERE ended_at IS NULL", -1, &statement, nullptr),
        impl_->db, "prepare stop timer");
    sqlite3_bind_int64(statement, 1, epoch(now));
    check(sqlite3_step(statement), impl_->db, "stop timer");
    sqlite3_finalize(statement);
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE allowance_sessions SET ended_at=? WHERE ended_at IS NULL", -1, &statement, nullptr),
        impl_->db, "prepare stop allowance timer");
    sqlite3_bind_int64(statement, 1, epoch(now));
    check(sqlite3_step(statement), impl_->db, "stop allowance timer");
    sqlite3_finalize(statement);
}

int Store::add_allowance(const std::string& name, int budget_minutes, const std::string& period) {
    if (budget_minutes <= 0 || (period != "daily" && period != "weekly"))
        throw std::runtime_error("Invalid allowance budget");
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO allowances(name,budget_minutes,period) VALUES(?,?,?)", -1, &statement, nullptr),
        impl_->db, "prepare allowance");
    sqlite3_bind_text(statement, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, budget_minutes);
    sqlite3_bind_text(statement, 3, period.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement), impl_->db, "add allowance");
    sqlite3_finalize(statement);
    return static_cast<int>(sqlite3_last_insert_rowid(impl_->db));
}

void Store::update_allowance(const Allowance& allowance) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE allowances SET name=?,budget_minutes=?,period=? WHERE id=?", -1, &statement, nullptr),
        impl_->db, "prepare update allowance");
    sqlite3_bind_text(statement, 1, allowance.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, allowance.budget_minutes);
    sqlite3_bind_text(statement, 3, allowance.period.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4, allowance.id);
    check(sqlite3_step(statement), impl_->db, "update allowance");
    sqlite3_finalize(statement);
}

void Store::start_allowance(int allowance_id, TimePoint now) {
    stop_timer(now);
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO allowance_sessions(allowance_id,started_at) VALUES(?,?)", -1, &statement, nullptr),
        impl_->db, "prepare start allowance");
    sqlite3_bind_int(statement, 1, allowance_id);
    sqlite3_bind_int64(statement, 2, epoch(now));
    check(sqlite3_step(statement), impl_->db, "start allowance");
    sqlite3_finalize(statement);
}

void Store::delete_allowance(int allowance_id) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "SELECT COUNT(*) FROM allowance_sessions WHERE allowance_id=?", -1, &statement, nullptr),
        impl_->db, "check allowance history");
    sqlite3_bind_int(statement, 1, allowance_id);
    const bool has_history = sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) > 0;
    sqlite3_finalize(statement);
    if (has_history) throw std::runtime_error("Allowance has tracking history and cannot be deleted");
    check(sqlite3_prepare_v2(impl_->db, "DELETE FROM allowances WHERE id=?", -1, &statement, nullptr),
          impl_->db, "prepare delete allowance");
    sqlite3_bind_int(statement, 1, allowance_id);
    check(sqlite3_step(statement), impl_->db, "delete allowance");
    sqlite3_finalize(statement);
}

int Store::add_todo(const std::string& title, const std::string& date, const std::string& list_name) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO todos(title,date,list_name,recurring,position) VALUES(?,?,?,? ,"
        "(SELECT COALESCE(MAX(position),-1)+1 FROM todos WHERE list_name=?))", -1,
        &statement, nullptr), impl_->db, "prepare todo");
    sqlite3_bind_text(statement, 1, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, list_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4, list_name == "today" ? 0 : 1);
    sqlite3_bind_text(statement, 5, list_name.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement), impl_->db, "add todo");
    sqlite3_finalize(statement);
    const int id = static_cast<int>(sqlite3_last_insert_rowid(impl_->db));
    if (list_name != "today") {
        check(sqlite3_prepare_v2(impl_->db, "UPDATE todos SET series_id=id WHERE id=?", -1,
            &statement, nullptr), impl_->db, "prepare todo series id");
        sqlite3_bind_int(statement, 1, id);
        check(sqlite3_step(statement), impl_->db, "set todo series id");
        sqlite3_finalize(statement);
    }
    return id;
}

void Store::update_todo(const Todo& todo) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE todos SET title=?,date=?,completed=?,position=?,list_name=?,recurring=?,end_date=?,series_id=? WHERE id=?", -1,
        &statement, nullptr), impl_->db, "prepare update todo");
    sqlite3_bind_text(statement, 1, todo.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, todo.date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 3, todo.completed ? 1 : 0);
    sqlite3_bind_int(statement, 4, todo.position);
    sqlite3_bind_text(statement, 5, todo.list_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 6, todo.recurring ? 1 : 0);
    sqlite3_bind_text(statement, 7, todo.end_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 8, todo.series_id);
    sqlite3_bind_int(statement, 9, todo.id);
    check(sqlite3_step(statement), impl_->db, "update todo");
    sqlite3_finalize(statement);
}

void Store::set_todo_completed(int todo_id, const std::string& date, bool completed) {
    sqlite3_stmt* statement = nullptr;
    const char* sql = completed ?
        "INSERT OR IGNORE INTO todo_completions(todo_id,date) VALUES(?,?)" :
        "DELETE FROM todo_completions WHERE todo_id=? AND date=?";
    check(sqlite3_prepare_v2(impl_->db, sql, -1, &statement, nullptr), impl_->db,
          "prepare recurring todo completion");
    sqlite3_bind_int(statement, 1, todo_id);
    sqlite3_bind_text(statement, 2, date.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement), impl_->db, "update recurring todo completion");
    sqlite3_finalize(statement);
}

void Store::delete_todo(int todo_id) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "DELETE FROM todos WHERE id=? OR (recurring=1 AND series_id=(SELECT series_id FROM todos WHERE id=?))",
        -1, &statement, nullptr),
          impl_->db, "prepare delete todo");
    sqlite3_bind_int(statement, 1, todo_id);
    sqlite3_bind_int(statement, 2, todo_id);
    check(sqlite3_step(statement), impl_->db, "delete todo");
    sqlite3_finalize(statement);
}

void Store::hide_todo_on(int todo_id, const std::string& date) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO todo_exceptions(todo_id,date,hidden) VALUES(?,?,1) "
        "ON CONFLICT(todo_id,date) DO UPDATE SET hidden=1", -1, &statement, nullptr),
        impl_->db, "prepare todo date deletion");
    sqlite3_bind_int(statement, 1, todo_id);
    sqlite3_bind_text(statement, 2, date.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement), impl_->db, "delete todo on date");
    sqlite3_finalize(statement);
}

void Store::rename_todo_on(int todo_id, const std::string& date, const std::string& title) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO todo_exceptions(todo_id,date,title) VALUES(?,?,?) "
        "ON CONFLICT(todo_id,date) DO UPDATE SET title=excluded.title,hidden=0", -1,
        &statement, nullptr), impl_->db, "prepare todo date rename");
    sqlite3_bind_int(statement, 1, todo_id);
    sqlite3_bind_text(statement, 2, date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, title.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement), impl_->db, "rename todo on date");
    sqlite3_finalize(statement);
}

void Store::rename_todo_all(int todo_id, const std::string& title) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE todos SET title=? WHERE recurring=1 AND series_id=(SELECT series_id FROM todos WHERE id=?)",
        -1, &statement, nullptr), impl_->db, "prepare all-date todo rename");
    sqlite3_bind_text(statement, 1, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, todo_id);
    check(sqlite3_step(statement), impl_->db, "rename todo for all dates");
    sqlite3_finalize(statement);
}

void Store::end_todo_before(int todo_id, const std::string& date) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE todos SET end_date=date(?,'-1 day') WHERE id=?", -1, &statement, nullptr),
        impl_->db, "prepare ending todo recurrence");
    sqlite3_bind_text(statement, 1, date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, todo_id);
    check(sqlite3_step(statement), impl_->db, "end todo recurrence");
    sqlite3_finalize(statement);
}

int Store::split_todo_from(int todo_id, const std::string& date, const std::string& new_title) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "SELECT title,list_name,position,series_id FROM todos WHERE id=? AND recurring=1", -1,
        &statement, nullptr), impl_->db, "prepare recurring todo split");
    sqlite3_bind_int(statement, 1, todo_id);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        throw std::runtime_error("Recurring todo not found.");
    }
    const std::string old_title = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    const std::string list = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
    const int position = sqlite3_column_int(statement, 2);
    const int series_id = sqlite3_column_int(statement, 3);
    sqlite3_finalize(statement);
    end_todo_before(todo_id, date);
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO todos(title,date,list_name,recurring,position,series_id) VALUES(?,?,?,1,?,?)", -1,
        &statement, nullptr), impl_->db, "prepare new todo series");
    const std::string title = new_title.empty() ? old_title : new_title;
    sqlite3_bind_text(statement, 1, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, list.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4, position);
    sqlite3_bind_int(statement, 5, series_id);
    check(sqlite3_step(statement), impl_->db, "create new todo series");
    sqlite3_finalize(statement);
    return static_cast<int>(sqlite3_last_insert_rowid(impl_->db));
}

int Store::add_goal(const std::string& title, const std::string& unit, double target,
                    const std::string& start_date, const std::string& target_date, double daily_goal,
                    const std::string& comment) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO goals(title,unit,target,daily_goal,start_date,target_date,comment) VALUES(?,?,?,?,?,?,?)", -1,
        &statement, nullptr), impl_->db, "prepare goal");
    sqlite3_bind_text(statement, 1, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, unit.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(statement, 3, target);
    sqlite3_bind_double(statement, 4, daily_goal);
    sqlite3_bind_text(statement, 5, start_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, target_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 7, comment.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement), impl_->db, "add goal");
    sqlite3_finalize(statement);
    return static_cast<int>(sqlite3_last_insert_rowid(impl_->db));
}

void Store::update_goal(const Goal& goal) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE goals SET title=?,unit=?,target=?,daily_goal=?,start_date=?,target_date=?,comment=? WHERE id=?", -1,
        &statement, nullptr), impl_->db, "prepare update goal");
    sqlite3_bind_text(statement, 1, goal.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, goal.unit.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(statement, 3, goal.target);
    sqlite3_bind_double(statement, 4, goal.daily_goal);
    sqlite3_bind_text(statement, 5, goal.start_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, goal.target_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 7, goal.comment.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 8, goal.id);
    check(sqlite3_step(statement), impl_->db, "update goal");
    sqlite3_finalize(statement);
}

void Store::delete_goal(int goal_id) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db, "DELETE FROM goals WHERE id=?", -1, &statement, nullptr),
        impl_->db, "prepare delete goal");
    sqlite3_bind_int(statement, 1, goal_id);
    check(sqlite3_step(statement), impl_->db, "delete goal");
    sqlite3_finalize(statement);
}

int Store::add_goal_entry(int goal_id, const std::string& date, double amount, const std::string& note) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO goal_entries(goal_id,date,amount,note) VALUES(?,?,?,?)", -1,
        &statement, nullptr), impl_->db, "prepare goal entry");
    sqlite3_bind_int(statement, 1, goal_id);
    sqlite3_bind_text(statement, 2, date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(statement, 3, amount);
    sqlite3_bind_text(statement, 4, note.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement), impl_->db, "add goal entry");
    sqlite3_finalize(statement);
    return static_cast<int>(sqlite3_last_insert_rowid(impl_->db));
}

void Store::clear_goal_entries_on(int goal_id, const std::string& date) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "DELETE FROM goal_entries WHERE goal_id=? AND date=?", -1, &statement, nullptr),
        impl_->db, "prepare clearing daily goal entries");
    sqlite3_bind_int(statement, 1, goal_id);
    sqlite3_bind_text(statement, 2, date.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement), impl_->db, "clear daily goal entries");
    sqlite3_finalize(statement);
}

void Store::remember_paused_task(int task_id) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE runtime_state SET paused_task_id=?,paused_allowance_id=NULL WHERE id=1", -1,
        &statement, nullptr), impl_->db, "remember paused task");
    sqlite3_bind_int(statement, 1, task_id);
    check(sqlite3_step(statement), impl_->db, "remember paused task");
    sqlite3_finalize(statement);
}

void Store::remember_paused_allowance(int allowance_id) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE runtime_state SET paused_task_id=NULL,paused_allowance_id=? WHERE id=1", -1,
        &statement, nullptr), impl_->db, "remember paused allowance");
    sqlite3_bind_int(statement, 1, allowance_id);
    check(sqlite3_step(statement), impl_->db, "remember paused allowance");
    sqlite3_finalize(statement);
}

void Store::clear_paused() {
    exec(impl_->db, "UPDATE runtime_state SET paused_task_id=NULL,paused_allowance_id=NULL WHERE id=1");
}

std::string Store::timezone() const {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db, "SELECT value FROM app_settings WHERE key='timezone'", -1,
        &statement, nullptr), impl_->db, "load timezone");
    std::string value = "UTC";
    if (sqlite3_step(statement) == SQLITE_ROW)
        value = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
    return value;
}

void Store::set_timezone(const std::string& timezone_value) {
    set_app_timezone(timezone_value);
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO app_settings(key,value) VALUES('timezone',?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value", -1, &statement, nullptr),
        impl_->db, "prepare timezone setting");
    sqlite3_bind_text(statement, 1, timezone_value.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement), impl_->db, "save timezone");
    sqlite3_finalize(statement);
}

int Store::add_session(int task_id, TimePoint started_at, TimePoint ended_at) {
    if (ended_at <= started_at) throw std::runtime_error("Session end must follow its start");
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "INSERT INTO sessions(task_id,started_at,ended_at) VALUES(?,?,?)", -1, &statement, nullptr),
        impl_->db, "prepare manual session");
    sqlite3_bind_int(statement, 1, task_id);
    sqlite3_bind_int64(statement, 2, epoch(started_at));
    sqlite3_bind_int64(statement, 3, epoch(ended_at));
    check(sqlite3_step(statement), impl_->db, "add manual session");
    sqlite3_finalize(statement);
    return static_cast<int>(sqlite3_last_insert_rowid(impl_->db));
}

void Store::update_session(int session_id, TimePoint started_at, TimePoint ended_at) {
    if (ended_at <= started_at) throw std::runtime_error("Session end must follow its start");
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE sessions SET started_at=?,ended_at=? WHERE id=? AND ended_at IS NOT NULL", -1,
        &statement, nullptr), impl_->db, "prepare session correction");
    sqlite3_bind_int64(statement, 1, epoch(started_at));
    sqlite3_bind_int64(statement, 2, epoch(ended_at));
    sqlite3_bind_int(statement, 3, session_id);
    check(sqlite3_step(statement), impl_->db, "correct session");
    sqlite3_finalize(statement);
}

void Store::update_session_start(int session_id, TimePoint started_at) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db,
        "UPDATE sessions SET started_at=? WHERE id=? AND (ended_at IS NULL OR ended_at>?)", -1,
        &statement, nullptr), impl_->db, "prepare update session start");
    sqlite3_bind_int64(statement, 1, epoch(started_at));
    sqlite3_bind_int(statement, 2, session_id);
    sqlite3_bind_int64(statement, 3, epoch(started_at));
    check(sqlite3_step(statement), impl_->db, "update session start");
    if (sqlite3_changes(impl_->db) == 0) {
        sqlite3_finalize(statement);
        throw std::runtime_error("Session start must be before its end");
    }
    sqlite3_finalize(statement);
}

void Store::update_session_task(int session_id, int task_id) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db, "UPDATE sessions SET task_id=? WHERE id=?", -1,
        &statement, nullptr), impl_->db, "prepare change tracked task");
    sqlite3_bind_int(statement, 1, task_id);
    sqlite3_bind_int(statement, 2, session_id);
    check(sqlite3_step(statement), impl_->db, "change tracked task");
    sqlite3_finalize(statement);
}

bool Store::empty() const {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(impl_->db, "SELECT COUNT(*) FROM projects", -1, &statement, nullptr),
          impl_->db, "count projects");
    const bool result = sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 0;
    sqlite3_finalize(statement);
    return result;
}

void Store::seed_demo() {
    if (!empty()) return;
    const int reset = add_project("Personal reset");
    const int jobs = add_project("Job search");
    add_project("Learn C++");
    const int paper = add_project("Research paper");
    Project paper_project{paper, "Research paper", "pages", {}, {}};
    update_project(paper_project);
    add_task(reset, "Sort papers", Status::Backlog);
    auto task_id = add_task(reset, "Update CV", Status::Ready);
    const int call = add_task(reset, "Call adviser", Status::Doing);
    const int plan = add_task(reset, "Submit weekly plan", Status::Ready);
    const int walk = add_task(reset, "Lunch and walk", Status::Ready);
    const int review = add_task(reset, "Daily review", Status::Ready);
    add_task(reset, "Set up calendar", Status::Done);
    add_task(jobs, "Find three suitable roles", Status::Ready);
    add_task(jobs, "Draft reusable cover letter", Status::Backlog);
    const int writing = add_task(paper, "Write literature review", Status::Doing);
    State state = load(Clock::now());
    auto it = std::find_if(state.tasks.begin(), state.tasks.end(), [task_id](const Task& task) {
        return task.id == task_id;
    });
    if (it != state.tasks.end()) {
        it->due_date = date_iso(Clock::now() + std::chrono::hours(48));
        update_task(*it);
    }
    state = load(Clock::now());
    auto plan_it = std::find_if(state.tasks.begin(), state.tasks.end(), [plan](const Task& task) {
        return task.id == plan;
    });
    if (plan_it != state.tasks.end()) {
        plan_it->due_date = date_iso(Clock::now());
        update_task(*plan_it);
    }
    auto writing_it = std::find_if(state.tasks.begin(), state.tasks.end(), [writing](const Task& task) {
        return task.id == writing;
    });
    if (writing_it != state.tasks.end()) {
        writing_it->progress_done = 2;
        writing_it->progress_target = 8;
        update_task(*writing_it);
    }
    add_schedule(plan, date_iso(Clock::now()), "09:00", 30);
    add_schedule(call, date_iso(Clock::now()), "10:00", 30);
    add_schedule(walk, date_iso(Clock::now()), "12:30", 45);
    add_schedule(writing, date_iso(Clock::now()), "14:00", 60);
    add_schedule(review, date_iso(Clock::now()), "16:30", 20);
    add_allowance("YouTube", 30, "daily");
    add_allowance("Gaming", 180, "weekly");
}

}  // namespace planner
