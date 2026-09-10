#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace planner {

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

enum class Status { Backlog, Ready, Doing, Done };
enum class View { Board, Calendar, Task, EditTask, TaskChecklist, Tracker, EditSession, SessionTaskPicker,
                  Allowances, Todos, Deadlines, Goals, EditGoal, Gantt, Settings, Menu, Help };

struct Project {
    int id{};
    std::string name;
    std::string unit_name{"points"};
    std::string plan_start;
    std::string plan_end;
};

struct Task {
    int id{};
    int project_id{};
    std::string title;
    std::string notes;
    std::string due_date;
    std::string tags;
    int estimate_minutes{};
    Status status{Status::Backlog};
    int position{};
    long long tracked_seconds{};
    int progress_done{};
    int progress_target{};
    bool archived{};
};

struct Session {
    int id{};
    int task_id{};
    TimePoint started_at{};
    std::optional<TimePoint> ended_at;
};

struct Allowance {
    int id{};
    std::string name;
    int budget_minutes{};
    std::string period{"daily"};
};

struct AllowanceSession {
    int id{};
    int allowance_id{};
    TimePoint started_at{};
    std::optional<TimePoint> ended_at;
};

struct ScheduleBlock {
    int id{};
    int task_id{};
    std::string date;
    std::string start_time;
    int duration_minutes{};
};

struct ChecklistItem {
    int id{};
    int task_id{};
    std::string title;
    bool completed{};
    int position{};
};

struct Todo {
    int id{};
    std::string title;
    std::string date;
    bool completed{};
    int position{};
    std::string list_name{"today"};
    bool recurring{};
    std::string end_date;
    int series_id{};
};

struct TodoCompletion { int todo_id{}; std::string date; };
struct TodoException { int todo_id{}; std::string date; std::string title; bool hidden{}; };

struct Goal {
    int id{};
    std::string title;
    std::string unit{"units"};
    double target{};
    double daily_goal{};
    std::string start_date;
    std::string target_date;
    std::string comment;
};

struct GoalEntry {
    int id{};
    int goal_id{};
    std::string date;
    double amount{};
    std::string note;
};

struct State {
    std::vector<Project> projects;
    std::vector<Task> tasks;
    std::vector<ChecklistItem> checklist_items;
    std::vector<ScheduleBlock> schedule;
    std::vector<Session> sessions;
    std::vector<Allowance> allowances;
    std::vector<AllowanceSession> allowance_sessions;
    std::vector<Todo> todos;
    std::vector<TodoCompletion> todo_completions;
    std::vector<TodoException> todo_exceptions;
    std::vector<Goal> goals;
    std::vector<GoalEntry> goal_entries;
    int project_index{};
    int column{};
    int row{};
    std::array<int, 4> board_scroll{};
    int schedule_row{};
    bool today_focused{};
    bool project_focused{};
    bool all_projects{};
    int tracker_row{};
    int session_edit_row{};
    int task_picker_row{};
    std::optional<int> editing_session_id;
    TimePoint tracker_date{now};
    bool calendar_selecting_tracker{};
    bool calendar_selecting_task_due{};
    bool calendar_selecting_todo{};
    std::optional<int> calendar_rescheduling_todo_id;
    std::optional<int> calendar_rescheduling_task_id;
    int calendar_selecting_session_date{}; // 0 none, 1 start, 2 end
    int menu_row{};
    int edit_row{};
    int checklist_row{};
    int allowance_row{};
    int todo_row{};
    int todo_list{};
    bool todo_date_focused{};
    int deadline_row{};
    int goal_row{};
    int goal_edit_row{};
    std::optional<int> editing_goal_id;
    int gantt_row{};
    int gantt_scale{1}; // 0 week, 1 month, 2 year
    int gantt_filter{}; // 0 all, 1 projects, 2 goals
    TimePoint gantt_date{now};
    std::string timezone{"UTC"};
    bool dragging{};
    std::optional<int> dragged_task_id;
    View previous_view{View::Board};
    View view{View::Board};
    std::optional<int> timer_task_id;
    std::optional<int> paused_task_id;
    std::optional<TimePoint> timer_started_at;
    std::optional<int> active_allowance_id;
    std::optional<int> paused_allowance_id;
    std::optional<TimePoint> allowance_started_at;
    std::string message;
    TimePoint now{Clock::now()};
    TimePoint calendar_date{now};
    TimePoint todo_date{now};
};

