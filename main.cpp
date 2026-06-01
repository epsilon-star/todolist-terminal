/*
 * todo.cpp — Terminal Task Manager v2.0
 * Compile:  g++ -std=c++17 -o todo todo.cpp
 * Run:      ./todo
 *
 * Data path: $TODO_DATA env var, or ~/.config/todo/tasks.dat by default
 * Config:    ~/.config/todo/config.dat
 *
 * File format:
 *   PROJECT|<id>|<name>
 *   TASK|<id>|<projectId>|<done>|<priority>|<due>|<tags>|<minutes>|<text>
 *   CONFIG|<key>|<value>
 */

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <optional>
#include <ctime>
#include <map>
#include <set>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

// ── ANSI colours ─────────────────────────────────────────────────────────────

const std::string RESET   = "\033[0m";
const std::string BOLD    = "\033[1m";
const std::string DIM     = "\033[2m";
const std::string RED     = "\033[31m";
const std::string GREEN   = "\033[32m";
const std::string YELLOW  = "\033[33m";
const std::string CYAN    = "\033[36m";
const std::string MAGENTA = "\033[35m";
const std::string BLUE    = "\033[34m";
const std::string WHITE   = "\033[37m";
const std::string BG_RED  = "\033[41m";

// ── Data structures ──────────────────────────────────────────────────────────

struct Project {
    int         id;
    std::string name;
};

struct Task {
    int         id;
    int         projectId;    // 0 = inbox
    std::string text;
    std::string priority;     // low / medium / high
    bool        done;
    std::string due;          // "YYYY-MM-DD" or ""
    std::string tags;         // space-separated e.g. "#work #waiting"
    int         minutes = 0;  // total tracked minutes
    // runtime-only (not saved)
    bool        timerRunning = false;
    time_t      timerStart   = 0;
};

struct Config {
    std::string dataPath;
    std::string dateFormat = "YYYY-MM-DD";
    std::string defaultPriority = "medium";
    bool        showDone = true;
    bool        confirmDelete = true;
};

// ── Globals ───────────────────────────────────────────────────────────────────

Config gConfig;
std::string gConfigPath;
std::optional<Task> gUndoTask;   // last deleted task for undo

// ── Path helpers ──────────────────────────────────────────────────────────────

std::string getHomeDir() {
    const char* home = getenv("HOME");
    if (home) return std::string(home);
    struct passwd* pw = getpwuid(getuid());
    return pw ? std::string(pw->pw_dir) : ".";
}

void mkdirP(const std::string& path) {
    // create directory and parents if needed
    std::string tmp = path;
    for (size_t i = 1; i < tmp.size(); i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp.c_str(), 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp.c_str(), 0755);
}

std::string defaultDataPath() {
    const char* env = getenv("TODO_DATA");
    if (env) return std::string(env);
    return getHomeDir() + "/.config/todo/tasks.dat";
}

std::string defaultConfigPath() {
    return getHomeDir() + "/.config/todo/config.dat";
}

// ── Date helpers ──────────────────────────────────────────────────────────────

std::string todayStr() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    return std::string(buf);
}

// returns -1 = overdue, 0 = today, 1 = future, 2 = no due date
int dueDaysDiff(const std::string& due) {
    if (due.empty()) return 2;
    std::string today = todayStr();
    if (due < today) return -1;
    if (due == today) return 0;
    return 1;
}

std::string dueLabel(const std::string& due, bool done) {
    if (due.empty()) return "";
    int diff = dueDaysDiff(due);
    if (done) return DIM + " [" + due + "]" + RESET;
    if (diff == -1) return RED  + BOLD + " [OVERDUE:" + due + "]" + RESET;
    if (diff == 0)  return YELLOW + " [TODAY]" + RESET;
    return DIM + " [" + due + "]" + RESET;
}

bool isValidDate(const std::string& s) {
    if (s.size() != 10) return false;
    if (s[4] != '-' || s[7] != '-') return false;
    for (int i : {0,1,2,3,5,6,8,9})
        if (!isdigit(s[i])) return false;
    return true;
}

// ── Config persistence ────────────────────────────────────────────────────────

void saveConfig() {
    mkdirP(gConfigPath.substr(0, gConfigPath.rfind('/')));
    std::ofstream f(gConfigPath);
    f << "CONFIG|dataPath|" << gConfig.dataPath << "\n";
    f << "CONFIG|defaultPriority|" << gConfig.defaultPriority << "\n";
    f << "CONFIG|showDone|" << gConfig.showDone << "\n";
    f << "CONFIG|confirmDelete|" << gConfig.confirmDelete << "\n";
}

void loadConfig() {
    gConfigPath = defaultConfigPath();
    gConfig.dataPath = defaultDataPath();

    std::ifstream f(gConfigPath);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string kind, key, val;
        std::getline(ss, kind, '|');
        std::getline(ss, key, '|');
        std::getline(ss, val);
        if (kind != "CONFIG") continue;
        if (key == "dataPath")        gConfig.dataPath = val;
        if (key == "defaultPriority") gConfig.defaultPriority = val;
        if (key == "showDone")        gConfig.showDone = (val == "1");
        if (key == "confirmDelete")   gConfig.confirmDelete = (val == "1");
    }
}

