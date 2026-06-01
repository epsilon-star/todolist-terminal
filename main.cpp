/*
 * todo.cpp — Terminal Task Manager with Projects
 * Compile:  g++ -o todo todo.cpp
 * Run:      ./todo
 *
 * Data is saved in tasks.dat (same directory).
 * Format:   PROJECT|<id>|<name>
 *           TASK|<id>|<projectId>|<done>|<priority>|<text>
 */

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>

// ── Data structures ──────────────────────────────────────────────────────────

struct Project {
    int         id;
    std::string name;
};

struct Task {
    int         id;
    int         projectId;   // 0 = inbox (no project)
    std::string text;
    std::string priority;    // low / medium / high
    bool        done;
};

// ── ANSI colours ─────────────────────────────────────────────────────────────

const std::string RESET  = "\033[0m";
const std::string BOLD   = "\033[1m";
const std::string DIM    = "\033[2m";
const std::string RED    = "\033[31m";
const std::string GREEN  = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string CYAN   = "\033[36m";
const std::string MAGENTA= "\033[35m";

// ── Persistence ──────────────────────────────────────────────────────────────

const std::string FILE_PATH = "tasks.dat";

void saveAll(const std::vector<Project>& projects, const std::vector<Task>& tasks) {
    std::ofstream f(FILE_PATH);
    for (const auto& p : projects)
        f << "PROJECT|" << p.id << "|" << p.name << "\n";
    for (const auto& t : tasks)
        f << "TASK|" << t.id << "|" << t.projectId << "|"
          << t.done << "|" << t.priority << "|" << t.text << "\n";
}

void loadAll(std::vector<Project>& projects, std::vector<Task>& tasks) {
    std::ifstream f(FILE_PATH);
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
            std::getline(ss, tmp, '|'); t.id        = std::stoi(tmp);
            std::getline(ss, tmp, '|'); t.projectId = std::stoi(tmp);
            std::getline(ss, tmp, '|'); t.done      = (tmp == "1");
            std::getline(ss, t.priority, '|');
            std::getline(ss, t.text);
            tasks.push_back(t);
        }
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

int nextProjectId(const std::vector<Project>& projects) {
    int mx = 0;
    for (const auto& p : projects) mx = std::max(mx, p.id);
    return mx + 1;
}

int nextTaskId(const std::vector<Task>& tasks) {
    int mx = 0;
    for (const auto& t : tasks) mx = std::max(mx, t.id);
    return mx + 1;
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
    if (s == "l" || s == "low")  return "low";
    if (s == "h" || s == "high") return "high";
    return "medium";
}

const Project* findProject(const std::vector<Project>& projects, int id) {
    for (const auto& p : projects) if (p.id == id) return &p;
    return nullptr;
}

void clearScreen() { std::cout << "\033[2J\033[H"; }

void pressEnter() {
    std::cout << DIM << "\nPress Enter to continue…" << RESET;
    std::cin.ignore(); std::cin.get();
}

// ── Display helpers ───────────────────────────────────────────────────────────

void printHeader(const std::string& subtitle = "") {
    std::cout << BOLD << CYAN
              << "╔══════════════════════════════════════════════╗\n"
              << "║        📋  Terminal Task Manager             ║\n"
              << "╚══════════════════════════════════════════════╝\n"
              << RESET;
    if (!subtitle.empty())
        std::cout << BOLD << "  " << subtitle << "\n" << RESET;
}

// ══════════════════════════════════════════════════════════════════════════════
// PROJECT SCREEN
// ══════════════════════════════════════════════════════════════════════════════

