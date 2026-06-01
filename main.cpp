/*
 * todo.cpp — Simple terminal task manager
 * Compile:  g++ -o todo todo.cpp
 * Run:      ./todo
 */

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>

struct Task {
    int         id;
    std::string text;
    std::string priority;   // low / medium / high
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
const std::string WHITE  = "\033[97m";

// ── Persistence ──────────────────────────────────────────────────────────────
const std::string FILE_PATH = "tasks.dat";

void saveTasks(const std::vector<Task>& tasks) {
    std::ofstream f(FILE_PATH);
    for (const auto& t : tasks)
        f << t.id << "|" << t.done << "|" << t.priority << "|" << t.text << "\n";
}

std::vector<Task> loadTasks() {
    std::vector<Task> tasks;
    std::ifstream f(FILE_PATH);
    if (!f.is_open()) return tasks;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        Task t;
        std::string doneStr;
        std::getline(ss, line,    '|'); t.id       = std::stoi(line);
        std::getline(ss, doneStr, '|'); t.done     = (doneStr == "1");
        std::getline(ss, t.priority, '|');
        std::getline(ss, t.text);
        tasks.push_back(t);
    }
    return tasks;
}

// ── Helpers ───────────────────────────────────────────────────────────────────
int nextId(const std::vector<Task>& tasks) {
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
    if (p == "high")   return "[HIGH  ]";
    if (p == "medium") return "[MED   ]";
    return "[LOW   ]";
}

void clearScreen() { std::cout << "\033[2J\033[H"; }

// ── Display ───────────────────────────────────────────────────────────────────
void printHeader() {
    std::cout << BOLD << CYAN
              << "╔══════════════════════════════════════════════╗\n"
              << "║        📋  Terminal Task Manager  v0.0       ║\n"
              << "╚══════════════════════════════════════════════╝\n"
              << RESET;
}

void listTasks(const std::vector<Task>& tasks, const std::string& filter = "all") {
    std::cout << "\n";
    int shown = 0;
    for (const auto& t : tasks) {
        if (filter == "active" &&  t.done) continue;
        if (filter == "done"   && !t.done) continue;
        if (filter == "high"   && (t.priority != "high" || t.done)) continue;

        std::string status = t.done ? GREEN + "✔" + RESET : " ";
        std::string style  = t.done ? DIM : "";
        std::cout << style
                  << std::setw(3) << t.id << "  "
                  << "[" << status << "]  "
                  << priColour(t.priority) << priTag(t.priority) << RESET
                  << "  " << style << t.text
                  << RESET << "\n";
        shown++;
    }
    if (shown == 0) std::cout << DIM << "  (no tasks to show)\n" << RESET;

    // stats
    int total  = tasks.size();
    int done   = 0, high = 0;
    for (const auto& t : tasks) {
        if (t.done) done++;
        if (t.priority == "high" && !t.done) high++;
    }
    std::cout << DIM
              << "\n  Total: " << total
              << "  |  Active: " << (total - done)
              << "  |  Done: " << done
              << "  |  High priority: " << high
              << "\n" << RESET;
}

void printMenu() {
    std::cout << BOLD << "\n── Commands ──────────────────────────────────────\n" << RESET
              << "  " << CYAN << "a" << RESET << "  Add task\n"
              << "  " << CYAN << "d" << RESET << "  Done / undone  (toggle)\n"
              << "  " << CYAN << "r" << RESET << "  Remove task\n"
              << "  " << CYAN << "e" << RESET << "  Edit task text\n"
              << "  " << CYAN << "p" << RESET << "  Change priority\n"
              << "  " << CYAN << "f" << RESET << "  Filter view\n"
              << "  " << CYAN << "c" << RESET << "  Clear all done tasks\n"
              << "  " << CYAN << "q" << RESET << "  Quit\n"
              << "──────────────────────────────────────────────────\n"
              << BOLD << "Enter command: " << RESET;
}