// ── Task persistence ──────────────────────────────────────────────────────────

void saveAll(const std::vector<Project>& projects, const std::vector<Task>& tasks) {
    mkdirP(gConfig.dataPath.substr(0, gConfig.dataPath.rfind('/')));
    std::ofstream f(gConfig.dataPath);
    for (const auto& p : projects)
        f << "PROJECT|" << p.id << "|" << p.name << "\n";
    for (const auto& t : tasks)
        f << "TASK|" << t.id << "|" << t.projectId << "|"
          << t.done << "|" << t.priority << "|"
          << t.due << "|" << t.tags << "|" << t.minutes << "|"
          << t.text << "\n";
}

void loadAll(std::vector<Project>& projects, std::vector<Task>& tasks) {
    std::ifstream f(gConfig.dataPath);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string kind;
        std::getline(ss, kind, '|');

        if (kind == "PROJECT") {
            Project p;
            std::string idStr;
            std::getline(ss, idStr, '|'); p.id = std::stoi(idStr);
            std::getline(ss, p.name);
            projects.push_back(p);

        } else if (kind == "TASK") {
            Task t;
            std::string tmp;
            std::getline(ss, tmp, '|');  t.id        = std::stoi(tmp);
            std::getline(ss, tmp, '|');  t.projectId = std::stoi(tmp);
            std::getline(ss, tmp, '|');  t.done      = (tmp == "1");
            std::getline(ss, t.priority, '|');
            std::getline(ss, t.due,      '|');
            std::getline(ss, t.tags,     '|');
            std::getline(ss, tmp,        '|');
            t.minutes = tmp.empty() ? 0 : std::stoi(tmp);
            std::getline(ss, t.text);
            tasks.push_back(t);
        }
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

int nextProjectId(const std::vector<Project>& v) {
    int mx = 0; for (const auto& p : v) mx = std::max(mx, p.id); return mx + 1;
}
int nextTaskId(const std::vector<Task>& v) {
    int mx = 0; for (const auto& t : v) mx = std::max(mx, t.id); return mx + 1;
}

std::string priColour(const std::string& p) {
    if (p == "high")   return RED;
    if (p == "medium") return YELLOW;
    return GREEN;
}
std::string priTag(const std::string& p) {
    if (p == "high")   return "[HIGH]";
    if (p == "medium") return "[MED] ";
    return "[LOW] ";
}
std::string parsePriority(const std::string& s) {
    if (s == "l" || s == "low")    return "low";
    if (s == "h" || s == "high")   return "high";
    if (s == "m" || s == "medium") return "medium";
    return gConfig.defaultPriority;
}

const Project* findProject(const std::vector<Project>& projects, int id) {
    for (const auto& p : projects) if (p.id == id) return &p;
    return nullptr;
}

Task* findTask(std::vector<Task>& tasks, int id) {
    for (auto& t : tasks) if (t.id == id) return &t;
    return nullptr;
}

void clearScreen() { std::cout << "\033[2J\033[H"; }

void pressEnter() {
    std::cout << DIM << "\nPress Enter to continue…" << RESET;
    std::cin.ignore(10000, '\n');
    std::cin.get();
}

std::string minutesToHMS(int mins) {
    if (mins <= 0) return "";
    int h = mins / 60, m = mins % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), " ⏱%dh%02dm", h, m);
    else        snprintf(buf, sizeof(buf), " ⏱%dm", m);
    return DIM + std::string(buf) + RESET;
}

// parse tags from text like "#work #waiting"
std::vector<std::string> splitTags(const std::string& tags) {
    std::vector<std::string> result;
    std::istringstream ss(tags);
    std::string tok;
    while (ss >> tok) result.push_back(tok);
    return result;
}

std::string colorTags(const std::string& tags) {
    if (tags.empty()) return "";
    std::string out = " ";
    for (const auto& t : splitTags(tags))
        out += CYAN + t + RESET + " ";
    return out;
}

// ── Display helpers ───────────────────────────────────────────────────────────

void printHeader(const std::string& subtitle = "") {
    std::cout << BOLD << CYAN
              << "╔══════════════════════════════════════════════╗\n"
              << "║      📋  Terminal Task Manager  v2.0         ║\n"
              << "╚══════════════════════════════════════════════╝\n"
              << RESET;
    if (!subtitle.empty())
        std::cout << BOLD << "  " << subtitle << "\n" << RESET;
}

// ── Search ────────────────────────────────────────────────────────────────────