void printProjectList(const std::vector<Project>& projects,
                      const std::vector<Task>&    tasks) {
    std::cout << "\n";

    // Inbox stats
    int inboxTotal = 0, inboxDone = 0;
    for (const auto& t : tasks) {
        if (t.projectId == 0) { inboxTotal++; if (t.done) inboxDone++; }
    }
    std::cout << BOLD << "  " << std::setw(3) << 0 << "  "
              << CYAN << "📥 Inbox" << RESET
              << DIM << "  (" << (inboxTotal - inboxDone) << " active / " << inboxTotal << " total)\n" << RESET;

    if (projects.empty()) {
        std::cout << DIM << "\n  No projects yet. Press 'n' to create one.\n" << RESET;
    } else {
        for (const auto& p : projects) {
            int total = 0, done = 0;
            for (const auto& t : tasks) {
                if (t.projectId == p.id) { total++; if (t.done) done++; }
            }
            int pct = (total > 0) ? (done * 100 / total) : 0;

            // simple ASCII progress bar (10 chars)
            int filled = pct / 10;
            std::string bar = "[";
            for (int i = 0; i < 10; i++) bar += (i < filled ? "█" : "░");
            bar += "]";

            std::cout << "  " << std::setw(3) << p.id << "  "
                      << MAGENTA << "📁 " << p.name << RESET
                      << DIM << "  " << bar << " " << pct << "%"
                      << "  (" << (total - done) << " active / " << total << " total)\n"
                      << RESET;
        }
    }
}

void printProjectMenu() {
    std::cout << BOLD << "\n── Commands ──────────────────────────────────────\n" << RESET
              << "  " << CYAN << "<id>" << RESET << "  Open project / inbox (enter its number)\n"
              << "  " << CYAN << "n"    << RESET << "    New project\n"
              << "  " << CYAN << "R"    << RESET << "    Rename project\n"
              << "  " << CYAN << "X"    << RESET << "    Delete project (moves tasks to inbox)\n"
              << "  " << CYAN << "q"    << RESET << "    Quit\n"
              << "──────────────────────────────────────────────────\n"
              << BOLD << "Enter command: " << RESET;
}

// ══════════════════════════════════════════════════════════════════════════════
// TASK SCREEN
// ══════════════════════════════════════════════════════════════════════════════

void printTaskList(const std::vector<Task>& tasks,
                   int projectId,
                   const std::string& filter) {
    std::cout << "\n";
    int shown = 0, total = 0, done = 0, high = 0;

    for (const auto& t : tasks) {
        if (t.projectId != projectId) continue;
        total++;
        if (t.done) done++;
        if (t.priority == "high" && !t.done) high++;

        if (filter == "active" &&  t.done) continue;
        if (filter == "done"   && !t.done) continue;
        if (filter == "high"   && (t.priority != "high" || t.done)) continue;

        std::string status = t.done ? GREEN + "✔" + RESET : " ";
        std::string style  = t.done ? DIM : "";
        std::cout << style
                  << std::setw(4) << t.id << "  "
                  << "[" << status << "]  "
                  << priColour(t.priority) << priTag(t.priority) << RESET
                  << "  " << style << t.text
                  << RESET << "\n";
        shown++;
    }

    if (shown == 0) std::cout << DIM << "  (no tasks to show)\n" << RESET;

    std::cout << DIM
              << "\n  Total: " << total
              << "  |  Active: " << (total - done)
              << "  |  Done: " << done
              << "  |  High: " << high
              << "\n" << RESET;
}

void printTaskMenu(const std::string& filter) {
    std::cout << BOLD << "\n── Commands ──────────────────────────────────────\n" << RESET
              << "  " << CYAN << "a" << RESET << "  Add task\n"
              << "  " << CYAN << "d" << RESET << "  Done / undone (toggle by ID)\n"
              << "  " << CYAN << "r" << RESET << "  Remove task\n"
              << "  " << CYAN << "e" << RESET << "  Edit task text\n"
              << "  " << CYAN << "p" << RESET << "  Change priority\n"
              << "  " << CYAN << "m" << RESET << "  Move task to another project\n"
              << "  " << CYAN << "f" << RESET << "  Filter  [" << filter << "]\n"
              << "  " << CYAN << "c" << RESET << "  Clear done tasks\n"
              << "  " << CYAN << "b" << RESET << "  Back to projects\n"
              << "──────────────────────────────────────────────────\n"
              << BOLD << "Enter command: " << RESET;
}

