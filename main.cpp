/*
 * todo.cpp — Terminal Task Manager v3.0
 * Compile:  g++ -std=c++17 -o todo todo.cpp
 * Run:      ./todo
 *
 * Data path: $TODO_DATA env var, or ~/.config/todo/tasks.dat by default
 * Config:    ~/.config/todo/config.dat
 * Auth:      ~/.config/todo/auth.dat  (hashed PIN)
 *
 * File format:
 *   PROJECT|<id>|<name>
 *   TASK|<id>|<projectId>|<parentId>|<done>|<priority>|<due>|<tags>|<minutes>|<pinned>|<recur>|<text>|||<notes>
 *   CONFIG|<key>|<value>
 *
 * v3 additions:
 *   - PIN login (optional, sha256-ish simple hash)
 *   - Themes (default / solarized / nord / mono)
 *   - Subtasks (parentId field; 0 = top-level)
 *   - Notes field per task (multi-line, stored after |||)
 *   - Recurring tasks (none / daily / weekly / monthly)
 *   - Pinned tasks (always shown first)
 *   - Weekly agenda view (all tasks due in next 7 days)
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
#include <functional>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

// ── Themes ────────────────────────────────────────────────────────────────────

struct Theme {
    std::string name;
    std::string reset, bold, dim;
    std::string red, green, yellow, cyan, magenta, blue, white;
    std::string bgRed;
    std::string accent;   // used for headers / highlights
    std::string success;  // positive feedback
    std::string warn;     // warnings
    std::string err;      // errors
};

// All themes use standard ANSI; "mono" strips most colour.
Theme makeTheme(const std::string& name) {
    Theme t;
    t.name    = name;
    t.reset   = "\033[0m";
    t.bold    = "\033[1m";
    t.dim     = "\033[2m";
    t.bgRed   = "\033[41m";

    if (name == "solarized") {
        t.red     = "\033[31m";
        t.green   = "\033[32m";  // solarized green
        t.yellow  = "\033[33m";
        t.cyan    = "\033[96m";  // bright cyan
        t.magenta = "\033[35m";
        t.blue    = "\033[94m";  // bright blue
        t.white   = "\033[37m";
        t.accent  = "\033[96m";
        t.success = "\033[32m";
        t.warn    = "\033[33m";
        t.err     = "\033[31m";
    } else if (name == "nord") {
        t.red     = "\033[91m";  // bright red
        t.green   = "\033[92m";  // bright green
        t.yellow  = "\033[93m";  // bright yellow
        t.cyan    = "\033[94m";  // nord uses blue/frost tones
        t.magenta = "\033[95m";
        t.blue    = "\033[94m";
        t.white   = "\033[97m";
        t.accent  = "\033[94m";
        t.success = "\033[92m";
        t.warn    = "\033[93m";
        t.err     = "\033[91m";
    } else if (name == "mono") {
        t.red     = "\033[1m";
        t.green   = "";
        t.yellow  = "\033[1m";
        t.cyan    = "\033[1m";
        t.magenta = "\033[1m";
        t.blue    = "";
        t.white   = "";
        t.accent  = "\033[1m";
        t.success = "";
        t.warn    = "\033[1m";
        t.err     = "\033[1m";
    } else { // default
        t.red     = "\033[31m";
        t.green   = "\033[32m";
        t.yellow  = "\033[33m";
        t.cyan    = "\033[36m";
        t.magenta = "\033[35m";
        t.blue    = "\033[34m";
        t.white   = "\033[37m";
        t.accent  = "\033[36m";
        t.success = "\033[32m";
        t.warn    = "\033[33m";
        t.err     = "\033[31m";
    }
    return t;
}

Theme gTheme = makeTheme("default");

// Shortcuts using theme
#define RESET    gTheme.reset
#define BOLD     gTheme.bold
#define DIM      gTheme.dim
#define RED      gTheme.red
#define GREEN    gTheme.green
#define YELLOW   gTheme.yellow
#define CYAN     gTheme.cyan
#define MAGENTA  gTheme.magenta
#define BLUE     gTheme.blue
#define WHITE    gTheme.white
#define BG_RED   gTheme.bgRed
#define ACCENT   gTheme.accent
#define SUCCESS  gTheme.success
#define WARN     gTheme.warn
#define ERR      gTheme.err

// ── Data structures ───────────────────────────────────────────────────────────

struct Project {
    int         id;
    std::string name;
};

struct Task {
    int         id;
    int         projectId;      // 0 = inbox
    int         parentId = 0;   // 0 = top-level task; >0 = subtask of that id
    std::string text;
    std::string priority;       // low / medium / high
    bool        done     = false;
    std::string due;            // "YYYY-MM-DD" or ""
    std::string tags;           // space-separated e.g. "#work #waiting"
    int         minutes  = 0;   // total tracked minutes
    bool        pinned   = false;
    std::string recur;          // "" / "daily" / "weekly" / "monthly"
    std::string notes;          // multi-line notes (newlines stored as \n literal)
    // runtime-only (not saved)
    bool        timerRunning = false;
    time_t      timerStart   = 0;
};

struct Config {
    std::string dataPath;
    std::string authPath;
    std::string dateFormat      = "YYYY-MM-DD";
    std::string defaultPriority = "medium";
    std::string theme           = "default";
    bool        showDone        = true;
    bool        confirmDelete   = true;
    bool        loginEnabled    = false;
};

// ── Globals ───────────────────────────────────────────────────────────────────

Config              gConfig;
std::string         gConfigPath;
std::optional<Task> gUndoTask;

// ── Path helpers ──────────────────────────────────────────────────────────────

std::string getHomeDir() {
    const char* home = getenv("HOME");
    if (home) return std::string(home);
    struct passwd* pw = getpwuid(getuid());
    return pw ? std::string(pw->pw_dir) : ".";
}

void mkdirP(const std::string& path) {
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

std::string defaultAuthPath() {
    return getHomeDir() + "/.config/todo/auth.dat";
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

std::string addDays(const std::string& dateStr, int days) {
    if (dateStr.empty()) return "";
    struct tm t = {};
    sscanf(dateStr.c_str(), "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday);
    t.tm_year -= 1900; t.tm_mon -= 1;
    t.tm_mday += days;
    mktime(&t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return std::string(buf);
}

std::string addMonths(const std::string& dateStr, int months) {
    if (dateStr.empty()) return "";
    struct tm t = {};
    sscanf(dateStr.c_str(), "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday);
    t.tm_year -= 1900; t.tm_mon -= 1;
    t.tm_mon += months;
    mktime(&t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return std::string(buf);
}

// returns -1=overdue, 0=today, N=days ahead, 999=no due
int dueDaysDiff(const std::string& due) {
    if (due.empty()) return 999;
    std::string today = todayStr();
    if (due < today) return -1;
    if (due == today) return 0;
    // compute exact diff for agenda
    struct tm t1 = {}, t2 = {};
    sscanf(today.c_str(), "%d-%d-%d", &t1.tm_year, &t1.tm_mon, &t1.tm_mday);
    sscanf(due.c_str(),   "%d-%d-%d", &t2.tm_year, &t2.tm_mon, &t2.tm_mday);
    t1.tm_year -= 1900; t1.tm_mon -= 1;
    t2.tm_year -= 1900; t2.tm_mon -= 1;
    time_t tt1 = mktime(&t1), tt2 = mktime(&t2);
    return (int)((tt2 - tt1) / 86400);
}

std::string dueLabel(const std::string& due, bool done) {
    if (due.empty()) return "";
    int diff = dueDaysDiff(due);
    if (done) return DIM + " [" + due + "]" + RESET;
    if (diff == -1) return RED + BOLD + " [OVERDUE:" + due + "]" + RESET;
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

// ── Simple hash for PIN ───────────────────────────────────────────────────────
// Not cryptographic — just enough to avoid plain-text storage.
std::string hashPin(const std::string& pin) {
    size_t h = 5381;
    for (char c : pin) h = ((h << 5) + h) + (unsigned char)c;
    // mix a bit more
    h ^= (h >> 16);
    h *= 0x45d9f3b;
    h ^= (h >> 16);
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << h;
    return oss.str();
}

// ── Auth persistence ──────────────────────────────────────────────────────────

void saveAuth(const std::string& pinHash) {
    mkdirP(gConfig.authPath.substr(0, gConfig.authPath.rfind('/')));
    std::ofstream f(gConfig.authPath);
    f << pinHash << "\n";
}

std::string loadAuth() {
    std::ifstream f(gConfig.authPath);
    if (!f.is_open()) return "";
    std::string h; std::getline(f, h);
    return h;
}

// ── Config persistence ────────────────────────────────────────────────────────

void saveConfig() {
    mkdirP(gConfigPath.substr(0, gConfigPath.rfind('/')));
    std::ofstream f(gConfigPath);
    f << "CONFIG|dataPath|"        << gConfig.dataPath        << "\n";
    f << "CONFIG|defaultPriority|" << gConfig.defaultPriority << "\n";
    f << "CONFIG|showDone|"        << gConfig.showDone        << "\n";
    f << "CONFIG|confirmDelete|"   << gConfig.confirmDelete   << "\n";
    f << "CONFIG|theme|"           << gConfig.theme           << "\n";
    f << "CONFIG|loginEnabled|"    << gConfig.loginEnabled    << "\n";
}

void loadConfig() {
    gConfigPath         = defaultConfigPath();
    gConfig.dataPath    = defaultDataPath();
    gConfig.authPath    = defaultAuthPath();

    std::ifstream f(gConfigPath);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string kind, key, val;
        std::getline(ss, kind, '|');
        std::getline(ss, key,  '|');
        std::getline(ss, val);
        if (kind != "CONFIG") continue;
        if (key == "dataPath")        gConfig.dataPath        = val;
        if (key == "defaultPriority") gConfig.defaultPriority = val;
        if (key == "showDone")        gConfig.showDone        = (val == "1");
        if (key == "confirmDelete")   gConfig.confirmDelete   = (val == "1");
        if (key == "theme")           gConfig.theme           = val;
        if (key == "loginEnabled")    gConfig.loginEnabled    = (val == "1");
    }
}

// ── Task persistence ──────────────────────────────────────────────────────────

// Encode newlines in notes for single-line storage
std::string encodeNotes(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else           out += c;
    }
    return out;
}

std::string decodeNotes(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i+1 < s.size() && s[i+1] == 'n') {
            out += '\n'; i++;
        } else {
            out += s[i];
        }
    }
    return out;
}

void saveAll(const std::vector<Project>& projects, const std::vector<Task>& tasks) {
    mkdirP(gConfig.dataPath.substr(0, gConfig.dataPath.rfind('/')));
    std::ofstream f(gConfig.dataPath);
    for (const auto& p : projects)
        f << "PROJECT|" << p.id << "|" << p.name << "\n";
    for (const auto& t : tasks)
        f << "TASK|"
          << t.id        << "|"
          << t.projectId << "|"
          << t.parentId  << "|"
          << t.done      << "|"
          << t.priority  << "|"
          << t.due       << "|"
          << t.tags      << "|"
          << t.minutes   << "|"
          << t.pinned    << "|"
          << t.recur     << "|"
          << t.text      << "|||"
          << encodeNotes(t.notes) << "\n";
}

// Count pipe characters in a raw line, used for v2/v3 format detection

void loadAll(std::vector<Project>& projects, std::vector<Task>& tasks) {
    std::ifstream f(gConfig.dataPath);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;

        // Tokenise the whole line on '|' for safe indexed access
        std::vector<std::string> fields;
        {
            std::istringstream ss2(line);
            std::string tok;
            while (std::getline(ss2, tok, '|')) fields.push_back(tok);
        }
        if (fields.empty()) continue;

        const std::string& kind = fields[0];

        if (kind == "PROJECT" && fields.size() >= 3) {
            Project p;
            try { p.id = std::stoi(fields[1]); } catch (...) { continue; }
            // name may contain '|' — reconstruct from raw line
            size_t nameStart = 0; int pc = 0;
            for (size_t i = 0; i < line.size(); i++) {
                if (line[i] == '|') { pc++; if (pc == 2) { nameStart = i+1; break; } }
            }
            p.name = line.substr(nameStart);
            projects.push_back(p);

        } else if (kind == "TASK" && fields.size() >= 9) {
            Task t;
            try {
                // v3 lines contain "|||" as the text/notes separator.
                // v2 lines never have "|||" (text was the last field).
                bool isV3 = (line.find("|||") != std::string::npos);

                if (isV3) {
                    // v3: TASK|id|projectId|parentId|done|priority|due|tags|minutes|pinned|recur|text|||notes
                    t.id        = std::stoi(fields[1]);
                    t.projectId = std::stoi(fields[2]);
                    t.parentId  = std::stoi(fields[3]);
                    t.done      = (fields[4] == "1");
                    t.priority  = fields[5];
                    t.due       = fields[6];
                    t.tags      = fields[7];
                    t.minutes   = fields[8].empty() ? 0 : std::stoi(fields[8]);
                    t.pinned    = (fields.size() > 9  && fields[9]  == "1");
                    t.recur     =  fields.size() > 10 ? fields[10] : "";
                    // text+notes: everything after the 11th '|'
                    size_t ts = 0; int pc2 = 0;
                    for (size_t i = 0; i < line.size(); i++) {
                        if (line[i] == '|') { pc2++; if (pc2 == 11) { ts = i+1; break; } }
                    }
                    std::string rest = line.substr(ts);
                    size_t sep = rest.find("|||");
                    if (sep != std::string::npos) {
                        t.text  = rest.substr(0, sep);
                        t.notes = decodeNotes(rest.substr(sep + 3));
                    } else {
                        t.text = rest;
                    }
                } else {
                    // v2: TASK|id|projectId|done|priority|due|tags|minutes|text
                    t.id        = std::stoi(fields[1]);
                    t.projectId = std::stoi(fields[2]);
                    t.parentId  = 0;
                    t.done      = (fields[3] == "1");
                    t.priority  = fields[4];
                    t.due       = fields[5];
                    t.tags      = fields[6];
                    t.minutes   = fields[7].empty() ? 0 : std::stoi(fields[7]);
                    t.pinned    = false;
                    t.recur     = "";
                    // text: everything after the 8th '|'
                    size_t ts = 0; int pc2 = 0;
                    for (size_t i = 0; i < line.size(); i++) {
                        if (line[i] == '|') { pc2++; if (pc2 == 8) { ts = i+1; break; } }
                    }
                    t.text  = line.substr(ts);
                    t.notes = "";
                }
            } catch (const std::exception& ex) {
                std::cerr << "Warning: skipping malformed task line: " << ex.what() << "\n";
                continue;
            }
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

std::string parseRecur(const std::string& s) {
    if (s == "d" || s == "daily")   return "daily";
    if (s == "w" || s == "weekly")  return "weekly";
    if (s == "m" || s == "monthly") return "monthly";
    return "";
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
    std::cout << BOLD << ACCENT
              << "╔══════════════════════════════════════════════╗\n"
              << "║      📋  Terminal Task Manager  v3.0         ║\n"
              << "╚══════════════════════════════════════════════╝\n"
              << RESET;
    if (!subtitle.empty())
        std::cout << BOLD << "  " << subtitle << "\n" << RESET;
}

// ── Login screen ──────────────────────────────────────────────────────────────

// Read a PIN without echoing (Linux only via termios)
std::string readPin(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();
    // Disable echo
    system("stty -echo");
    std::string pin;
    std::getline(std::cin, pin);
    system("stty echo");
    std::cout << "\n";
    return pin;
}

bool loginScreen() {
    std::string stored = loadAuth();
    if (stored.empty()) return true; // no PIN set, allow through

    clearScreen();
    printHeader("🔐 Login");
    std::cout << "\n";

    for (int attempt = 1; attempt <= 3; attempt++) {
        std::string pin = readPin("  Enter PIN: ");
        if (hashPin(pin) == stored) {
            std::cout << SUCCESS << "  ✔ Access granted.\n" << RESET;
            return true;
        }
        if (attempt < 3)
            std::cout << ERR << "  ✘ Wrong PIN. " << (3 - attempt) << " attempt(s) left.\n" << RESET;
    }
    std::cout << ERR << BOLD << "\n  Too many failed attempts. Bye.\n" << RESET;
    return false;
}

void setupPin(bool disable = false) {
    if (disable) {
        gConfig.loginEnabled = false;
        saveConfig();
        // remove auth file
        remove(gConfig.authPath.c_str());
        std::cout << SUCCESS << "Login disabled.\n" << RESET;
        return;
    }
    std::string pin1 = readPin("  New PIN: ");
    std::string pin2 = readPin("  Confirm PIN: ");
    if (pin1 != pin2) {
        std::cout << ERR << "PINs don't match.\n" << RESET; return;
    }
    if (pin1.empty()) {
        std::cout << ERR << "PIN cannot be empty.\n" << RESET; return;
    }
    saveAuth(hashPin(pin1));
    gConfig.loginEnabled = true;
    saveConfig();
    std::cout << SUCCESS << "PIN set. Login enabled.\n" << RESET;
}

// ── Recurring task helper ─────────────────────────────────────────────────────

// When a recurring task is completed, spawn a new instance with the next due date.
void spawnNextRecurrence(std::vector<Task>& tasks, std::vector<Project>& /*projects*/,
                         const Task& t) {
    if (t.recur.empty() || t.due.empty()) return;
    std::string nextDue;
    if      (t.recur == "daily")   nextDue = addDays(t.due, 1);
    else if (t.recur == "weekly")  nextDue = addDays(t.due, 7);
    else if (t.recur == "monthly") nextDue = addMonths(t.due, 1);

    // Check if a future instance already exists (avoid duplicates)
    for (const auto& existing : tasks)
        if (existing.text == t.text && existing.projectId == t.projectId
            && existing.due == nextDue && !existing.done) return;

    Task next      = t;
    next.id        = nextTaskId(tasks);
    next.done      = false;
    next.due       = nextDue;
    next.minutes   = 0;
    next.timerRunning = false;
    next.timerStart   = 0;
    tasks.push_back(next);
    std::cout << CYAN << "  ↻ Next recurrence scheduled for " << nextDue << ".\n" << RESET;
}