void searchTasks(const std::vector<Task>& tasks,
                 const std::vector<Project>& projects) {
    std::string kw;
    std::cout << "Search keyword: ";
    std::cin.ignore(10000, '\n');
    std::getline(std::cin, kw);
    if (kw.empty()) return;

    // lowercase kw
    std::string kwl = kw;
    std::transform(kwl.begin(), kwl.end(), kwl.begin(), ::tolower);

    clearScreen();
    printHeader("🔍 Search: \"" + kw + "\"");
    std::cout << "\n";

    int found = 0;
    for (const auto& t : tasks) {
        std::string textl = t.text; std::transform(textl.begin(), textl.end(), textl.begin(), ::tolower);
        std::string tagsl = t.tags; std::transform(tagsl.begin(), tagsl.end(), tagsl.begin(), ::tolower);
        if (textl.find(kwl) == std::string::npos &&
            tagsl.find(kwl) == std::string::npos) continue;

        const Project* proj = findProject(projects, t.projectId);
        std::string projName = (t.projectId == 0) ? "Inbox" : (proj ? proj->name : "?");

        std::string status = t.done ? GREEN + "✔" + RESET : " ";
        std::string style  = t.done ? DIM : "";

        std::cout << style
                  << std::setw(4) << t.id << "  "
                  << "[" << status << "]  "
                  << priColour(t.priority) << priTag(t.priority) << RESET
                  << "  " << style << t.text << RESET
                  << dueLabel(t.due, t.done)
                  << colorTags(t.tags)
                  << minutesToHMS(t.minutes)
                  << DIM << "  [" << projName << "]" << RESET
                  << "\n";
        found++;
    }
    if (found == 0)
        std::cout << DIM << "  No tasks matching \"" << kw << "\".\n" << RESET;
    else
        std::cout << DIM << "\n  " << found << " result(s).\n" << RESET;

    pressEnter();
}

// ── Export ────────────────────────────────────────────────────────────────────

void exportTasks(const std::vector<Task>& tasks,
                 const std::vector<Project>& projects) {
    std::cout << "Export filename [todo_export.md]: ";
    std::cin.ignore(10000, '\n');
    std::string fname; std::getline(std::cin, fname);
    if (fname.empty()) fname = "todo_export.md";

    std::ofstream f(fname);
    if (!f.is_open()) {
        std::cout << RED << "Could not open file for writing.\n" << RESET;
        return;
    }

    std::string today = todayStr();
    f << "# Task Export — " << today << "\n\n";

    // inbox
    f << "## 📥 Inbox\n\n";
    bool anyInbox = false;
    for (const auto& t : tasks) {
        if (t.projectId != 0) continue;
        if (t.done) continue;
        f << "- [" << (t.done ? "x" : " ") << "] "
          << "[" << t.priority << "] "
          << t.text;
        if (!t.due.empty())  f << " (due: " << t.due << ")";
        if (!t.tags.empty()) f << " " << t.tags;
        f << "\n";
        anyInbox = true;
    }
    if (!anyInbox) f << "_No active tasks._\n";
    f << "\n";

    // per project
    for (const auto& p : projects) {
        f << "## 📁 " << p.name << "\n\n";
        bool any = false;
        for (const auto& t : tasks) {
            if (t.projectId != p.id) continue;
            if (t.done) continue;
            f << "- [ ] [" << t.priority << "] " << t.text;
            if (!t.due.empty())  f << " (due: " << t.due << ")";
            if (!t.tags.empty()) f << " " << t.tags;
            if (t.minutes > 0)  f << " ⏱" << t.minutes << "min";
            f << "\n";
            any = true;
        }
        if (!any) f << "_No active tasks._\n";
        f << "\n";
    }

    // stats
    int total = tasks.size(), done = 0;
    for (const auto& t : tasks) if (t.done) done++;
    f << "---\n_Total: " << total << " | Done: " << done
      << " | Active: " << (total - done) << "_\n";

    std::cout << GREEN << "Exported to " << fname << "\n" << RESET;
}

// ── Stats ─────────────────────────────────────────────────────────────────────