std::string status_name(Status status);
Status next_status(Status status);
std::vector<const Task*> visible_tasks(const State& state, Status status);
const Project* selected_project(const State& state);
Project* selected_project(State& state);
const Task* selected_task(const State& state);
Task* selected_task(State& state);
long long active_seconds(const State& state, int task_id);
std::string duration(long long seconds);
std::string date_iso(TimePoint point);
std::string date_human(TimePoint point);
TimePoint parse_time(const std::string& value);
void set_app_timezone(const std::string& timezone);
bool todo_visible_on(const State& state, const Todo& todo, const std::string& date);
std::string todo_title_on(const State& state, const Todo& todo, const std::string& date);

class Store {
  public:
    explicit Store(const std::string& path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    void initialize();
    State load(TimePoint now);
    int add_project(const std::string& name);
    int add_task(int project_id, const std::string& title, Status status);
    void update_project(const Project& project);
    void update_task(const Task& task);
    int add_checklist_item(int task_id, const std::string& title);
    void update_checklist_item(const ChecklistItem& item);
    void move_checklist_item(int item_id, int position);
    void delete_checklist_item(int item_id);
    void move_task(int task_id, Status status, int position);
    void move_task_to_project(int task_id, int project_id);
    void archive_task(int task_id);
    void delete_task(int task_id);
    void checkpoint();
    bool undo();
    bool redo();
    int add_schedule(int task_id, const std::string& date, const std::string& start_time,
                     int duration_minutes);
    void start_timer(int task_id, TimePoint now);
    void stop_timer(TimePoint now);
    int add_session(int task_id, TimePoint started_at, TimePoint ended_at);
    void update_session(int session_id, TimePoint started_at, TimePoint ended_at);
    void update_session_start(int session_id, TimePoint started_at);
    void update_session_task(int session_id, int task_id);
    int add_allowance(const std::string& name, int budget_minutes, const std::string& period);
    void update_allowance(const Allowance& allowance);
    void start_allowance(int allowance_id, TimePoint now);
    void delete_allowance(int allowance_id);
    int add_todo(const std::string& title, const std::string& date,
                 const std::string& list_name = "today");
    void update_todo(const Todo& todo);
    void set_todo_completed(int todo_id, const std::string& date, bool completed);
    void delete_todo(int todo_id);
    void hide_todo_on(int todo_id, const std::string& date);
    void rename_todo_on(int todo_id, const std::string& date, const std::string& title);
    void rename_todo_all(int todo_id, const std::string& title);
    int split_todo_from(int todo_id, const std::string& date, const std::string& new_title);
    void end_todo_before(int todo_id, const std::string& date);
    int add_goal(const std::string& title, const std::string& unit, double target,
                 const std::string& start_date, const std::string& target_date,
                 double daily_goal = 0, const std::string& comment = {});
    void update_goal(const Goal& goal);
    void delete_goal(int goal_id);
    int add_goal_entry(int goal_id, const std::string& date, double amount, const std::string& note);
    void clear_goal_entries_on(int goal_id, const std::string& date);
    void remember_paused_task(int task_id);
    void remember_paused_allowance(int allowance_id);
    void clear_paused();
    std::string timezone() const;
    void set_timezone(const std::string& timezone);
    bool empty() const;
    void seed_demo();

  private:
    struct Impl;
    Impl* impl_;
};

}  // namespace planner