// ── Search ────────────────────────────────────────────────────────────────────

void searchTasks(const std::vector<Task>& tasks,
                 const std::vector<Project>& projects) {
    std::string kw;
    std::cout << "Search keyword: ";
    std::cin.ignore(10000, '\n');
    std::getline(std::cin, kw);
    if (kw.empty()) return;

    std::string kwl = kw;
    std::transform(kwl.begin(), kwl.end(), kwl.begin(), ::tolower);

    clearScreen();
    printHeader("🔍 Search: \"" + kw + "\"");
    std::cout << "\n";

    int found = 0;
    for (const auto& t : tasks) {
        std::string textl = t.text;   std::transform(textl.begin(), textl.end(), textl.begin(), ::tolower);
        std::string tagsl = t.tags;   std::transform(tagsl.begin(), tagsl.end(), tagsl.begin(), ::tolower);
        std::string notesl = t.notes; std::transform(notesl.begin(), notesl.end(), notesl.begin(), ::tolower);
        if (textl.find(kwl)  == std::string::npos &&
            tagsl.find(kwl)  == std::string::npos &&
            notesl.find(kwl) == std::string::npos) continue;

        const Project* proj = findProject(projects, t.projectId);
        std::string projName = (t.projectId == 0) ? "Inbox" : (proj ? proj->name : "?");
        std::string status = t.done ? GREEN + "✔" + RESET : " ";
        std::string style  = t.done ? DIM : "";
        std::string indent = (t.parentId > 0) ? "    └ " : "";

        std::cout << style << indent
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

// ── Weekly agenda ─────────────────────────────────────────────────────────────

void weeklyAgenda(const std::vector<Task>& tasks,
                  const std::vector<Project>& projects) {
    clearScreen();
    printHeader("🗓  Weekly Agenda");

    // Day names
    const char* dayNames[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    time_t now = time(nullptr);
    struct tm* tnow = localtime(&now);
    (void)tnow; // used only for tm_wday below via mktime

    std::cout << "\n";

    bool anyShown = false;

    // Show overdue separately
    {
        std::vector<const Task*> overdueTasks;
        for (const auto& t : tasks)
            if (!t.done && dueDaysDiff(t.due) == -1)
                overdueTasks.push_back(&t);
        if (!overdueTasks.empty()) {
            std::cout << RED << BOLD << "  ⚠  Overdue\n" << RESET;
            for (const Task* t : overdueTasks) {
                const Project* proj = findProject(projects, t->projectId);
                std::string projName = (t->projectId == 0) ? "Inbox" : (proj ? proj->name : "?");
                std::cout << "    " << std::setw(4) << t->id << "  "
                          << priColour(t->priority) << priTag(t->priority) << RESET
                          << "  " << t->text
                          << RED << " [" << t->due << "]" << RESET
                          << DIM << "  [" << projName << "]" << RESET << "\n";
            }
            std::cout << "\n";
            anyShown = true;
        }
    }

    // Show next 7 days
    for (int d = 0; d < 7; d++) {
        std::string dayDate = addDays(todayStr(), d);

        // Get weekday name
        struct tm tday = {};
        sscanf(dayDate.c_str(), "%d-%d-%d", &tday.tm_year, &tday.tm_mon, &tday.tm_mday);
        tday.tm_year -= 1900; tday.tm_mon -= 1;
        mktime(&tday);
        const char* dayName = dayNames[tday.tm_wday];

        std::vector<const Task*> dayTasks;
        for (const auto& t : tasks)
            if (!t.done && t.due == dayDate)
                dayTasks.push_back(&t);

        std::string label = (d == 0) ? std::string("  Today (") + dayName + ", " + dayDate + ")"
                                     : std::string("  ") + dayName + ", " + dayDate;

        if (d == 0)
            std::cout << YELLOW << BOLD << label << "\n" << RESET;
        else
            std::cout << BOLD << label << "\n" << RESET;

        if (dayTasks.empty()) {
            std::cout << DIM << "    (nothing due)\n" << RESET;
        } else {
            for (const Task* t : dayTasks) {
                const Project* proj = findProject(projects, t->projectId);
                std::string projName = (t->projectId == 0) ? "Inbox" : (proj ? proj->name : "?");
                std::string indent = (t->parentId > 0) ? "      └ " : "    ";
                std::cout << indent
                          << std::setw(4) << t->id << "  "
                          << priColour(t->priority) << priTag(t->priority) << RESET
                          << "  " << (t->pinned ? "📌 " : "")
                          << t->text
                          << colorTags(t->tags)
                          << DIM << "  [" << projName << "]" << RESET << "\n";
            }
            anyShown = true;
        }
        std::cout << "\n";
    }

    if (!anyShown)
        std::cout << SUCCESS << "  ✔ All clear — nothing overdue or due this week!\n" << RESET;

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
        std::cout << ERR << "Could not open file for writing.\n" << RESET; return;
    }

    std::string today = todayStr();
    f << "# Task Export — " << today << "\n\n";

    auto writeSection = [&](int projId, const std::string& heading) {
        f << "## " << heading << "\n\n";
        bool any = false;
        for (const auto& t : tasks) {
            if (t.projectId != projId || t.parentId != 0) continue;
            if (t.done) continue;
            f << "- [" << (t.done ? "x" : " ") << "] [" << t.priority << "] " << t.text;
            if (!t.due.empty())   f << " (due: " << t.due << ")";
            if (!t.tags.empty())  f << " " << t.tags;
            if (t.minutes > 0)   f << " ⏱" << t.minutes << "min";
            if (!t.recur.empty()) f << " ↻" << t.recur;
            if (!t.notes.empty()) f << "\n  > " << t.notes;
            f << "\n";
            // subtasks
            for (const auto& sub : tasks) {
                if (sub.parentId != t.id) continue;
                f << "  - [" << (sub.done ? "x" : " ") << "] " << sub.text << "\n";
            }
            any = true;
        }
        if (!any) f << "_No active tasks._\n";
        f << "\n";
    };

    writeSection(0, "📥 Inbox");
    for (const auto& p : projects) writeSection(p.id, "📁 " + p.name);

    int total = tasks.size(), done = 0;
    for (const auto& t : tasks) if (t.done) done++;
    f << "---\n_Total: " << total << " | Done: " << done
      << " | Active: " << (total - done) << "_\n";

    std::cout << SUCCESS << "Exported to " << fname << "\n" << RESET;
}

// ── Stats ─────────────────────────────────────────────────────────────────────

void showStats(const std::vector<Task>& tasks,
               const std::vector<Project>& projects) {
    clearScreen();
    printHeader("📊 Statistics");
    std::cout << "\n";

    int total = 0, done = 0, overdue = 0, dueToday = 0, subtasks = 0, pinned = 0, recurring = 0;
    int byPri[3] = {0,0,0};
    int totalMins = 0;

    for (const auto& t : tasks) {
        total++;
        if (t.done)          done++;
        if (t.parentId > 0)  subtasks++;
        if (t.pinned)        pinned++;
        if (!t.recur.empty()) recurring++;
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

    int pct    = (total > 0) ? (done * 100 / total) : 0;
    int filled = pct / 5;
    std::string bar = "[";
    for (int i = 0; i < 20; i++) bar += (i < filled ? "█" : "░");
    bar += "]";

    std::cout << BOLD << "  Overall completion\n" << RESET;
    std::cout << "  " << SUCCESS << bar << " " << pct << "%" << RESET
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

    std::cout << BOLD << "  Extra\n" << RESET;
    std::cout << "  📌 Pinned:    " << pinned    << "\n";
    std::cout << "  ↻  Recurring: " << recurring << "\n";
    std::cout << "  ⊹  Subtasks:  " << subtasks  << "\n\n";

    if (totalMins > 0) {
        std::cout << BOLD << "  Time tracked\n" << RESET;
        std::cout << "  Total: " << CYAN << (totalMins / 60) << "h "
                  << (totalMins % 60) << "m" << RESET << "\n\n";
    }

    std::cout << BOLD << "  Per project\n" << RESET;
    auto printRow = [&](int projId, const std::string& label) {
        int t2 = 0, d2 = 0;
        for (const auto& t : tasks) if (t.projectId == projId) { t2++; if (t.done) d2++; }
        int p2 = t2 > 0 ? d2 * 100 / t2 : 0;
        std::cout << "  " << std::setw(20) << std::left << label
                  << DIM << "  " << (t2-d2) << " active / " << t2 << " total  "
                  << p2 << "%\n" << RESET;
    };
    printRow(0, "Inbox");
    for (const auto& p : projects) printRow(p.id, p.name);

    pressEnter();
}

// ══════════════════════════════════════════════════════════════════════════════
// PROJECT SCREEN
// ══════════════════════════════════════════════════════════════════════════════

void printProjectList(const std::vector<Project>& projects,
                      const std::vector<Task>&    tasks) {
    std::cout << "\n";
    auto printRow = [&](int projId, const std::string& label, const std::string& icon) {
        int total = 0, done = 0, overdue = 0;
        for (const auto& t : tasks) {
            if (t.projectId != projId) continue;
            total++; if (t.done) done++;
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
    for (const auto& p : projects) printRow(p.id, p.name, "📁");
    if (projects.empty())
        std::cout << DIM << "\n  No projects yet. Press 'n' to create one.\n" << RESET;
}

void printProjectMenu() {
    std::cout << BOLD << "\n── Commands ──────────────────────────────────────\n" << RESET
              << "  " << ACCENT << "<id>" << RESET << "  Open project / inbox\n"
              << "  " << ACCENT << "n"    << RESET << "    New project\n"
              << "  " << ACCENT << "R"    << RESET << "    Rename project\n"
              << "  " << ACCENT << "X"    << RESET << "    Delete project (tasks → inbox)\n"
              << "  " << ACCENT << "/"    << RESET << "    Search all tasks\n"
              << "  " << ACCENT << "W"    << RESET << "    Weekly agenda\n"
              << "  " << ACCENT << "E"    << RESET << "    Export to markdown\n"
              << "  " << ACCENT << "S"    << RESET << "    Statistics\n"
              << "  " << ACCENT << "O"    << RESET << "    Options / settings\n"
              << "  " << ACCENT << "q"    << RESET << "    Quit\n"
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

    std::vector<const Task*> shown;
    int total = 0, done = 0, high = 0;

    // Only top-level tasks for main list
    for (const auto& t : tasks) {
        if (t.projectId != projectId) continue;
        if (t.parentId != 0) continue; // subtasks printed under parent
        total++;
        if (t.done) done++;
        if (t.priority == "high" && !t.done) high++;

        if (filter == "active" && t.done)  continue;
        if (filter == "done"   && !t.done) continue;
        if (filter == "high"   && (t.priority != "high" || t.done)) continue;
        if (filter == "today"  && !(dueDaysDiff(t.due) == 0 || dueDaysDiff(t.due) == -1)) continue;
        if (filter.size() > 0 && filter[0] == '#') {
            if (t.tags.find(filter) == std::string::npos) continue;
        }
        if (!gConfig.showDone && t.done) continue;
        shown.push_back(&t);
    }

    // Sort: pinned first, then by chosen sort
    if (sortMode == "priority") {
        std::sort(shown.begin(), shown.end(), [](const Task* a, const Task* b) {
            if (a->pinned != b->pinned) return a->pinned > b->pinned;
            auto rank = [](const std::string& p) {
                if (p == "high")   return 0;
                if (p == "medium") return 1;
                return 2;
            };
            return rank(a->priority) < rank(b->priority);
        });
    } else if (sortMode == "due") {
        std::sort(shown.begin(), shown.end(), [](const Task* a, const Task* b) {
            if (a->pinned != b->pinned) return a->pinned > b->pinned;
            std::string da = a->due.empty() ? "9999-99-99" : a->due;
            std::string db = b->due.empty() ? "9999-99-99" : b->due;
            return da < db;
        });
    } else {
        // id order but pinned first
        std::stable_sort(shown.begin(), shown.end(), [](const Task* a, const Task* b) {
            return a->pinned > b->pinned;
        });
    }

    for (const Task* t : shown) {
        std::string status  = t->done   ? GREEN + "✔" + RESET : " ";
        std::string style   = t->done   ? DIM : "";
        std::string pinMark = t->pinned ? "📌 " : "   ";
        bool timerOn = t->timerRunning;

        std::string recurMark = "";
        if (!t->recur.empty()) recurMark = CYAN + " ↻" + t->recur[0] + RESET;

        std::cout << style
                  << std::setw(4) << t->id << "  "
                  << "[" << status << "]  "
                  << pinMark
                  << priColour(t->priority) << priTag(t->priority) << RESET
                  << "  " << style << t->text << RESET
                  << recurMark
                  << dueLabel(t->due, t->done)
                  << colorTags(t->tags)
                  << minutesToHMS(t->minutes)
                  << (timerOn ? YELLOW + " ▶TIMER" + RESET : "")
                  << "\n";

        // Print notes preview (first line)
        if (!t->notes.empty()) {
            std::string firstLine = t->notes;
            size_t nl = firstLine.find('\n');
            if (nl != std::string::npos) firstLine = firstLine.substr(0, nl) + "…";
            std::cout << DIM << "       ↳ " << firstLine << RESET << "\n";
        }

        // Print subtasks
        for (const auto& sub : tasks) {
            if (sub.projectId != projectId || sub.parentId != t->id) continue;
            if (!gConfig.showDone && sub.done) continue;
            std::string subStatus = sub.done ? GREEN + "✔" + RESET : "○";
            std::string subStyle  = sub.done ? DIM : "";
            std::cout << subStyle
                      << "       └ " << std::setw(3) << sub.id << "  "
                      << "[" << subStatus << "]  "
                      << subStyle << sub.text << RESET
                      << dueLabel(sub.due, sub.done)
                      << "\n";
        }
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
              << "  " << ACCENT << "a" << RESET << "  Add task\n"
              << "  " << ACCENT << "A" << RESET << "  Add subtask\n"
              << "  " << ACCENT << "d" << RESET << "  Done / undone (toggle)\n"
              << "  " << ACCENT << "B" << RESET << "  Bulk done (e.g. 1 3 5)\n"
              << "  " << ACCENT << "r" << RESET << "  Remove task\n"
              << "  " << ACCENT << "e" << RESET << "  Edit task text\n"
              << "  " << ACCENT << "N" << RESET << "  Edit notes\n"
              << "  " << ACCENT << "p" << RESET << "  Change priority\n"
              << "  " << ACCENT << "D" << RESET << "  Set / clear due date\n"
              << "  " << ACCENT << "T" << RESET << "  Edit tags\n"
              << "  " << ACCENT << "R" << RESET << "  Set recurrence\n"
              << "  " << ACCENT << "P" << RESET << "  Pin / unpin task\n"
              << "  " << ACCENT << "m" << RESET << "  Move task to another project\n"
              << "  " << ACCENT << "t" << RESET << "  Timer start/stop\n"
              << "  " << ACCENT << "f" << RESET << "  Filter  [" << filter << "]\n"
              << "  " << ACCENT << "o" << RESET << "  Sort    [" << sortMode << "]\n"
              << "  " << ACCENT << "c" << RESET << "  Clear done tasks\n"
              << "  " << ACCENT << "u" << RESET << "  Undo last delete\n"
              << "  " << ACCENT << "b" << RESET << "  Back to projects\n"
              << "──────────────────────────────────────────────────\n"
              << BOLD << "Enter command: " << RESET;
}

// ── Task actions ──────────────────────────────────────────────────────────────

void addTask(std::vector<Task>& tasks,
             std::vector<Project>& projects,
             int projectId,
             int parentId = 0) {
    std::string text, pri, due, tags, recurStr;
    if (parentId > 0) {
        Task* parent = findTask(tasks, parentId);
        if (!parent) { std::cout << ERR << "Parent task not found.\n" << RESET; return; }
        std::cout << "Subtask for [" << parentId << "] \"" << parent->text << "\"\n";
    }
    std::cout << (parentId > 0 ? "Subtask description: " : "Task description: ");
    std::cin.ignore(10000, '\n');
    std::getline(std::cin, text);
    if (text.empty()) { std::cout << ERR << "Empty — cancelled.\n" << RESET; return; }

    std::cout << "Priority (l/m/h) [" << gConfig.defaultPriority[0] << "]: ";
    std::getline(std::cin, pri);

    std::cout << "Due date (YYYY-MM-DD or blank): ";
    std::getline(std::cin, due);
    if (!due.empty() && !isValidDate(due)) {
        std::cout << WARN << "Invalid date format, skipping.\n" << RESET; due = "";
    }

    std::cout << "Tags (e.g. #work #waiting or blank): ";
    std::getline(std::cin, tags);

    if (parentId == 0) {
        std::cout << "Recurrence (d/w/m or blank for none): ";
        std::getline(std::cin, recurStr);
    }

    Task t;
    t.id        = nextTaskId(tasks);
    t.projectId = projectId;
    t.parentId  = parentId;
    t.text      = text;
    t.priority  = parsePriority(pri);
    t.done      = false;
    t.due       = due;
    t.tags      = tags;
    t.minutes   = 0;
    t.pinned    = false;
    t.recur     = parseRecur(recurStr);
    tasks.push_back(t);
    saveAll(projects, tasks);
    std::cout << SUCCESS << "Task added.\n" << RESET;
}

void addSubtask(std::vector<Task>& tasks, std::vector<Project>& projects, int projectId) {
    int parentId;
    std::cout << "Parent task ID: "; std::cin >> parentId;
    if (!findTask(tasks, parentId)) {
        std::cout << ERR << "Parent task not found.\n" << RESET; return;
    }
    addTask(tasks, projects, projectId, parentId);
}

void toggleDone(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << ERR << "ID not found.\n" << RESET; return; }
    if (t->timerRunning) {
        t->minutes += (int)difftime(time(nullptr), t->timerStart) / 60;
        t->timerRunning = false;
    }
    t->done = !t->done;
    if (t->done && !t->recur.empty())
        spawnNextRecurrence(tasks, projects, *t);
    saveAll(projects, tasks);
    std::cout << SUCCESS << (t->done ? "Marked done.\n" : "Marked active.\n") << RESET;
}

void bulkDone(std::vector<Task>& tasks, std::vector<Project>& projects) {
    std::cout << "Task IDs (space-separated): ";
    std::cin.ignore(10000, '\n');
    std::string line; std::getline(std::cin, line);
    std::istringstream ss(line);
    int id, count = 0;
    while (ss >> id) {
        Task* t = findTask(tasks, id);
        if (t && !t->done) {
            t->done = true;
            if (!t->recur.empty()) spawnNextRecurrence(tasks, projects, *t);
            count++;
        }
    }
    saveAll(projects, tasks);
    std::cout << SUCCESS << count << " task(s) marked done.\n" << RESET;
}

void removeTask(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID to remove: "; std::cin >> id;

    if (gConfig.confirmDelete) {
        Task* t = findTask(tasks, id);
        if (!t) { std::cout << ERR << "ID not found.\n" << RESET; return; }
        std::cout << "  Delete: \"" << t->text << "\"? (y/N) ";
        char c; std::cin >> c;
        if (c != 'y' && c != 'Y') {
            std::cout << DIM << "Cancelled.\n" << RESET; return;
        }
    }

    for (auto it = tasks.begin(); it != tasks.end(); ++it) {
        if (it->id == id) {
            gUndoTask = *it;
            tasks.erase(it);
            // Also remove subtasks
            tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
                [id](const Task& t){ return t.parentId == id; }), tasks.end());
            saveAll(projects, tasks);
            std::cout << SUCCESS << "Task removed. (press 'u' to undo)\n" << RESET;
            return;
        }
    }
    std::cout << ERR << "ID not found.\n" << RESET;
}

void undoDelete(std::vector<Task>& tasks, std::vector<Project>& projects) {
    if (!gUndoTask) { std::cout << WARN << "Nothing to undo.\n" << RESET; return; }
    gUndoTask->id = nextTaskId(tasks);
    tasks.push_back(*gUndoTask);
    gUndoTask.reset();
    saveAll(projects, tasks);
    std::cout << SUCCESS << "Task restored.\n" << RESET;
}

void editTask(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID to edit: "; std::cin >> id;
    std::cin.ignore(10000, '\n');
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << ERR << "ID not found.\n" << RESET; return; }
    std::cout << "New text [" << t->text << "]: ";
    std::string s; std::getline(std::cin, s);
    if (!s.empty()) t->text = s;
    saveAll(projects, tasks);
    std::cout << SUCCESS << "Updated.\n" << RESET;
}

void editNotes(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    std::cin.ignore(10000, '\n');
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << ERR << "ID not found.\n" << RESET; return; }

    if (!t->notes.empty()) {
        std::cout << DIM << "Current notes:\n" << t->notes << "\n" << RESET;
    }
    std::cout << "Enter new notes (blank line to finish, empty to clear):\n";

    std::string allNotes, line;
    while (true) {
        std::getline(std::cin, line);
        if (line.empty()) break;
        if (!allNotes.empty()) allNotes += "\n";
        allNotes += line;
    }
    t->notes = allNotes;
    saveAll(projects, tasks);
    std::cout << SUCCESS << "Notes updated.\n" << RESET;
}

void changePriority(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    std::cin.ignore(10000, '\n');
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << ERR << "ID not found.\n" << RESET; return; }
    std::string s;
    std::cout << "New priority (l/m/h): "; std::getline(std::cin, s);
    t->priority = parsePriority(s);
    saveAll(projects, tasks);
    std::cout << SUCCESS << "Priority updated.\n" << RESET;
}