void showStats(const std::vector<Task>& tasks,
               const std::vector<Project>& projects) {
    clearScreen();
    printHeader("📊 Statistics");
    std::cout << "\n";

    int total = 0, done = 0, overdue = 0, dueToday = 0;
    int byPri[3] = {0,0,0}; // low, medium, high
    int totalMins = 0;

    // "this week" = last 7 days (approximated by checking done tasks)
    // We don't store completion date, so we track all done
    for (const auto& t : tasks) {
        total++;
        if (t.done) done++;
        if (!t.done) {
            int diff = dueDaysDiff(t.due);
            if (diff == -1) overdue++;
            if (diff == 0)  dueToday++;
        }
        if (t.priority == "low")    byPri[0]++;
        if (t.priority == "medium") byPri[1]++;
        if (t.priority == "high")   byPri[2]++;
        totalMins += t.minutes;
    }

    // int active = total - done;
    int pct = (total > 0) ? (done * 100 / total) : 0;

    // overall bar
    int filled = pct / 5; // 20-char bar
    std::string bar = "[";
    for (int i = 0; i < 20; i++) bar += (i < filled ? "█" : "░");
    bar += "]";

    std::cout << BOLD << "  Overall completion\n" << RESET;
    std::cout << "  " << GREEN << bar << " " << pct << "%" << RESET
              << DIM << "  (" << done << "/" << total << " tasks)\n\n" << RESET;

    std::cout << BOLD << "  Priority breakdown\n" << RESET;
    std::cout << "  " << RED    << "[HIGH]  " << RESET << byPri[2] << " tasks\n";
    std::cout << "  " << YELLOW << "[MED]   " << RESET << byPri[1] << " tasks\n";
    std::cout << "  " << GREEN  << "[LOW]   " << RESET << byPri[0] << " tasks\n\n";

    std::cout << BOLD << "  Due dates\n" << RESET;
    if (overdue  > 0) std::cout << "  " << RED    << "⚠ Overdue:   " << overdue  << RESET << "\n";
    if (dueToday > 0) std::cout << "  " << YELLOW << "● Due today: " << dueToday << RESET << "\n";
    if (overdue == 0 && dueToday == 0)
        std::cout << "  " << DIM << "No overdue or due-today tasks.\n" << RESET;
    std::cout << "\n";

    if (totalMins > 0) {
        std::cout << BOLD << "  Time tracked\n" << RESET;
        std::cout << "  Total: " << CYAN << (totalMins / 60) << "h "
                  << (totalMins % 60) << "m" << RESET << "\n\n";
    }

    std::cout << BOLD << "  Per project\n" << RESET;
    // inbox
    {
        int t2 = 0, d2 = 0;
        for (const auto& t : tasks) if (t.projectId == 0) { t2++; if (t.done) d2++; }
        int p2 = t2 > 0 ? d2 * 100 / t2 : 0;
        std::cout << "  " << std::setw(20) << std::left << "Inbox"
                  << DIM << "  " << (t2-d2) << " active / " << t2 << " total  "
                  << p2 << "%\n" << RESET;
    }
    for (const auto& p : projects) {
        int t2 = 0, d2 = 0;
        for (const auto& t : tasks) if (t.projectId == p.id) { t2++; if (t.done) d2++; }
        int p2 = t2 > 0 ? d2 * 100 / t2 : 0;
        std::cout << "  " << std::setw(20) << std::left << p.name
                  << DIM << "  " << (t2-d2) << " active / " << t2 << " total  "
                  << p2 << "%\n" << RESET;
    }

    pressEnter();
}

// ══════════════════════════════════════════════════════════════════════════════
// PROJECT SCREEN
// ══════════════════════════════════════════════════════════════════════════════

void printProjectList(const std::vector<Project>& projects,
                      const std::vector<Task>&    tasks) {
    std::cout << "\n";

    // helper lambda for stats
    auto printRow = [&](int projId, const std::string& label, const std::string& icon) {
        int total = 0, done = 0, overdue = 0;
        for (const auto& t : tasks) {
            if (t.projectId != projId) continue;
            total++;
            if (t.done) done++;
            if (!t.done && dueDaysDiff(t.due) == -1) overdue++;
        }
        int pct = (total > 0) ? (done * 100 / total) : 0;
        int filled = pct / 10;
        std::string bar = "[";
        for (int i = 0; i < 10; i++) bar += (i < filled ? "█" : "░");
        bar += "]";

        std::cout << "  " << std::setw(3) << projId << "  "
                  << BOLD << icon << " " << label << RESET
                  << DIM << "  " << bar << " " << pct << "%"
                  << "  (" << (total-done) << " active / " << total << " total)";
        if (overdue > 0)
            std::cout << "  " << RED << "⚠ " << overdue << " overdue" << RESET;
        std::cout << "\n";
    };

    printRow(0, "Inbox", "📥");
    for (const auto& p : projects)
        printRow(p.id, p.name, "📁");

    if (projects.empty())
        std::cout << DIM << "\n  No projects yet. Press 'n' to create one.\n" << RESET;
}

void printProjectMenu() {
    std::cout << BOLD << "\n── Commands ──────────────────────────────────────\n" << RESET
              << "  " << CYAN << "<id>" << RESET << "  Open project / inbox\n"
              << "  " << CYAN << "n"    << RESET << "    New project\n"
              << "  " << CYAN << "R"    << RESET << "    Rename project\n"
              << "  " << CYAN << "X"    << RESET << "    Delete project (tasks → inbox)\n"
              << "  " << CYAN << "/"    << RESET << "    Search all tasks\n"
              << "  " << CYAN << "E"    << RESET << "    Export to markdown\n"
              << "  " << CYAN << "S"    << RESET << "    Statistics\n"
              << "  " << CYAN << "O"    << RESET << "    Options / settings\n"
              << "  " << CYAN << "q"    << RESET << "    Quit\n"
              << "──────────────────────────────────────────────────\n"
              << BOLD << "Enter command: " << RESET;
}

// ══════════════════════════════════════════════════════════════════════════════
// TASK SCREEN
// ══════════════════════════════════════════════════════════════════════════════