// ── Actions ───────────────────────────────────────────────────────────────────
void addTask(std::vector<Task>& tasks) {
    std::string text, pri;
    std::cout << "Task description: ";
    std::cin.ignore();
    std::getline(std::cin, text);
    if (text.empty()) { std::cout << RED << "Empty task — cancelled.\n" << RESET; return; }

    std::cout << "Priority (l=low / m=medium / h=high) [m]: ";
    std::getline(std::cin, pri);
    std::string priority = "medium";
    if (pri == "l" || pri == "low")    priority = "low";
    if (pri == "h" || pri == "high")   priority = "high";

    tasks.push_back({nextId(tasks), text, priority, false});
    saveTasks(tasks);
    std::cout << GREEN << "Task added.\n" << RESET;
}

void toggleDone(std::vector<Task>& tasks) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    for (auto& t : tasks) {
        if (t.id == id) {
            t.done = !t.done;
            saveTasks(tasks);
            std::cout << GREEN << "Task " << (t.done ? "marked done." : "marked active.") << "\n" << RESET;
            return;
        }
    }
    std::cout << RED << "ID not found.\n" << RESET;
}

void removeTask(std::vector<Task>& tasks) {
    int id;
    std::cout << "Task ID to remove: "; std::cin >> id;
    auto it = std::remove_if(tasks.begin(), tasks.end(), [id](const Task& t){ return t.id == id; });
    if (it == tasks.end()) { std::cout << RED << "ID not found.\n" << RESET; return; }
    tasks.erase(it, tasks.end());
    saveTasks(tasks);
    std::cout << GREEN << "Task removed.\n" << RESET;
}

void editTask(std::vector<Task>& tasks) {
    int id;
    std::cout << "Task ID to edit: "; std::cin >> id;
    std::cin.ignore();
    for (auto& t : tasks) {
        if (t.id == id) {
            std::cout << "New text [" << t.text << "]: ";
            std::string newText;
            std::getline(std::cin, newText);
            if (!newText.empty()) t.text = newText;
            saveTasks(tasks);
            std::cout << GREEN << "Task updated.\n" << RESET;
            return;
        }
    }
    std::cout << RED << "ID not found.\n" << RESET;
}

void changePriority(std::vector<Task>& tasks) {
    int id;
    std::cout << "Task ID: "; std::cin >> id;
    std::cin.ignore();
    for (auto& t : tasks) {
        if (t.id == id) {
            std::string pri;
            std::cout << "New priority (l/m/h): ";
            std::getline(std::cin, pri);
            if (pri == "l") t.priority = "low";
            else if (pri == "h") t.priority = "high";
            else t.priority = "medium";
            saveTasks(tasks);
            std::cout << GREEN << "Priority updated.\n" << RESET;
            return;
        }
    }
    std::cout << RED << "ID not found.\n" << RESET;
}

void clearDone(std::vector<Task>& tasks) {
    int before = tasks.size();
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [](const Task& t){ return t.done; }), tasks.end());
    saveTasks(tasks);
    std::cout << GREEN << "Removed " << (before - (int)tasks.size()) << " completed task(s).\n" << RESET;
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main() {
    auto tasks = loadTasks();
    std::string filter = "all";

    while (true) {
        clearScreen();
        printHeader();
        std::cout << DIM << "  View: " << filter << "\n" << RESET;
        listTasks(tasks, filter);
        printMenu();

        char cmd;
        std::cin >> cmd;

        switch (cmd) {
            case 'a': addTask(tasks);         break;
            case 'd': toggleDone(tasks);      break;
            case 'r': removeTask(tasks);      break;
            case 'e': editTask(tasks);        break;
            case 'p': changePriority(tasks);  break;
            case 'c': clearDone(tasks);       break;
            case 'f': {
                std::cout << "Filter (all / active / done / high): ";
                std::cin >> filter;
                break;
            }
            case 'q':
                std::cout << CYAN << "\nBye!\n" << RESET;
                return 0;
            default:
                std::cout << RED << "Unknown command.\n" << RESET;
        }

        std::cout << DIM << "\nPress Enter to continue…" << RESET;
        std::cin.ignore(); std::cin.get();
    }
}