void setDueDate(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    std::cin.ignore(10000, '\n');
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << ERR << "ID not found.\n" << RESET; return; }
    std::cout << "Due date (YYYY-MM-DD, or blank to clear) [" << t->due << "]: ";
    std::string s; std::getline(std::cin, s);
    if (s.empty()) {
        t->due = ""; std::cout << SUCCESS << "Due date cleared.\n" << RESET;
    } else if (isValidDate(s)) {
        t->due = s; std::cout << SUCCESS << "Due date set to " << s << ".\n" << RESET;
    } else {
        std::cout << ERR << "Invalid format (need YYYY-MM-DD).\n" << RESET; return;
    }
    saveAll(projects, tasks);
}

void editTags(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    std::cin.ignore(10000, '\n');
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << ERR << "ID not found.\n" << RESET; return; }
    std::cout << "Tags (e.g. #work #waiting, blank to clear) [" << t->tags << "]: ";
    std::string s; std::getline(std::cin, s);
    t->tags = s;
    saveAll(projects, tasks);
    std::cout << SUCCESS << "Tags updated.\n" << RESET;
}

void setRecurrence(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    std::cin.ignore(10000, '\n');
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << ERR << "ID not found.\n" << RESET; return; }
    if (t->due.empty()) {
        std::cout << WARN << "Note: task has no due date — recurrence needs a due date to calculate next occurrence.\n" << RESET;
    }
    std::cout << "Recurrence (d=daily / w=weekly / m=monthly / blank=none) [" << t->recur << "]: ";
    std::string s; std::getline(std::cin, s);
    t->recur = parseRecur(s);
    saveAll(projects, tasks);
    std::cout << SUCCESS << "Recurrence set to \"" << (t->recur.empty() ? "none" : t->recur) << "\".\n" << RESET;
}