// ── Task actions ──────────────────────────────────────────────────────────────

void addTask(std::vector<Task>& tasks,
             std::vector<Project>& projects,
             int projectId) {
    std::string text, pri;
    std::cout << "Task description: ";
    std::cin.ignore();
    std::getline(std::cin, text);
    if (text.empty()) { std::cout << RED << "Empty — cancelled.\n" << RESET; return; }

    std::cout << "Priority (l=low / m=medium / h=high) [m]: ";
    std::getline(std::cin, pri);

    tasks.push_back({nextTaskId(tasks), projectId, text, parsePriority(pri), false});
    saveAll(projects, tasks);
    std::cout << GREEN << "Task added.\n" << RESET;
}

void toggleDone(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    for (auto& t : tasks) {
        if (t.id == id) {
            t.done = !t.done;
            saveAll(projects, tasks);
            std::cout << GREEN << (t.done ? "Marked done.\n" : "Marked active.\n") << RESET;
            return;
        }
    }
    std::cout << RED << "ID not found.\n" << RESET;
}

void removeTask(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID to remove: "; std::cin >> id;
    auto it = std::remove_if(tasks.begin(), tasks.end(),
                             [id](const Task& t){ return t.id == id; });
    if (it == tasks.end()) { std::cout << RED << "ID not found.\n" << RESET; return; }
    tasks.erase(it, tasks.end());
    saveAll(projects, tasks);
    std::cout << GREEN << "Task removed.\n" << RESET;
}

void editTask(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID to edit: "; std::cin >> id;
    std::cin.ignore();
    for (auto& t : tasks) {
        if (t.id == id) {
            std::cout << "New text [" << t.text << "]: ";
            std::string s; std::getline(std::cin, s);
            if (!s.empty()) t.text = s;
            saveAll(projects, tasks);
            std::cout << GREEN << "Updated.\n" << RESET;
            return;
        }
    }
    std::cout << RED << "ID not found.\n" << RESET;
}

void changePriority(std::vector<Task>& tasks, std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    std::cin.ignore();
    for (auto& t : tasks) {
        if (t.id == id) {
            std::string s;
            std::cout << "New priority (l/m/h): "; std::getline(std::cin, s);
            t.priority = parsePriority(s);
            saveAll(projects, tasks);
            std::cout << GREEN << "Priority updated.\n" << RESET;
            return;
        }
    }
    std::cout << RED << "ID not found.\n" << RESET;
}

void moveTask(std::vector<Task>& tasks,
              const std::vector<Project>& projects) {
    int id;
    std::cout << "Task ID to move: "; std::cin >> id;
    Task* found = nullptr;
    for (auto& t : tasks) if (t.id == id) { found = &t; break; }
    if (!found) { std::cout << RED << "ID not found.\n" << RESET; return; }

    std::cout << "\nAvailable projects:\n";
    std::cout << DIM << "  0  📥 Inbox\n" << RESET;
    for (const auto& p : projects)
        std::cout << "  " << p.id << "  📁 " << p.name << "\n";

    int dest;
    std::cout << "Move to project ID: "; std::cin >> dest;

    if (dest != 0 && !findProject(projects, dest)) {
        std::cout << RED << "Project not found.\n" << RESET; return;
    }
    found->projectId = dest;
    // saveAll called by caller after return — pass by ref
    std::cout << GREEN << "Task moved.\n" << RESET;
}

void clearDone(std::vector<Task>& tasks,
               std::vector<Project>& projects,
               int projectId) {
    int before = tasks.size();
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
        [projectId](const Task& t){ return t.done && t.projectId == projectId; }),
        tasks.end());
    saveAll(projects, tasks);
    std::cout << GREEN << "Removed " << (before - (int)tasks.size()) << " completed task(s).\n" << RESET;
}