void printTaskList(const std::vector<Task>& tasks,
                   int projectId,
                   const std::string& filter,
                   const std::string& sortMode) {
    std::cout << "\n";

    // gather matching tasks
    std::vector<const Task*> shown;
    int total = 0, done = 0, high = 0;

    for (const auto& t : tasks) {
        if (t.projectId != projectId) continue;
        total++;
        if (t.done) done++;
        if (t.priority == "high" && !t.done) high++;

        if (filter == "active" && t.done)  continue;
        if (filter == "done"   && !t.done) continue;
        if (filter == "high"   && (t.priority != "high" || t.done)) continue;
        if (filter == "today"  && !(dueDaysDiff(t.due) == 0 || dueDaysDiff(t.due) == -1)) continue;
        if (filter.size() > 0 && filter[0] == '#') {
            // tag filter
            if (t.tags.find(filter) == std::string::npos) continue;
        }
        if (!gConfig.showDone && t.done) continue;

        shown.push_back(&t);
    }

    // sort
    if (sortMode == "priority") {
        std::sort(shown.begin(), shown.end(), [](const Task* a, const Task* b) {
            auto rank = [](const std::string& p) {
                if (p == "high")   return 0;
                if (p == "medium") return 1;
                return 2;
            };
            return rank(a->priority) < rank(b->priority);
        });
    } else if (sortMode == "due") {
        std::sort(shown.begin(), shown.end(), [](const Task* a, const Task* b) {
            std::string da = a->due.empty() ? "9999-99-99" : a->due;
            std::string db = b->due.empty() ? "9999-99-99" : b->due;
            return da < db;
        });
    }
    // default: id order (insertion order)

    for (const Task* t : shown) {
        std::string status = t->done ? GREEN + "✔" + RESET : " ";
        std::string style  = t->done ? DIM : "";
        bool timerOn = t->timerRunning;

        std::cout << style
                  << std::setw(4) << t->id << "  "
                  << "[" << status << "]  "
                  << priColour(t->priority) << priTag(t->priority) << RESET
                  << "  " << style << t->text << RESET
                  << dueLabel(t->due, t->done)
                  << colorTags(t->tags)
                  << minutesToHMS(t->minutes)
                  << (timerOn ? YELLOW + " ▶TIMER" + RESET : "")
                  << "\n";
    }

    if (shown.empty()) std::cout << DIM << "  (no tasks to show)\n" << RESET;

    std::cout << DIM
              << "\n  Total: " << total
              << "  |  Active: " << (total-done)
              << "  |  Done: " << done
              << "  |  High: " << high
              << "\n" << RESET;
}

void printTaskMenu(const std::string& filter, const std::string& sortMode) {
    std::cout << BOLD << "\n── Commands ──────────────────────────────────────\n" << RESET
              << "  " << CYAN << "a" << RESET << "  Add task\n"
              << "  " << CYAN << "d" << RESET << "  Done / undone (toggle)\n"
              << "  " << CYAN << "r" << RESET << "  Remove task\n"
              << "  " << CYAN << "e" << RESET << "  Edit task text\n"
              << "  " << CYAN << "p" << RESET << "  Change priority\n"
              << "  " << CYAN << "D" << RESET << "  Set / clear due date\n"
              << "  " << CYAN << "T" << RESET << "  Edit tags\n"
              << "  " << CYAN << "m" << RESET << "  Move task to another project\n"
              << "  " << CYAN << "t" << RESET << "  Timer start/stop\n"
              << "  " << CYAN << "B" << RESET << "  Bulk done (e.g. 1 3 5)\n"
              << "  " << CYAN << "f" << RESET << "  Filter  [" << filter << "]\n"
              << "  " << CYAN << "o" << RESET << "  Sort    [" << sortMode << "]\n"
              << "  " << CYAN << "c" << RESET << "  Clear done tasks\n"
              << "  " << CYAN << "u" << RESET << "  Undo last delete\n"
              << "  " << CYAN << "b" << RESET << "  Back to projects\n"
              << "──────────────────────────────────────────────────\n"
              << BOLD << "Enter command: " << RESET;
}

// ── Task actions ──────────────────────────────────────────────────────────────

void addTask(std::vector<Task>& tasks,
             std::vector<Project>& projects,
             int projectId) {
    std::string text, pri, due, tags;
    std::cout << "Task description: ";
    std::cin.ignore(10000, '\n');
    std::getline(std::cin, text);
    if (text.empty()) { std::cout << RED << "Empty — cancelled.\n" << RESET; return; }

    std::cout << "Priority (l/m/h) [" << gConfig.defaultPriority[0] << "]: ";
    std::getline(std::cin, pri);

    std::cout << "Due date (YYYY-MM-DD or blank): ";
    std::getline(std::cin, due);
    if (!due.empty() && !isValidDate(due)) {
        std::cout << YELLOW << "Invalid date format, skipping.\n" << RESET;
        due = "";
    }

    std::cout << "Tags (e.g. #work #waiting or blank): ";
    std::getline(std::cin, tags);

    tasks.push_back({nextTaskId(tasks), projectId, text, parsePriority(pri),
                     false, due, tags, 0, false, 0});
    saveAll(projects, tasks);
    std::cout << GREEN << "Task added.\n" << RESET;
}