void togglePin(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << ERR << "ID not found.\n" << RESET; return; }
    t->pinned = !t->pinned;
    saveAll(projects, tasks);
    std::cout << SUCCESS << (t->pinned ? "📌 Task pinned.\n" : "Task unpinned.\n") << RESET;
}

void timerToggle(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << ERR << "ID not found.\n" << RESET; return; }
    if (t->timerRunning) {
        int elapsed = (int)difftime(time(nullptr), t->timerStart) / 60;
        t->minutes += elapsed;
        t->timerRunning = false;
        saveAll(projects, tasks);
        std::cout << SUCCESS << "Timer stopped. +" << elapsed << "m  (total: "
                  << t->minutes << "m)\n" << RESET;
    } else {
        t->timerRunning = true;
        t->timerStart   = time(nullptr);
        std::cout << SUCCESS << "Timer started for task " << id << ".\n" << RESET;
    }
}

void moveTask(std::vector<Task>& tasks, const std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID to move: "; std::cin >> id;
    Task* t = findTask(tasks, id);
    if (!t) { std::cout << ERR << "ID not found.\n" << RESET; return; }

    std::cout << "\nAvailable projects:\n";
    std::cout << DIM << "  0  📥 Inbox\n" << RESET;
    for (const auto& p : projects)
        std::cout << "  " << p.id << "  📁 " << p.name << "\n";

    int dest;
    std::cout << "Move to project ID: "; std::cin >> dest;
    if (dest != 0 && !findProject(projects, dest)) {
        std::cout << ERR << "Project not found.\n" << RESET; return;
    }
    t->projectId = dest;
    std::cout << SUCCESS << "Task moved.\n" << RESET;
}