// ── Project actions ───────────────────────────────────────────────────────────

void newProject(std::vector<Project>& projects,
                std::vector<Task>& tasks) {
    std::string name;
    std::cout << "Project name: ";
    std::cin.ignore();
    std::getline(std::cin, name);
    if (name.empty()) { std::cout << RED << "Empty name — cancelled.\n" << RESET; return; }
    projects.push_back({nextProjectId(projects), name});
    saveAll(projects, tasks);
    std::cout << GREEN << "Project \"" << name << "\" created.\n" << RESET;
}

void renameProject(std::vector<Project>& projects,
                   std::vector<Task>& tasks) {
    int id;
    std::cout << "Project ID to rename: "; std::cin >> id;
    std::cin.ignore();
    for (auto& p : projects) {
        if (p.id == id) {
            std::cout << "New name [" << p.name << "]: ";
            std::string s; std::getline(std::cin, s);
            if (!s.empty()) p.name = s;
            saveAll(projects, tasks);
            std::cout << GREEN << "Renamed.\n" << RESET;
            return;
        }
    }
    std::cout << RED << "Project not found.\n" << RESET;
}

void deleteProject(std::vector<Project>& projects,
                   std::vector<Task>& tasks) {
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

// ── Task screen loop ──────────────────────────────────────────────────────────

void taskScreen(std::vector<Project>& projects,
                std::vector<Task>& tasks,
                int projectId) {
    std::string filter = "all";
    const Project* proj = findProject(projects, projectId);
    std::string title = (projectId == 0)
        ? "📥 Inbox"
        : (proj ? "📁 " + proj->name : "Unknown project");

    while (true) {
        clearScreen();
        printHeader(title);
        std::cout << DIM << "  Filter: " << filter << "\n" << RESET;
        printTaskList(tasks, projectId, filter);
        printTaskMenu(filter);

        char cmd; std::cin >> cmd;

        switch (cmd) {
            case 'a': addTask(tasks, projects, projectId);    break;
            case 'd': toggleDone(tasks, projects);            break;
            case 'r': removeTask(tasks, projects);            break;
            case 'e': editTask(tasks, projects);              break;
            case 'p': changePriority(tasks, projects);        break;
            case 'm':
                moveTask(tasks, projects);
                saveAll(projects, tasks);
                break;
            case 'c': clearDone(tasks, projects, projectId);  break;
            case 'f': {
                std::cout << "Filter (all / active / done / high): ";
                std::cin >> filter;
                break;
            }
            case 'b': return;
            default:  std::cout << RED << "Unknown command.\n" << RESET;
        }

        pressEnter();
    }
}

// ── Project screen loop ───────────────────────────────────────────────────────

void projectScreen(std::vector<Project>& projects,
                   std::vector<Task>& tasks) {
    while (true) {
        clearScreen();
        printHeader("Projects");
        printProjectList(projects, tasks);
        printProjectMenu();

        std::string input;
        std::cin >> input;

        // numeric → open project/inbox
        bool isNum = !input.empty() &&
                     std::all_of(input.begin(), input.end(), ::isdigit);
        if (isNum) {
            int id = std::stoi(input);
            if (id == 0) {
                taskScreen(projects, tasks, 0);
                continue;
            }
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
            case 'n': newProject(projects, tasks);          break;
            case 'R': renameProject(projects, tasks);       break;
            case 'X': deleteProject(projects, tasks);       break;
            case 'q':
                std::cout << CYAN << "\nBye!\n" << RESET;
                return;
            default:
                std::cout << RED << "Unknown command.\n" << RESET;
        }
        pressEnter();
    }
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::vector<Project> projects;
    std::vector<Task>    tasks;
    loadAll(projects, tasks);
    projectScreen(projects, tasks);
    return 0;
}