void toggleDone(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << RED << "ID not found.\n" << RESET; return; }
    // stop timer if running
    if (t->timerRunning) {
        t->minutes += (int)difftime(time(nullptr), t->timerStart) / 60;
        t->timerRunning = false;
    }
    t->done = !t->done;
    saveAll(projects, tasks);
    std::cout << GREEN << (t->done ? "Marked done.\n" : "Marked active.\n") << RESET;
}

void bulkDone(std::vector<Task>& tasks, std::vector<Project>& projects) {
    std::cout << "Task IDs (space-separated): ";
    std::cin.ignore(10000, '\n');
    std::string line; std::getline(std::cin, line);
    std::istringstream ss(line);
    int id, count = 0;
    while (ss >> id) {
        Task* t = findTask(tasks, id);
        if (t && !t->done) { t->done = true; count++; }
    }
    saveAll(projects, tasks);
    std::cout << GREEN << count << " task(s) marked done.\n" << RESET;
}

void removeTask(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID to remove: "; std::cin >> id;

    if (gConfig.confirmDelete) {
        Task* t = findTask(tasks, id);
        if (!t) { std::cout << RED << "ID not found.\n" << RESET; return; }
        std::cout << "  Delete: \"" << t->text << "\"? (y/N) ";
        char c; std::cin >> c;
        if (c != 'y' && c != 'Y') {
            std::cout << DIM << "Cancelled.\n" << RESET; return;
        }
    }

    for (auto it = tasks.begin(); it != tasks.end(); ++it) {
        if (it->id == id) {
            gUndoTask = *it;   // save for undo
            tasks.erase(it);
            saveAll(projects, tasks);
            std::cout << GREEN << "Task removed. (press 'u' to undo)\n" << RESET;
            return;
        }
    }
    std::cout << RED << "ID not found.\n" << RESET;
}

void undoDelete(std::vector<Task>& tasks, std::vector<Project>& projects) {
    if (!gUndoTask) {
        std::cout << YELLOW << "Nothing to undo.\n" << RESET; return;
    }
    // give it a fresh ID to avoid conflicts
    gUndoTask->id = nextTaskId(tasks);
    tasks.push_back(*gUndoTask);
    gUndoTask.reset();
    saveAll(projects, tasks);
    std::cout << GREEN << "Task restored.\n" << RESET;
}

void editTask(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID to edit: "; std::cin >> id;
    std::cin.ignore(10000, '\n');
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << RED << "ID not found.\n" << RESET; return; }
    std::cout << "New text [" << t->text << "]: ";
    std::string s; std::getline(std::cin, s);
    if (!s.empty()) t->text = s;
    saveAll(projects, tasks);
    std::cout << GREEN << "Updated.\n" << RESET;
}

void changePriority(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    std::cin.ignore(10000, '\n');
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << RED << "ID not found.\n" << RESET; return; }
    std::string s;
    std::cout << "New priority (l/m/h): "; std::getline(std::cin, s);
    t->priority = parsePriority(s);
    saveAll(projects, tasks);
    std::cout << GREEN << "Priority updated.\n" << RESET;
}

void setDueDate(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    std::cin.ignore(10000, '\n');
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << RED << "ID not found.\n" << RESET; return; }
    std::cout << "Due date (YYYY-MM-DD, or blank to clear) [" << t->due << "]: ";
    std::string s; std::getline(std::cin, s);
    if (s.empty()) {
        t->due = "";
        std::cout << GREEN << "Due date cleared.\n" << RESET;
    } else if (isValidDate(s)) {
        t->due = s;
        std::cout << GREEN << "Due date set to " << s << ".\n" << RESET;
    } else {
        std::cout << RED << "Invalid format (need YYYY-MM-DD).\n" << RESET;
        return;
    }
    saveAll(projects, tasks);
}

void editTags(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    std::cin.ignore(10000, '\n');
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << RED << "ID not found.\n" << RESET; return; }
    std::cout << "Tags (e.g. #work #waiting, blank to clear) [" << t->tags << "]: ";
    std::string s; std::getline(std::cin, s);
    t->tags = s;
    saveAll(projects, tasks);
    std::cout << GREEN << "Tags updated.\n" << RESET;
}

void timerToggle(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << RED << "ID not found.\n" << RESET; return; }
    if (t->timerRunning) {
        int elapsed = (int)difftime(time(nullptr), t->timerStart) / 60;
        t->minutes += elapsed;
        t->timerRunning = false;
        saveAll(projects, tasks);
        std::cout << GREEN << "Timer stopped. +" << elapsed << "m  (total: "
                  << t->minutes << "m)\n" << RESET;
    } else {
        t->timerRunning = true;
        t->timerStart   = time(nullptr);
        std::cout << GREEN << "Timer started for task " << id << ".\n" << RESET;
    }
}