void clearDone(std::vector<Task>& tasks, std::vector<Project>& projects, int projectId) {
    int before = tasks.size();
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
        [projectId](const Task& t){ return t.done && t.projectId == projectId; }),
        tasks.end());
    saveAll(projects, tasks);
    std::cout << SUCCESS << "Removed " << (before - (int)tasks.size()) << " completed task(s).\n" << RESET;
}

// ── Project actions ───────────────────────────────────────────────────────────

void newProject(std::vector<Project>& projects, std::vector<Task>& tasks) {
    std::string name;
    std::cout << "Project name: ";
    std::cin.ignore(10000, '\n');
    std::getline(std::cin, name);
    if (name.empty()) { std::cout << ERR << "Empty name — cancelled.\n" << RESET; return; }
    projects.push_back({nextProjectId(projects), name});
    saveAll(projects, tasks);
    std::cout << SUCCESS << "Project \"" << name << "\" created.\n" << RESET;
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
            std::cout << SUCCESS << "Renamed.\n" << RESET; return;
        }
    }
    std::cout << ERR << "Project not found.\n" << RESET;
}

void deleteProject(std::vector<Project>& projects, std::vector<Task>& tasks) {
    int id;
    std::cout << "Project ID to delete: "; std::cin >> id;
    if (id == 0) { std::cout << ERR << "Cannot delete Inbox.\n" << RESET; return; }

    auto it = std::remove_if(projects.begin(), projects.end(),
                             [id](const Project& p){ return p.id == id; });
    if (it == projects.end()) { std::cout << ERR << "Project not found.\n" << RESET; return; }

    int moved = 0;
    for (auto& t : tasks) if (t.projectId == id) { t.projectId = 0; moved++; }

    projects.erase(it, projects.end());
    saveAll(projects, tasks);
    std::cout << SUCCESS << "Project deleted. " << moved << " task(s) moved to Inbox.\n" << RESET;
}

// ── Options screen ────────────────────────────────────────────────────────────

void optionsScreen() {
    while (true) {
        clearScreen();
        printHeader("⚙  Options");
        std::cout << "\n"
                  << "  " << ACCENT << "1" << RESET << "  Data file path\n"
                  << "       " << DIM << gConfig.dataPath << RESET << "\n\n"
                  << "  " << ACCENT << "2" << RESET << "  Default priority\n"
                  << "       " << DIM << gConfig.defaultPriority << RESET << "\n\n"
                  << "  " << ACCENT << "3" << RESET << "  Show done tasks in list\n"
                  << "       " << DIM << (gConfig.showDone ? "yes" : "no") << RESET << "\n\n"
                  << "  " << ACCENT << "4" << RESET << "  Confirm before delete\n"
                  << "       " << DIM << (gConfig.confirmDelete ? "yes" : "no") << RESET << "\n\n"
                  << "  " << ACCENT << "5" << RESET << "  Theme\n"
                  << "       " << DIM << gConfig.theme << RESET << "  (default / solarized / nord / mono)\n\n"
                  << "  " << ACCENT << "6" << RESET << "  Login PIN\n"
                  << "       " << DIM << (gConfig.loginEnabled ? "enabled" : "disabled") << RESET << "\n\n"
                  << "  " << ACCENT << "b" << RESET << "  Back\n"
                  << BOLD << "\nEnter option: " << RESET;

        char cmd; std::cin >> cmd;
        std::cin.ignore(10000, '\n');

        if (cmd == '1') {
            std::cout << "New data path [" << gConfig.dataPath << "]: ";
            std::string s; std::getline(std::cin, s);
            if (!s.empty()) gConfig.dataPath = s;
            saveConfig();
            std::cout << SUCCESS << "Saved. Restart to load from new path.\n" << RESET;
            pressEnter();
        } else if (cmd == '2') {
            std::cout << "Default priority (l/m/h): ";
            std::string s; std::getline(std::cin, s);
            if (!s.empty()) gConfig.defaultPriority = parsePriority(s);
            saveConfig();
            std::cout << SUCCESS << "Saved.\n" << RESET;
            pressEnter();
        } else if (cmd == '3') {
            gConfig.showDone = !gConfig.showDone;
            saveConfig();
            std::cout << SUCCESS << "Show done: " << (gConfig.showDone ? "yes" : "no") << "\n" << RESET;
            pressEnter();
        } else if (cmd == '4') {
            gConfig.confirmDelete = !gConfig.confirmDelete;
            saveConfig();
            std::cout << SUCCESS << "Confirm delete: " << (gConfig.confirmDelete ? "yes" : "no") << "\n" << RESET;
            pressEnter();
        } else if (cmd == '5') {
            std::cout << "Theme (default / solarized / nord / mono): ";
            std::string s; std::getline(std::cin, s);
            if (s == "default" || s == "solarized" || s == "nord" || s == "mono") {
                gConfig.theme = s;
                gTheme = makeTheme(s);
                saveConfig();
                std::cout << SUCCESS << "Theme set to \"" << s << "\".\n" << RESET;
            } else {
                std::cout << ERR << "Unknown theme.\n" << RESET;
            }
            pressEnter();
        } else if (cmd == '6') {
            if (gConfig.loginEnabled) {
                std::cout << "Disable login? (y/N): ";
                char c; std::cin >> c;
                std::cin.ignore(10000, '\n');
                if (c == 'y' || c == 'Y') setupPin(true);
            } else {
                setupPin(false);
            }
            pressEnter();
        } else if (cmd == 'b') {
            return;
        } else {
            std::cout << ERR << "Unknown option.\n" << RESET;
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
            case 'a': addTask(tasks, projects, projectId);           break;
            case 'A': addSubtask(tasks, projects, projectId);        break;
            case 'd': toggleDone(tasks, projects);                   break;
            case 'B': bulkDone(tasks, projects);                     break;
            case 'r': removeTask(tasks, projects);                   break;
            case 'u': undoDelete(tasks, projects);                   break;
            case 'e': editTask(tasks, projects);                     break;
            case 'N': editNotes(tasks, projects);                    break;
            case 'p': changePriority(tasks, projects);               break;
            case 'D': setDueDate(tasks, projects);                   break;
            case 'T': editTags(tasks, projects);                     break;
            case 'R': setRecurrence(tasks, projects);                break;
            case 'P': togglePin(tasks, projects);                    break;
            case 't': timerToggle(tasks, projects);                  break;
            case 'm':
                moveTask(tasks, projects);
                saveAll(projects, tasks);
                break;
            case 'c': clearDone(tasks, projects, projectId);         break;
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
            default:  std::cout << ERR << "Unknown command.\n" << RESET; break;
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
                std::cout << ERR << "Project ID not found.\n" << RESET;
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
            case 'W': weeklyAgenda(tasks, projects);                  break;
            case 'E': {
                std::cin.ignore(10000, '\n');
                exportTasks(tasks, projects);
                pressEnter();
                continue;
            }
            case 'S': showStats(tasks, projects);                     break;
            case 'O': optionsScreen();                                break;
            case 'q':
                std::cout << ACCENT << "\nBye! Stay productive 🚀\n" << RESET;
                return;
            default:
                std::cout << ERR << "Unknown command.\n" << RESET;
        }
        pressEnter();
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    loadConfig();
    gTheme = makeTheme(gConfig.theme);

    // Login
    if (gConfig.loginEnabled) {
        if (!loginScreen()) return 1;
    }

    std::vector<Project> projects;
    std::vector<Task>    tasks;
    loadAll(projects, tasks);

    int overdue = 0;
    for (const auto& t : tasks)
        if (!t.done && dueDaysDiff(t.due) == -1) overdue++;

    clearScreen();
    printHeader();
    if (overdue > 0)
        std::cout << BG_RED << BOLD << "  ⚠  " << overdue
                  << " overdue task(s)! Check your projects." << RESET << "\n";
    std::cout << DIM << "  Data:  " << gConfig.dataPath << "\n" << RESET;
    std::cout << DIM << "  Today: " << todayStr() << "\n" << RESET;
    std::cout << DIM << "  Theme: " << gConfig.theme << "\n" << RESET;

    pressEnter();

    projectScreen(projects, tasks);
    return 0;
}