void moveTask(std::vector<Task>& tasks, const std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID to move: "; std::cin >> id;
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << RED << "ID not found.\n" << RESET; return; }

    std::cout << "\nAvailable projects:\n";
    std::cout << DIM << "  0  📥 Inbox\n" << RESET;
    for (const auto& p : projects)
        std::cout << "  " << p.id << "  📁 " << p.name << "\n";

    int dest;
    std::cout << "Move to project ID: "; std::cin >> dest;
    if (dest != 0 && !findProject(projects, dest)) {
        std::cout << RED << "Project not found.\n" << RESET; return;
    }
    t->projectId = dest;
    std::cout << GREEN << "Task moved.\n" << RESET;
}

void clearDone(std::vector<Task>& tasks, std::vector<Project>& projects, int projectId) {
    int before = tasks.size();
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
        [projectId](const Task& t){ return t.done && t.projectId == projectId; }),
        tasks.end());
    saveAll(projects, tasks);
    std::cout << GREEN << "Removed " << (before - (int)tasks.size()) << " completed task(s).\n" << RESET;
}

// ── Project actions ───────────────────────────────────────────────────────────

void newProject(std::vector<Project>& projects, std::vector<Task>& tasks) {
    std::string name;
    std::cout << "Project name: ";
    std::cin.ignore(10000, '\n');
    std::getline(std::cin, name);
    if (name.empty()) { std::cout << RED << "Empty name — cancelled.\n" << RESET; return; }
    projects.push_back({nextProjectId(projects), name});
    saveAll(projects, tasks);
    std::cout << GREEN << "Project \"" << name << "\" created.\n" << RESET;
}

void renameProject(std::vector<Project>& projects, std::vector<Task>& tasks) {
    int id;
    std::cout << "Project ID to rename: "; std::cin >> id;
    std::cin.ignore(10000, '\n');
    for (auto& p : projects) {
        if (p.id == id) {
            std::cout << "New name [" << p.name << "]: ";
            std::string s; std::getline(std::cin, s);
            if (!s.empty()) p.name = s;
            saveAll(projects, tasks);
            std::cout << GREEN << "Renamed.\n" << RESET; return;
        }
    }
    std::cout << RED << "Project not found.\n" << RESET;
}

void deleteProject(std::vector<Project>& projects, std::vector<Task>& tasks) {
    int id;
    std::cout << "Project ID to delete: "; std::cin >> id;
    if (id == 0) { std::cout << RED << "Cannot delete Inbox.\n" << RESET; return; }

    auto it = std::remove_if(projects.begin(), projects.end(),
                             [id](const Project& p){ return p.id == id; });
    if (it == projects.end()) { std::cout << RED << "Project not found.\n" << RESET; return; }

    int moved = 0;
    for (auto& t : tasks) if (t.projectId == id) { t.projectId = 0; moved++; }

    projects.erase(it, projects.end());
    saveAll(projects, tasks);
    std::cout << GREEN << "Project deleted. " << moved << " task(s) moved to Inbox.\n" << RESET;
}

// ── Options screen ────────────────────────────────────────────────────────────

void optionsScreen() {
    while (true) {
        clearScreen();
        printHeader("⚙  Options");
        std::cout << "\n"
                  << "  " << CYAN << "1" << RESET << "  Data file path\n"
                  << "       " << DIM << gConfig.dataPath << RESET << "\n\n"
                  << "  " << CYAN << "2" << RESET << "  Default priority\n"
                  << "       " << DIM << gConfig.defaultPriority << RESET << "\n\n"
                  << "  " << CYAN << "3" << RESET << "  Show done tasks in list\n"
                  << "       " << DIM << (gConfig.showDone ? "yes" : "no") << RESET << "\n\n"
                  << "  " << CYAN << "4" << RESET << "  Confirm before delete\n"
                  << "       " << DIM << (gConfig.confirmDelete ? "yes" : "no") << RESET << "\n\n"
                  << "  " << CYAN << "b" << RESET << "  Back\n"
                  << BOLD << "\nEnter option: " << RESET;

        char cmd; std::cin >> cmd;
        std::cin.ignore(10000, '\n');

        if (cmd == '1') {
            std::cout << "New data path [" << gConfig.dataPath << "]: ";
            std::string s; std::getline(std::cin, s);
            if (!s.empty()) gConfig.dataPath = s;
            saveConfig();
            std::cout << GREEN << "Saved. Restart to load from new path.\n" << RESET;
            pressEnter();
        } else if (cmd == '2') {
            std::cout << "Default priority (l/m/h) [" << gConfig.defaultPriority[0] << "]: ";
            std::string s; std::getline(std::cin, s);
            if (!s.empty()) gConfig.defaultPriority = parsePriority(s);
            saveConfig();
            std::cout << GREEN << "Saved.\n" << RESET;
            pressEnter();
        } else if (cmd == '3') {
            gConfig.showDone = !gConfig.showDone;
            saveConfig();
            std::cout << GREEN << "Show done: " << (gConfig.showDone ? "yes" : "no") << "\n" << RESET;
            pressEnter();
        } else if (cmd == '4') {
            gConfig.confirmDelete = !gConfig.confirmDelete;
            saveConfig();
            std::cout << GREEN << "Confirm delete: " << (gConfig.confirmDelete ? "yes" : "no") << "\n" << RESET;
            pressEnter();
        } else if (cmd == 'b') {
            return;
        } else {
            std::cout << RED << "Unknown option.\n" << RESET;
            pressEnter();
        }
    }
}

// ── Task screen loop ──────────────────────────────────────────────────────────

void taskScreen(std::vector<Project>& projects,
                std::vector<Task>& tasks,
                int projectId) {
    std::string filter   = "all";
    std::string sortMode = "id";
    const Project* proj = findProject(projects, projectId);
    std::string title = (projectId == 0)
        ? "📥 Inbox"
        : (proj ? "📁 " + proj->name : "Unknown project");

    while (true) {
        clearScreen();
        printHeader(title);
        std::cout << DIM << "  Filter: " << filter << "   Sort: " << sortMode << "\n" << RESET;
        printTaskList(tasks, projectId, filter, sortMode);
        printTaskMenu(filter, sortMode);

        char cmd; std::cin >> cmd;

        switch (cmd) {
            case 'a': addTask(tasks, projects, projectId);       break;
            case 'd': toggleDone(tasks, projects);               break;
            case 'B': bulkDone(tasks, projects);                 break;
            case 'r': removeTask(tasks, projects);               break;
            case 'u': undoDelete(tasks, projects);               break;
            case 'e': editTask(tasks, projects);                 break;
            case 'p': changePriority(tasks, projects);           break;
            case 'D': setDueDate(tasks, projects);               break;
            case 'T': editTags(tasks, projects);                 break;
            case 't': timerToggle(tasks, projects);              break;
            case 'm':
                moveTask(tasks, projects);
                saveAll(projects, tasks);
                break;
            case 'c': clearDone(tasks, projects, projectId);     break;
            case 'f': {
                std::cout << "Filter (all / active / done / high / today / #tag): ";
                std::cin.ignore(10000, '\n');
                std::getline(std::cin, filter);
                break;
            }
            case 'o': {
                std::cout << "Sort (id / priority / due): ";
                std::cin.ignore(10000, '\n');
                std::getline(std::cin, sortMode);
                break;
            }
            case 'b': return;
            default:  std::cout << RED << "Unknown command.\n" << RESET; break;
        }

        pressEnter();
    }
}

// ── Project screen loop ───────────────────────────────────────────────────────

void projectScreen(std::vector<Project>& projects, std::vector<Task>& tasks) {
    while (true) {
        clearScreen();
        printHeader("Projects");
        printProjectList(projects, tasks);
        printProjectMenu();

        std::string input;
        std::cin >> input;

        bool isNum = !input.empty() &&
                     std::all_of(input.begin(), input.end(), ::isdigit);
        if (isNum) {
            int id = std::stoi(input);
            if (id == 0) { taskScreen(projects, tasks, 0); continue; }
            if (findProject(projects, id)) {
                taskScreen(projects, tasks, id);
            } else {
                std::cout << RED << "Project ID not found.\n" << RESET;
                pressEnter();
            }
            continue;
        }

        char cmd = input[0];
        switch (cmd) {
            case 'n': newProject(projects, tasks);                    break;
            case 'R': renameProject(projects, tasks);                 break;
            case 'X': deleteProject(projects, tasks);                 break;
            case '/': searchTasks(tasks, projects);                   break;
            case 'E': {
                std::cin.ignore(10000, '\n');
                exportTasks(tasks, projects);
                pressEnter();
                continue;
            }
            case 'S': showStats(tasks, projects);                     break;
            case 'O': optionsScreen();                                break;
            case 'q':
                std::cout << CYAN << "\nBye! Stay productive 🚀\n" << RESET;
                return;
            default:
                std::cout << RED << "Unknown command.\n" << RESET;
        }
        pressEnter();
    }
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    loadConfig();

    std::vector<Project> projects;
    std::vector<Task>    tasks;
    loadAll(projects, tasks);

    // show overdue banner if any
    int overdue = 0;
    for (const auto& t : tasks)
        if (!t.done && dueDaysDiff(t.due) == -1) overdue++;

    clearScreen();
    printHeader();
    if (overdue > 0)
        std::cout << BG_RED << BOLD << "  ⚠  " << overdue
                  << " overdue task(s)! Check your projects." << RESET << "\n";
    std::cout << DIM << "  Data: " << gConfig.dataPath << "\n" << RESET;
    std::cout << DIM << "  Today: " << todayStr() << "\n" << RESET;

    pressEnter();

    projectScreen(projects, tasks);
    return 0;
}