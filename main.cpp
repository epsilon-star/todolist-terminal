/*
 * todo.cpp — Terminal Task Manager v4.0
 * Compile:  g++ -std=c++17 -o todo todo.cpp
 * Run:      ./todo
 *          ./todo add "task text !high due:2026-06-10 #tag ~weekly"
 *          ./todo done 5
 *          ./todo list
 *
 * Data layout  (~/.config/todo/<profile>/)
 *   tasks.dat         — tasks & projects
 *   tasks_archive.dat — archived completed tasks
 *   config.dat        — per-profile config
 *   auth.dat          — hashed PIN
 * Global:
 *   ~/.config/todo/profiles.dat  — list of profiles + active profile
 *
 * v4 additions over v3:
 *   🍅 Pomodoro timer (25/5 cycles, bell alert, logged per task)
 *   🔥 Streaks on recurring tasks
 *   ⏱  Time estimates (est vs actual)
 *   📈 Weekly review (Sat–Fri week)
 *   🔗 Task dependencies (blocked-by)
 *   🗄  Archive (done tasks moved, not deleted; browsable)
 *   ⚡ Quick-add CLI syntax
 *   👤 Profiles (per-profile data + theme)
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
#include <functional>
#include <csignal>
#include <thread>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <termios.h>

// ── Themes ────────────────────────────────────────────────────────────────────

struct Theme {
    std::string name;
    std::string reset, bold, dim;
    std::string red, green, yellow, cyan, magenta, blue, white;
    std::string bgRed, bgGreen, bgYellow;
    std::string accent, success, warn, err;
};

Theme makeTheme(const std::string& name) {
    Theme t;
    t.name    = name;
    t.reset   = "\033[0m";
    t.bold    = "\033[1m";
    t.dim     = "\033[2m";
    t.bgRed   = "\033[41m";
    t.bgGreen = "\033[42m";
    t.bgYellow= "\033[43m";

    if (name == "solarized") {
        t.red="\033[31m"; t.green="\033[32m"; t.yellow="\033[33m";
        t.cyan="\033[96m"; t.magenta="\033[35m"; t.blue="\033[94m"; t.white="\033[37m";
        t.accent="\033[96m"; t.success="\033[32m"; t.warn="\033[33m"; t.err="\033[31m";
    } else if (name == "nord") {
        t.red="\033[91m"; t.green="\033[92m"; t.yellow="\033[93m";
        t.cyan="\033[94m"; t.magenta="\033[95m"; t.blue="\033[94m"; t.white="\033[97m";
        t.accent="\033[94m"; t.success="\033[92m"; t.warn="\033[93m"; t.err="\033[91m";
    } else if (name == "mono") {
        t.red="\033[1m"; t.green=""; t.yellow="\033[1m"; t.cyan="\033[1m";
        t.magenta="\033[1m"; t.blue=""; t.white="";
        t.accent="\033[1m"; t.success=""; t.warn="\033[1m"; t.err="\033[1m";
    } else { // default
        t.red="\033[31m"; t.green="\033[32m"; t.yellow="\033[33m";
        t.cyan="\033[36m"; t.magenta="\033[35m"; t.blue="\033[34m"; t.white="\033[37m";
        t.accent="\033[36m"; t.success="\033[32m"; t.warn="\033[33m"; t.err="\033[31m";
    }
    return t;
}

Theme gTheme = makeTheme("default");

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
#define BG_GREEN gTheme.bgGreen
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
    int         projectId   = 0;
    int         parentId    = 0;
    std::string text;
    std::string priority    = "medium";
    bool        done        = false;
    std::string due;
    std::string tags;
    int         minutes     = 0;      // tracked actual minutes
    int         estMinutes  = 0;      // estimated minutes
    int         pomodoros   = 0;      // completed pomodoro sessions
    int         streak      = 0;      // consecutive on-time completions
    bool        pinned      = false;
    std::string recur;                // "" / "daily" / "weekly" / "monthly"
    std::string blockedBy;            // space-separated task IDs
    std::string notes;
    std::string lastCompleted;        // "YYYY-MM-DD" for streak tracking
    // runtime only
    bool        timerRunning = false;
    time_t      timerStart   = 0;
};

struct Config {
    std::string dataPath;
    std::string archivePath;
    std::string authPath;
    std::string defaultPriority = "medium";
    std::string theme           = "default";
    bool        showDone        = true;
    bool        confirmDelete   = true;
    bool        loginEnabled    = false;
    int         pomodoroWork    = 25;   // minutes
    int         pomodoroBreak   = 5;
    int         pomodoroLong    = 15;
};

struct Profile {
    std::string name;
    std::string dir;   // ~/.config/todo/<name>/
};

// ── Globals ───────────────────────────────────────────────────────────────────

Config              gConfig;
std::string         gConfigPath;
std::string         gProfilesPath;
std::string         gActiveProfile = "default";
std::optional<Task> gUndoTask;

// ── Path helpers ──────────────────────────────────────────────────────────────

std::string getHomeDir() {
    const char* h = getenv("HOME");
    if (h) return h;
    struct passwd* pw = getpwuid(getuid());
    return pw ? pw->pw_dir : ".";
}

void mkdirP(const std::string& path) {
    std::string tmp = path;
    for (size_t i = 1; i < tmp.size(); i++) {
        if (tmp[i] == '/') { tmp[i]='\0'; mkdir(tmp.c_str(),0755); tmp[i]='/'; }
    }
    mkdir(tmp.c_str(), 0755);
}

std::string profileBaseDir() {
    return getHomeDir() + "/.config/todo";
}

std::string profileDir(const std::string& profile) {
    return profileBaseDir() + "/" + profile;
}

void initPaths(const std::string& profile) {
    std::string dir    = profileDir(profile);
    gConfig.dataPath   = dir + "/tasks.dat";
    gConfig.archivePath= dir + "/tasks_archive.dat";
    gConfig.authPath   = dir + "/auth.dat";
    gConfigPath        = dir + "/config.dat";
    gProfilesPath      = profileBaseDir() + "/profiles.dat";
    mkdirP(dir);
}

// ── Date helpers ──────────────────────────────────────────────────────────────

std::string todayStr() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[32];
    snprintf(buf,sizeof(buf),"%04d-%02d-%02d",
             t->tm_year+1900, t->tm_mon+1, t->tm_mday);
    return buf;
}

std::string addDays(const std::string& d, int days) {
    if (d.empty()) return "";
    struct tm t={};
    sscanf(d.c_str(),"%d-%d-%d",&t.tm_year,&t.tm_mon,&t.tm_mday);
    t.tm_year-=1900; t.tm_mon-=1; t.tm_mday+=days; mktime(&t);
    char buf[32];
    snprintf(buf,sizeof(buf),"%04d-%02d-%02d",
             t.tm_year+1900,t.tm_mon+1,t.tm_mday);
    return buf;
}

std::string addMonths(const std::string& d, int m) {
    if (d.empty()) return "";
    struct tm t={};
    sscanf(d.c_str(),"%d-%d-%d",&t.tm_year,&t.tm_mon,&t.tm_mday);
    t.tm_year-=1900; t.tm_mon-=1; t.tm_mon+=m; mktime(&t);
    char buf[32];
    snprintf(buf,sizeof(buf),"%04d-%02d-%02d",
             t.tm_year+1900,t.tm_mon+1,t.tm_mday);
    return buf;
}

int dueDaysDiff(const std::string& due) {
    if (due.empty()) return 999;
    std::string today = todayStr();
    if (due == today) return 0;
    if (due < today)  return -1;
    struct tm t1={},t2={};
    sscanf(today.c_str(),"%d-%d-%d",&t1.tm_year,&t1.tm_mon,&t1.tm_mday);
    sscanf(due.c_str(),  "%d-%d-%d",&t2.tm_year,&t2.tm_mon,&t2.tm_mday);
    t1.tm_year-=1900; t1.tm_mon-=1;
    t2.tm_year-=1900; t2.tm_mon-=1;
    return (int)((mktime(&t2)-mktime(&t1))/86400);
}

std::string dueLabel(const std::string& due, bool done) {
    if (due.empty()) return "";
    int diff = dueDaysDiff(due);
    if (done)       return DIM+" ["+due+"]"+RESET;
    if (diff == -1) return RED+BOLD+" [OVERDUE:"+due+"]"+RESET;
    if (diff == 0)  return YELLOW+" [TODAY]"+RESET;
    return DIM+" ["+due+"]"+RESET;
}

bool isValidDate(const std::string& s) {
    if (s.size()!=10||s[4]!='-'||s[7]!='-') return false;
    for (int i:{0,1,2,3,5,6,8,9}) if(!isdigit(s[i])) return false;
    return true;
}

// Saturday-anchored week: returns the Saturday on or before the given date
std::string weekStart(const std::string& date) {
    struct tm t={};
    sscanf(date.c_str(),"%d-%d-%d",&t.tm_year,&t.tm_mon,&t.tm_mday);
    t.tm_year-=1900; t.tm_mon-=1; mktime(&t);
    // wday: 0=Sun,1=Mon,...,6=Sat
    int daysBack = (t.tm_wday + 1) % 7; // Sat=0,Sun=1,...,Fri=6
    return addDays(date, -daysBack);
}

std::string weekEnd(const std::string& start) { return addDays(start, 6); }

// ── String helpers ────────────────────────────────────────────────────────────

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return (a==std::string::npos) ? "" : s.substr(a, b-a+1);
}

std::string toLower(std::string s) {
    std::transform(s.begin(),s.end(),s.begin(),::tolower); return s;
}

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream ss(s);
    std::string w;
    while (ss >> w) v.push_back(w);
    return v;
}

std::string encodeNL(const std::string& s) {
    std::string o; for(char c:s){if(c=='\n')o+="\\n";else o+=c;} return o;
}
std::string decodeNL(const std::string& s) {
    std::string o;
    for(size_t i=0;i<s.size();i++){
        if(s[i]=='\\'&&i+1<s.size()&&s[i+1]=='n'){o+='\n';i++;}else o+=s[i];
    }
    return o;
}

// ── Priority / recur helpers ──────────────────────────────────────────────────

std::string parsePriority(const std::string& s) {
    if (s=="l"||s=="low")    return "low";
    if (s=="h"||s=="high")   return "high";
    if (s=="m"||s=="medium") return "medium";
    return gConfig.defaultPriority;
}

std::string parseRecur(const std::string& s) {
    if (s=="d"||s=="daily")   return "daily";
    if (s=="w"||s=="weekly")  return "weekly";
    if (s=="m"||s=="monthly") return "monthly";
    return "";
}

std::string priColour(const std::string& p) {
    if (p=="high")   return RED;
    if (p=="medium") return YELLOW;
    return GREEN;
}
std::string priTag(const std::string& p) {
    if (p=="high")   return "[HIGH]";
    if (p=="medium") return "[MED] ";
    return "[LOW] ";
}

std::string minutesToHMS(int mins) {
    if (mins<=0) return "";
    int h=mins/60, m=mins%60;
    char buf[32];
    if(h>0) snprintf(buf,sizeof(buf)," ⏱%dh%02dm",h,m);
    else     snprintf(buf,sizeof(buf)," ⏱%dm",m);
    return DIM+std::string(buf)+RESET;
}

std::string colorTags(const std::string& tags) {
    if (tags.empty()) return "";
    std::string out=" ";
    for(auto& t:splitWords(tags)) out+=CYAN+t+RESET+" ";
    return out;
}

// ── Auth ──────────────────────────────────────────────────────────────────────

std::string hashPin(const std::string& pin) {
    size_t h=5381;
    for(char c:pin) h=((h<<5)+h)+(unsigned char)c;
    h^=(h>>16); h*=0x45d9f3b; h^=(h>>16);
    std::ostringstream oss;
    oss<<std::hex<<std::setw(16)<<std::setfill('0')<<h;
    return oss.str();
}

void saveAuth(const std::string& hash) {
    std::ofstream f(gConfig.authPath); f<<hash<<"\n";
}
std::string loadAuth() {
    std::ifstream f(gConfig.authPath); if(!f.is_open())return "";
    std::string h; std::getline(f,h); return h;
}

std::string readPin(const std::string& prompt) {
    std::cout<<prompt; std::cout.flush();
    struct termios old,neo;
    tcgetattr(STDIN_FILENO,&old); neo=old;
    neo.c_lflag&=~ECHO; tcsetattr(STDIN_FILENO,TCSANOW,&neo);
    std::string pin; std::getline(std::cin,pin);
    tcsetattr(STDIN_FILENO,TCSANOW,&old);
    std::cout<<"\n"; return pin;
}

bool loginScreen() {
    std::string stored=loadAuth();
    if(stored.empty()) return true;
    std::cout<<"\n"<<BOLD<<ACCENT<<"  🔐 Login\n"<<RESET<<"\n";
    for(int i=1;i<=3;i++){
        std::string pin=readPin("  PIN: ");
        if(hashPin(pin)==stored){std::cout<<SUCCESS<<"  ✔ Access granted.\n"<<RESET;return true;}
        if(i<3) std::cout<<ERR<<"  ✘ Wrong. "<<(3-i)<<" attempt(s) left.\n"<<RESET;
    }
    std::cout<<ERR<<BOLD<<"\n  Too many attempts. Bye.\n"<<RESET;
    return false;
}

void setupPin(bool disable=false){
    if(disable){
        gConfig.loginEnabled=false;
        remove(gConfig.authPath.c_str());
        std::cout<<SUCCESS<<"Login disabled.\n"<<RESET; return;
    }
    std::string p1=readPin("  New PIN: "), p2=readPin("  Confirm: ");
    if(p1!=p2){std::cout<<ERR<<"Mismatch.\n"<<RESET;return;}
    if(p1.empty()){std::cout<<ERR<<"Empty PIN.\n"<<RESET;return;}
    saveAuth(hashPin(p1)); gConfig.loginEnabled=true;
    std::cout<<SUCCESS<<"PIN set.\n"<<RESET;
}

// ── Config ────────────────────────────────────────────────────────────────────

void saveConfig(){
    std::ofstream f(gConfigPath);
    f<<"CONFIG|defaultPriority|"<<gConfig.defaultPriority<<"\n";
    f<<"CONFIG|showDone|"       <<gConfig.showDone       <<"\n";
    f<<"CONFIG|confirmDelete|"  <<gConfig.confirmDelete  <<"\n";
    f<<"CONFIG|theme|"          <<gConfig.theme          <<"\n";
    f<<"CONFIG|loginEnabled|"   <<gConfig.loginEnabled   <<"\n";
    f<<"CONFIG|pomodoroWork|"   <<gConfig.pomodoroWork   <<"\n";
    f<<"CONFIG|pomodoroBreak|"  <<gConfig.pomodoroBreak  <<"\n";
    f<<"CONFIG|pomodoroLong|"   <<gConfig.pomodoroLong   <<"\n";
}

void loadConfig(){
    std::ifstream f(gConfigPath); if(!f.is_open()) return;
    std::string line;
    while(std::getline(f,line)){
        std::istringstream ss(line);
        std::string kind,key,val;
        std::getline(ss,kind,'|'); std::getline(ss,key,'|'); std::getline(ss,val);
        if(kind!="CONFIG") continue;
        if(key=="defaultPriority") gConfig.defaultPriority=val;
        if(key=="showDone")        gConfig.showDone=(val=="1");
        if(key=="confirmDelete")   gConfig.confirmDelete=(val=="1");
        if(key=="theme")           gConfig.theme=val;
        if(key=="loginEnabled")    gConfig.loginEnabled=(val=="1");
        if(key=="pomodoroWork")    gConfig.pomodoroWork=std::stoi(val);
        if(key=="pomodoroBreak")   gConfig.pomodoroBreak=std::stoi(val);
        if(key=="pomodoroLong")    gConfig.pomodoroLong=std::stoi(val);
    }
}

// ── Profiles ──────────────────────────────────────────────────────────────────

std::vector<std::string> listProfiles(){
    std::vector<std::string> v;
    std::ifstream f(gProfilesPath); if(!f.is_open()){v.push_back("default");return v;}
    std::string line;
    while(std::getline(f,line)) if(!line.empty()&&line[0]!='#') v.push_back(trim(line));
    if(v.empty()) v.push_back("default");
    return v;
}

std::string loadActiveProfile(){
    std::ifstream f(profileBaseDir()+"/active_profile.dat");
    if(!f.is_open()) return "default";
    std::string p; std::getline(f,p); return trim(p).empty()?"default":trim(p);
}

void saveActiveProfile(const std::string& p){
    std::ofstream f(profileBaseDir()+"/active_profile.dat"); f<<p<<"\n";
}

void saveProfileList(const std::vector<std::string>& profiles){
    std::ofstream f(gProfilesPath);
    for(auto& p:profiles) f<<p<<"\n";
}

// ── Persistence ───────────────────────────────────────────────────────────────

// v4 TASK format:
// TASK|id|projectId|parentId|done|priority|due|tags|minutes|pinned|recur|
//      estMinutes|pomodoros|streak|blockedBy|lastCompleted|text|||notes

void saveTaskLine(std::ofstream& f, const Task& t){
    f<<"TASK|"
     <<t.id<<"|"<<t.projectId<<"|"<<t.parentId<<"|"<<t.done<<"|"
     <<t.priority<<"|"<<t.due<<"|"<<t.tags<<"|"<<t.minutes<<"|"
     <<t.pinned<<"|"<<t.recur<<"|"
     <<t.estMinutes<<"|"<<t.pomodoros<<"|"<<t.streak<<"|"
     <<t.blockedBy<<"|"<<t.lastCompleted<<"|"
     <<t.text<<"|||"<<encodeNL(t.notes)<<"\n";
}

void saveAll(const std::vector<Project>& projects, const std::vector<Task>& tasks){
    mkdirP(gConfig.dataPath.substr(0,gConfig.dataPath.rfind('/')));
    std::ofstream f(gConfig.dataPath);
    for(auto& p:projects) f<<"PROJECT|"<<p.id<<"|"<<p.name<<"\n";
    for(auto& t:tasks)    saveTaskLine(f,t);
}

void saveArchive(const std::vector<Task>& archive){
    std::ofstream f(gConfig.archivePath);
    for(auto& t:archive) saveTaskLine(f,t);
}

Task parseTaskLine(const std::string& line){
    Task t;
    // Detect format version by counting pipes before "|||"
    // v4 has 16 pipes before text, v3 has 11, v2 has 8
    bool isV4 = (line.find("|||")!=std::string::npos);

    std::vector<std::string> fields;
    {
        std::istringstream ss(line);
        std::string tok;
        while(std::getline(ss,tok,'|')) fields.push_back(tok);
    }

    auto safeInt=[&](size_t i)->int{
        if(i>=fields.size()||fields[i].empty()) return 0;
        try{ return std::stoi(fields[i]); } catch(...){ return 0; }
    };

    if(isV4 && fields.size()>=17){
        // v4 format
        t.id            = safeInt(1);
        t.projectId     = safeInt(2);
        t.parentId      = safeInt(3);
        t.done          = (fields[4]=="1");
        t.priority      = fields[5];
        t.due           = fields[6];
        t.tags          = fields[7];
        t.minutes       = safeInt(8);
        t.pinned        = (fields[9]=="1");
        t.recur         = fields[10];
        t.estMinutes    = safeInt(11);
        t.pomodoros     = safeInt(12);
        t.streak        = safeInt(13);
        t.blockedBy     = fields[14];
        t.lastCompleted = fields[15];
        // text+notes after 16th pipe
        size_t ts=0; int pc=0;
        for(size_t i=0;i<line.size();i++){
            if(line[i]=='|'){pc++;if(pc==16){ts=i+1;break;}}
        }
        std::string rest=line.substr(ts);
        size_t sep=rest.find("|||");
        if(sep!=std::string::npos){
            t.text=rest.substr(0,sep);
            t.notes=decodeNL(rest.substr(sep+3));
        } else t.text=rest;

    } else if(isV4 && fields.size()>=12){
        // v3 format (has ||| but fewer fields)
        t.id        = safeInt(1);
        t.projectId = safeInt(2);
        t.parentId  = safeInt(3);
        t.done      = (fields[4]=="1");
        t.priority  = fields[5];
        t.due       = fields[6];
        t.tags      = fields[7];
        t.minutes   = safeInt(8);
        t.pinned    = (fields.size()>9 && fields[9]=="1");
        t.recur     = fields.size()>10 ? fields[10] : "";
        size_t ts=0; int pc=0;
        for(size_t i=0;i<line.size();i++){
            if(line[i]=='|'){pc++;if(pc==11){ts=i+1;break;}}
        }
        std::string rest=line.substr(ts);
        size_t sep=rest.find("|||");
        if(sep!=std::string::npos){
            t.text=rest.substr(0,sep);
            t.notes=decodeNL(rest.substr(sep+3));
        } else t.text=rest;

    } else {
        // v2 format
        t.id        = safeInt(1);
        t.projectId = safeInt(2);
        t.done      = (fields[3]=="1");
        t.priority  = fields.size()>4 ? fields[4] : "medium";
        t.due       = fields.size()>5 ? fields[5] : "";
        t.tags      = fields.size()>6 ? fields[6] : "";
        t.minutes   = safeInt(7);
        size_t ts=0; int pc=0;
        for(size_t i=0;i<line.size();i++){
            if(line[i]=='|'){pc++;if(pc==8){ts=i+1;break;}}
        }
        t.text = line.substr(ts);
    }
    return t;
}

void loadAll(std::vector<Project>& projects, std::vector<Task>& tasks){
    std::ifstream f(gConfig.dataPath); if(!f.is_open()) return;
    std::string line;
    while(std::getline(f,line)){
        if(line.empty()) continue;
        if(line.substr(0,8)=="PROJECT|"){
            std::istringstream ss(line);
            std::string k,idStr,name;
            std::getline(ss,k,'|'); std::getline(ss,idStr,'|');
            size_t ns=0;int pc=0;
            for(size_t i=0;i<line.size();i++){
                if(line[i]=='|'){pc++;if(pc==2){ns=i+1;break;}}
            }
            Project p; try{p.id=std::stoi(idStr);}catch(...){continue;}
            p.name=line.substr(ns);
            projects.push_back(p);
        } else if(line.substr(0,5)=="TASK|"){
            try{ tasks.push_back(parseTaskLine(line)); }
            catch(const std::exception& e){
                std::cerr<<"Warning: skipping bad task: "<<e.what()<<"\n";
            }
        }
    }
}

void loadArchive(std::vector<Task>& archive){
    std::ifstream f(gConfig.archivePath); if(!f.is_open()) return;
    std::string line;
    while(std::getline(f,line)){
        if(line.substr(0,5)=="TASK|"){
            try{ archive.push_back(parseTaskLine(line)); }
            catch(...){};
        }
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

int nextProjId(const std::vector<Project>& v){
    int mx=0; for(auto& p:v) mx=std::max(mx,p.id); return mx+1;
}
int nextTaskId(const std::vector<Task>& v){
    int mx=0; for(auto& t:v) mx=std::max(mx,t.id); return mx+1;
}

const Project* findProject(const std::vector<Project>& v,int id){
    for(auto& p:v) if(p.id==id) return &p;
    return nullptr;
}
Task* findTask(std::vector<Task>& v,int id){
    for(auto& t:v) if(t.id==id) return &t;
    return nullptr;
}
const Task* findTaskC(const std::vector<Task>& v,int id){
    for(auto& t:v) if(t.id==id) return &t;
    return nullptr;
}

bool isBlocked(const Task& t, const std::vector<Task>& tasks){
    if(t.blockedBy.empty()) return false;
    for(auto& idStr:splitWords(t.blockedBy)){
        try{
            int bid=std::stoi(idStr);
            const Task* dep=findTaskC(tasks,bid);
            if(dep && !dep->done) return true;
        } catch(...){}
    }
    return false;
}

void clearScreen(){ std::cout<<"\033[2J\033[H"; }
void pressEnter(){
    std::cout<<DIM<<"\nPress Enter to continue…"<<RESET;
    std::cin.ignore(10000,'\n'); std::cin.get();
}

void printHeader(const std::string& sub=""){
    std::cout<<BOLD<<ACCENT
             <<"╔══════════════════════════════════════════════╗\n"
             <<"║      📋  Terminal Task Manager  v4.0         ║\n"
             <<"╚══════════════════════════════════════════════╝\n"
             <<RESET;
    if(!sub.empty()) std::cout<<BOLD<<"  "<<sub<<"\n"<<RESET;
    std::cout<<DIM<<"  Profile: "<<gActiveProfile<<"   Theme: "
             <<gConfig.theme<<"   Today: "<<todayStr()<<"\n"<<RESET;
}

// ── Recurring / Streak ────────────────────────────────────────────────────────

void spawnNextRecurrence(std::vector<Task>& tasks,
                         const std::vector<Project>& /*proj*/,
                         const Task& t){
    if(t.recur.empty()||t.due.empty()) return;
    std::string nextDue;
    if     (t.recur=="daily")   nextDue=addDays(t.due,1);
    else if(t.recur=="weekly")  nextDue=addDays(t.due,7);
    else if(t.recur=="monthly") nextDue=addMonths(t.due,1);

    for(auto& ex:tasks)
        if(ex.text==t.text&&ex.projectId==t.projectId&&ex.due==nextDue&&!ex.done) return;

    Task next=t;
    next.id=nextTaskId(tasks);
    next.done=false; next.due=nextDue;
    next.minutes=0; next.pomodoros=0;
    next.timerRunning=false; next.timerStart=0;
    // preserve streak into next instance
    tasks.push_back(next);
    std::cout<<CYAN<<"  ↻ Next recurrence: "<<nextDue<<"\n"<<RESET;
}

// Update streak when completing a recurring task
void updateStreak(Task& t){
    if(t.recur.empty()) return;
    std::string today=todayStr();
    bool onTime=(t.due.empty()||today<=t.due);
    if(onTime){
        t.streak++;
        std::cout<<YELLOW<<"  🔥 Streak: "<<t.streak<<" on-time completions!\n"<<RESET;
    } else {
        if(t.streak>0)
            std::cout<<DIM<<"  Streak reset (was "<<t.streak<<"). Task was overdue.\n"<<RESET;
        t.streak=0;
    }
    t.lastCompleted=today;
}

// ── Quick-add parser ──────────────────────────────────────────────────────────
// Syntax: "task text !high/!med/!low due:YYYY-MM-DD #tag ~daily/weekly/monthly"
// Parts can be in any order; everything else is the task title.

struct QuickTask {
    std::string text, priority, due, tags, recur;
    int estMinutes=0;
};

QuickTask parseQuick(const std::string& input){
    QuickTask q; q.priority=gConfig.defaultPriority;
    std::vector<std::string> words=splitWords(input);
    std::vector<std::string> titleWords;
    for(auto& w:words){
        if(w=="!high"||w=="!h"){ q.priority="high"; }
        else if(w=="!med"||w=="!m"||w=="!medium"){ q.priority="medium"; }
        else if(w=="!low"||w=="!l"){ q.priority="low"; }
        else if(w.size()>4&&w.substr(0,4)=="due:"){ q.due=w.substr(4); }
        else if(w[0]=='#'){ if(!q.tags.empty())q.tags+=" "; q.tags+=w; }
        else if(w=="~daily"||w=="~d"){ q.recur="daily"; }
        else if(w=="~weekly"||w=="~w"){ q.recur="weekly"; }
        else if(w=="~monthly"||w=="~m"){ q.recur="monthly"; }
        else if(w.size()>4&&w.substr(0,4)=="est:"){
            try{q.estMinutes=std::stoi(w.substr(4));}catch(...){}
        }
        else titleWords.push_back(w);
    }
    for(size_t i=0;i<titleWords.size();i++){
        if(i) q.text+=" ";
        q.text+=titleWords[i];
    }
    if(!q.due.empty()&&!isValidDate(q.due)){
        std::cout<<WARN<<"Warning: invalid date '"<<q.due<<"' ignored.\n"<<RESET;
        q.due="";
    }
    return q;
}

// ── Pomodoro ──────────────────────────────────────────────────────────────────

void pomodoroSession(std::vector<Task>& tasks,
                     std::vector<Project>& projects){
    clearScreen();
    printHeader("🍅 Pomodoro");
    std::cout<<"\n";

    // Show tasks
    std::cout<<BOLD<<"  Active tasks:\n"<<RESET;
    for(auto& t:tasks){
        if(t.done||t.parentId!=0) continue;
        std::cout<<"  "<<std::setw(4)<<t.id<<"  "
                 <<priColour(t.priority)<<priTag(t.priority)<<RESET
                 <<"  "<<t.text;
        if(t.pomodoros>0)
            std::cout<<CYAN<<"  ["<<t.pomodoros<<" 🍅]"<<RESET;
        std::cout<<"\n";
    }

    int tid;
    std::cout<<"\n  Task ID (0 to cancel): "; std::cin>>tid;
    if(tid==0) return;
    Task* t=findTask(tasks,tid);
    if(!t){ std::cout<<ERR<<"Not found.\n"<<RESET; return; }

    int cycle=0;
    while(true){
        cycle++;
        int workMins=gConfig.pomodoroWork;
        clearScreen();
        printHeader("🍅 Pomodoro — "+t->text);
        std::cout<<"\n"
                 <<BOLD<<YELLOW<<"  🍅 Session "<<cycle
                 <<"  ("<<workMins<<" min work)\n"<<RESET
                 <<"  Task: "<<t->text<<"\n"
                 <<"  Completed pomodoros: "<<t->pomodoros<<"\n\n"
                 <<DIM<<"  Press Enter to start…"<<RESET;
        std::cin.ignore(10000,'\n'); std::cin.get();

        // Countdown
        auto startTime=std::chrono::steady_clock::now();
        int totalSecs=workMins*60;
        while(true){
            auto now=std::chrono::steady_clock::now();
            int elapsed=(int)std::chrono::duration_cast<std::chrono::seconds>(now-startTime).count();
            int remaining=totalSecs-elapsed;
            if(remaining<0) remaining=0;
            int m=remaining/60, s=remaining%60;
            std::cout<<"\r  "<<BOLD<<YELLOW<<"⏱  "
                     <<std::setw(2)<<std::setfill('0')<<m<<":"
                     <<std::setw(2)<<std::setfill('0')<<s
                     <<"  remaining"<<RESET<<"  (Ctrl+C to abort session)    "
                     <<std::flush;
            if(remaining==0) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Session complete
        std::cout<<"\n\n"
                 <<BG_GREEN<<BOLD<<"  ✔ Work session complete!  "<<RESET<<"\a\n";
        t->pomodoros++;
        t->minutes+=workMins;
        saveAll(projects,tasks);

        // Break
        int breakMins=(cycle%4==0)?gConfig.pomodoroLong:gConfig.pomodoroBreak;
        std::string breakLabel=(cycle%4==0)?"Long break":"Short break";
        std::cout<<"\n  "<<CYAN<<breakLabel<<" — "<<breakMins<<" min"<<RESET<<"\n"
                 <<DIM<<"  Press Enter to start break…"<<RESET;
        std::cin.get();

        auto bStart=std::chrono::steady_clock::now();
        int bSecs=breakMins*60;
        while(true){
            auto now=std::chrono::steady_clock::now();
            int elapsed=(int)std::chrono::duration_cast<std::chrono::seconds>(now-bStart).count();
            int remaining=bSecs-elapsed;
            if(remaining<0) remaining=0;
            int mm=remaining/60, ss=remaining%60;
            std::cout<<"\r  "<<BOLD<<CYAN<<"☕  "
                     <<std::setw(2)<<std::setfill('0')<<mm<<":"
                     <<std::setw(2)<<std::setfill('0')<<ss
                     <<"  break"<<RESET<<"                    "<<std::flush;
            if(remaining==0) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::cout<<"\n\n"<<BG_GREEN<<BOLD<<"  ✔ Break over!"<<RESET<<"\a\n";

        std::cout<<"\n  Continue another pomodoro for this task? (y/N): ";
        char c; std::cin>>c;
        if(c!='y'&&c!='Y') break;
    }

    std::cout<<"\n"<<SUCCESS<<"  Session done. Total 🍅 on this task: "
             <<t->pomodoros<<"\n"<<RESET;
    pressEnter();
}

// ── Archive ───────────────────────────────────────────────────────────────────

void archiveDone(std::vector<Task>& tasks,
                 std::vector<Project>& projects,
                 int projectId){
    // Load existing archive
    std::vector<Task> archive;
    loadArchive(archive);

    int count=0;
    std::vector<Task> remaining;
    for(auto& t:tasks){
        if(t.done&&t.projectId==projectId){
            archive.push_back(t); count++;
        } else {
            remaining.push_back(t);
        }
    }
    tasks=remaining;
    saveAll(projects,tasks);
    saveArchive(archive);
    std::cout<<SUCCESS<<count<<" task(s) archived.\n"<<RESET;
}

void browseArchive(){
    std::vector<Task> archive;
    loadArchive(archive);

    clearScreen();
    printHeader("🗄  Archive");
    if(archive.empty()){
        std::cout<<DIM<<"\n  Archive is empty.\n"<<RESET;
        pressEnter(); return;
    }

    std::cout<<"\n"<<DIM<<"  "<<archive.size()<<" archived task(s):\n\n"<<RESET;
    for(auto& t:archive){
        std::cout<<DIM
                 <<std::setw(4)<<t.id<<"  ✔  "
                 <<priTag(t.priority)<<"  "<<t.text;
        if(!t.due.empty())   std::cout<<"  [due:"<<t.due<<"]";
        if(t.minutes>0)      std::cout<<minutesToHMS(t.minutes);
        if(t.pomodoros>0)    std::cout<<"  ["<<t.pomodoros<<"🍅]";
        if(t.streak>0)       std::cout<<"  [🔥"<<t.streak<<"]";
        std::cout<<"\n"<<RESET;
    }

    std::cout<<"\n  "<<ACCENT<<"r"<<RESET<<" Restore a task   "
             <<ACCENT<<"X"<<RESET<<" Clear all archive   "
             <<ACCENT<<"b"<<RESET<<" Back\n"
             <<BOLD<<"Command: "<<RESET;

    char cmd; std::cin>>cmd;
    if(cmd=='r'){
        // Restore: not full impl here — just notify
        std::cout<<WARN<<"Restore not yet impl in this view — edit archive file manually.\n"<<RESET;
        pressEnter();
    } else if(cmd=='X'){
        std::cout<<"Clear entire archive? (y/N): ";
        char c; std::cin>>c;
        if(c=='y'||c=='Y'){
            std::ofstream f(gConfig.archivePath); // truncate
            std::cout<<SUCCESS<<"Archive cleared.\n"<<RESET;
        }
        pressEnter();
    }
}

// ── Weekly review (Sat–Fri) ───────────────────────────────────────────────────

void weeklyReview(const std::vector<Task>& tasks,
                  const std::vector<Project>& /*projects*/){
    clearScreen();
    printHeader("📈 Weekly Review");

    std::string today=todayStr();
    std::string wStart=weekStart(today);
    std::string wEnd=weekEnd(wStart);

    std::cout<<"\n"<<BOLD<<"  Week: "<<wStart<<" → "<<wEnd<<"\n\n"<<RESET;

    // Completed this week
    std::vector<const Task*> completed, slipped, overdueTasks;
    int totalMinutes=0, totalPomodoros=0;
    std::vector<std::pair<int,std::string>> streakHighlights; // streak,name

    // Load archive for completed-this-week
    std::vector<Task> archive;
    loadArchive(archive);

    auto inWeek=[&](const std::string& d)->bool{
        return !d.empty() && d>=wStart && d<=wEnd;
    };

    for(auto& t:archive){
        if(inWeek(t.lastCompleted)){
            completed.push_back(&t);
            totalMinutes+=t.minutes;
            totalPomodoros+=t.pomodoros;
        }
    }
    // Also check active tasks marked done
    for(auto& t:tasks){
        if(t.done&&inWeek(t.lastCompleted)){
            completed.push_back(&t);
            totalMinutes+=t.minutes;
            totalPomodoros+=t.pomodoros;
        }
        if(!t.done&&!t.due.empty()&&t.due>=wStart&&t.due<=wEnd&&dueDaysDiff(t.due)==-1)
            slipped.push_back(&t);
        if(!t.done&&dueDaysDiff(t.due)==-1)
            overdueTasks.push_back(&t);
        if(t.streak>=3)
            streakHighlights.push_back({t.streak,t.text});
    }

    // Completed
    std::cout<<SUCCESS<<BOLD<<"  ✔ Completed ("<<completed.size()<<")\n"<<RESET;
    if(completed.empty())
        std::cout<<DIM<<"    (none recorded this week)\n"<<RESET;
    for(auto* t:completed){
        std::cout<<"    "<<priTag(t->priority)<<"  "<<t->text;
        if(t->minutes>0) std::cout<<minutesToHMS(t->minutes);
        if(t->pomodoros>0) std::cout<<CYAN<<"  ["<<t->pomodoros<<"🍅]"<<RESET;
        std::cout<<"\n";
    }

    // Slipped
    std::cout<<"\n"<<WARN<<BOLD<<"  ⚠ Slipped / Missed ("<<slipped.size()<<")\n"<<RESET;
    for(auto* t:slipped)
        std::cout<<DIM<<"    "<<t->text<<"  [due:"<<t->due<<"]\n"<<RESET;
    if(slipped.empty()) std::cout<<DIM<<"    (none)\n"<<RESET;

    // Still overdue
    std::cout<<"\n"<<RED<<BOLD<<"  ✘ Still overdue ("<<overdueTasks.size()<<")\n"<<RESET;
    for(auto* t:overdueTasks)
        std::cout<<RED<<"    "<<t->text<<"  [due:"<<t->due<<"]\n"<<RESET;
    if(overdueTasks.empty()) std::cout<<DIM<<"    (none)\n"<<RESET;

    // Time & pomodoros
    std::cout<<"\n"<<BOLD<<"  ⏱ Time tracked\n"<<RESET;
    if(totalMinutes>0)
        std::cout<<"    "<<CYAN<<(totalMinutes/60)<<"h "<<(totalMinutes%60)<<"m"
                 <<RESET<<" across "<<completed.size()<<" tasks\n";
    else
        std::cout<<DIM<<"    (no time logged)\n"<<RESET;
    if(totalPomodoros>0)
        std::cout<<"    "<<CYAN<<totalPomodoros<<" 🍅 pomodoros"<<RESET<<"\n";

    // Streaks
    if(!streakHighlights.empty()){
        std::sort(streakHighlights.begin(),streakHighlights.end(),
                  [](auto& a,auto& b){return a.first>b.first;});
        std::cout<<"\n"<<BOLD<<"  🔥 Active streaks\n"<<RESET;
        for(auto& [s,name]:streakHighlights)
            std::cout<<"    🔥"<<YELLOW<<s<<RESET<<"  "<<name<<"\n";
    }

    // Simple score
    int score=0;
    if(!completed.empty()) score+=std::min((int)completed.size()*10,50);
    if(slipped.empty())    score+=20;
    if(overdueTasks.empty()) score+=20;
    if(totalPomodoros>0)   score+=10;
    std::cout<<"\n"<<BOLD<<"  📊 Week score: "<<RESET
             <<(score>=80?SUCCESS:score>=50?WARN:ERR)
             <<score<<"/100"<<RESET<<"\n";

    pressEnter();
}

// ── Dependencies ──────────────────────────────────────────────────────────────

void setDependencies(std::vector<Task>& tasks,
                     std::vector<Project>& projects){
    int id;
    std::cout<<"Task ID: "; std::cin>>id;
    std::cin.ignore(10000,'\n');
    Task* t=findTask(tasks,id);
    if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}

    std::cout<<"This task is blocked by (space-separated IDs, blank to clear) ["
             <<t->blockedBy<<"]: ";
    std::string s; std::getline(std::cin,s);
    t->blockedBy=s;
    saveAll(projects,tasks);
    std::cout<<SUCCESS<<"Dependencies updated.\n"<<RESET;
}

// ── Search ────────────────────────────────────────────────────────────────────

void searchTasks(const std::vector<Task>& tasks,
                 const std::vector<Project>& projects){
    std::cout<<"Search: ";
    std::cin.ignore(10000,'\n');
    std::string kw; std::getline(std::cin,kw);
    if(kw.empty()) return;
    std::string kwl=toLower(kw);

    clearScreen(); printHeader("🔍 \""+kw+"\"");
    std::cout<<"\n";
    int found=0;
    for(auto& t:tasks){
        std::string tl=toLower(t.text),gl=toLower(t.tags),nl=toLower(t.notes);
        if(tl.find(kwl)==std::string::npos&&
           gl.find(kwl)==std::string::npos&&
           nl.find(kwl)==std::string::npos) continue;
        const Project* p=findProject(projects,t.projectId);
        std::string proj=(t.projectId==0)?"Inbox":(p?p->name:"?");
        std::string style=t.done?DIM:"";
        std::cout<<style
                 <<std::setw(4)<<t.id<<"  ["<<(t.done?GREEN+"✔"+RESET:" ")<<"]  "
                 <<priColour(t.priority)<<priTag(t.priority)<<RESET
                 <<"  "<<style<<t.text<<RESET
                 <<dueLabel(t.due,t.done)<<colorTags(t.tags)
                 <<DIM<<"  ["<<proj<<"]"<<RESET<<"\n";
        found++;
    }
    if(!found) std::cout<<DIM<<"  No results.\n"<<RESET;
    pressEnter();
}

// ── Stats ─────────────────────────────────────────────────────────────────────

void showStats(const std::vector<Task>& tasks,
               const std::vector<Project>& projects){
    clearScreen(); printHeader("📊 Statistics");
    std::cout<<"\n";

    int total=0,done=0,overdue=0,today=0,pinned=0,recurring=0,blocked=0,pomodoros=0;
    int totalMins=0,totalEst=0;
    int byPri[3]={0,0,0};

    for(auto& t:tasks){
        total++; if(t.done)done++; if(t.pinned)pinned++;
        if(!t.recur.empty())recurring++;
        if(!t.blockedBy.empty()&&!t.done)blocked++;
        pomodoros+=t.pomodoros; totalMins+=t.minutes; totalEst+=t.estMinutes;
        if(!t.done){
            int d=dueDaysDiff(t.due);
            if(d==-1)overdue++;
            if(d==0)today++;
        }
        if(t.priority=="low")byPri[0]++;
        else if(t.priority=="medium")byPri[1]++;
        else byPri[2]++;
    }

    int pct=(total>0)?(done*100/total):0;
    int filled=pct/5;
    std::string bar="[";
    for(int i=0;i<20;i++) bar+=(i<filled?"█":"░");
    bar+="]";

    std::cout<<BOLD<<"  Completion\n"<<RESET;
    std::cout<<"  "<<SUCCESS<<bar<<" "<<pct<<"%"<<RESET
             <<DIM<<"  ("<<done<<"/"<<total<<")\n\n"<<RESET;

    std::cout<<BOLD<<"  Priority\n"<<RESET;
    std::cout<<"  "<<RED<<"[HIGH] "<<RESET<<byPri[2]<<"   "
             <<YELLOW<<"[MED]  "<<RESET<<byPri[1]<<"   "
             <<GREEN<<"[LOW]  "<<RESET<<byPri[0]<<"\n\n";

    if(overdue>0||today>0){
        std::cout<<BOLD<<"  Due dates\n"<<RESET;
        if(overdue>0) std::cout<<"  "<<RED<<"⚠ Overdue:   "<<overdue<<RESET<<"\n";
        if(today>0)   std::cout<<"  "<<YELLOW<<"● Today:    "<<today<<RESET<<"\n";
        std::cout<<"\n";
    }

    std::cout<<BOLD<<"  Tracking\n"<<RESET;
    std::cout<<"  ⏱ Tracked:  "<<CYAN<<(totalMins/60)<<"h "<<(totalMins%60)<<"m"<<RESET<<"\n";
    if(totalEst>0)
        std::cout<<"  📐 Estimated: "<<CYAN<<(totalEst/60)<<"h "<<(totalEst%60)<<"m"<<RESET<<"\n";
    std::cout<<"  🍅 Pomodoros: "<<CYAN<<pomodoros<<RESET<<"\n\n";

    std::cout<<BOLD<<"  Extra\n"<<RESET;
    std::cout<<"  📌 Pinned:    "<<pinned<<"\n";
    std::cout<<"  ↻  Recurring: "<<recurring<<"\n";
    std::cout<<"  ⛔ Blocked:   "<<blocked<<"\n\n";

    std::cout<<BOLD<<"  Per project\n"<<RESET;
    auto row=[&](int pid,const std::string& label){
        int t2=0,d2=0;
        for(auto& t:tasks) if(t.projectId==pid){t2++;if(t.done)d2++;}
        int p2=t2>0?d2*100/t2:0;
        std::cout<<"  "<<std::setw(20)<<std::left<<label
                 <<DIM<<"  "<<(t2-d2)<<" active / "<<t2<<" total  "<<p2<<"%\n"<<RESET;
    };
    row(0,"Inbox");
    for(auto& p:projects) row(p.id,p.name);

    pressEnter();
}

// ── Export ────────────────────────────────────────────────────────────────────

void exportTasks(const std::vector<Task>& tasks,
                 const std::vector<Project>& projects){
    std::cout<<"Filename [todo_export.md]: ";
    std::cin.ignore(10000,'\n');
    std::string fname; std::getline(std::cin,fname);
    if(fname.empty()) fname="todo_export.md";
    std::ofstream f(fname);
    if(!f.is_open()){std::cout<<ERR<<"Cannot open file.\n"<<RESET;return;}
    f<<"# Task Export — "<<todayStr()<<"\n\n";
    auto section=[&](int pid,const std::string& h){
        f<<"## "<<h<<"\n\n"; bool any=false;
        for(auto& t:tasks){
            if(t.projectId!=pid||t.parentId!=0||t.done) continue;
            f<<"- [ ] ["<<t.priority<<"] "<<t.text;
            if(!t.due.empty())     f<<"  due:"<<t.due;
            if(!t.tags.empty())    f<<"  "<<t.tags;
            if(t.estMinutes>0)     f<<"  est:"<<t.estMinutes<<"min";
            if(t.minutes>0)        f<<"  actual:"<<t.minutes<<"min";
            if(t.pomodoros>0)      f<<"  🍅"<<t.pomodoros;
            if(t.streak>0)         f<<"  🔥"<<t.streak;
            if(!t.recur.empty())   f<<"  ~"<<t.recur;
            if(!t.blockedBy.empty())f<<"  blockedBy:"<<t.blockedBy;
            if(!t.notes.empty())   f<<"\n  > "<<t.notes;
            f<<"\n";
            for(auto& s:tasks)
                if(s.parentId==t.id) f<<"  - ["<<(s.done?"x":" ")<<"] "<<s.text<<"\n";
            any=true;
        }
        if(!any) f<<"_No active tasks._\n";
        f<<"\n";
    };
    section(0,"📥 Inbox");
    for(auto& p:projects) section(p.id,"📁 "+p.name);
    std::cout<<SUCCESS<<"Exported to "<<fname<<"\n"<<RESET;
}

// ── Weekly agenda ─────────────────────────────────────────────────────────────

void weeklyAgenda(const std::vector<Task>& tasks,
                  const std::vector<Project>& projects){
    clearScreen(); printHeader("🗓  Weekly Agenda");
    const char* days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    std::cout<<"\n";

    // Overdue block
    {
        std::vector<const Task*> od;
        for(auto& t:tasks) if(!t.done&&dueDaysDiff(t.due)==-1) od.push_back(&t);
        if(!od.empty()){
            std::cout<<RED<<BOLD<<"  ⚠ Overdue\n"<<RESET;
            for(auto* t:od){
                (void)findProject(projects,t->projectId);
                std::cout<<"    "<<std::setw(4)<<t->id<<"  "
                         <<priColour(t->priority)<<priTag(t->priority)<<RESET
                         <<"  "<<(isBlocked(*t,tasks)?RED+"⛔ "+RESET:"")
                         <<t->text<<RED<<"  ["<<t->due<<"]"<<RESET<<"\n";
            }
            std::cout<<"\n";
        }
    }

    for(int d=0;d<7;d++){
        std::string date=addDays(todayStr(),d);
        struct tm td={};
        sscanf(date.c_str(),"%d-%d-%d",&td.tm_year,&td.tm_mon,&td.tm_mday);
        td.tm_year-=1900; td.tm_mon-=1; mktime(&td);
        std::string label=(d==0)
            ? std::string("  Today (")+days[td.tm_wday]+", "+date+")"
            : std::string("  ")+days[td.tm_wday]+", "+date;
        if(d==0) std::cout<<YELLOW<<BOLD<<label<<"\n"<<RESET;
        else      std::cout<<BOLD<<label<<"\n"<<RESET;
        std::vector<const Task*> dts;
        for(auto& t:tasks) if(!t.done&&t.due==date) dts.push_back(&t);
        if(dts.empty()){ std::cout<<DIM<<"    (nothing)\n"<<RESET; }
        else for(auto* t:dts){
            const Project* p=findProject(projects,t->projectId);
            std::string proj=(t->projectId==0)?"Inbox":(p?p->name:"?");
            std::cout<<"    "<<std::setw(4)<<t->id<<"  "
                     <<priColour(t->priority)<<priTag(t->priority)<<RESET
                     <<"  "<<(t->pinned?"📌 ":"")
                     <<(isBlocked(*t,tasks)?RED+"⛔ "+RESET:"")
                     <<t->text<<colorTags(t->tags)
                     <<DIM<<"  ["<<proj<<"]"<<RESET<<"\n";
        }
        std::cout<<"\n";
    }
    pressEnter();
}

// ── Profiles UI ───────────────────────────────────────────────────────────────

// Forward declarations
void saveConfig();
void loadConfig();
void loadAll(std::vector<Project>&,std::vector<Task>&);

void profilesMenu(std::vector<Project>& projects, std::vector<Task>& tasks){
    while(true){
        clearScreen(); printHeader("👤 Profiles");
        auto profiles=listProfiles();
        std::cout<<"\n  Active: "<<BOLD<<CYAN<<gActiveProfile<<RESET<<"\n\n";
        for(size_t i=0;i<profiles.size();i++)
            std::cout<<"  "<<(i+1)<<"  "<<profiles[i]
                     <<(profiles[i]==gActiveProfile?" ← active":"")<<"\n";
        std::cout<<"\n"
                 <<ACCENT<<"  n"<<RESET<<" New profile   "
                 <<ACCENT<<"s"<<RESET<<" Switch   "
                 <<ACCENT<<"X"<<RESET<<" Delete   "
                 <<ACCENT<<"b"<<RESET<<" Back\n"
                 <<BOLD<<"Command: "<<RESET;

        char cmd; std::cin>>cmd; std::cin.ignore(10000,'\n');
        if(cmd=='n'){
            std::cout<<"Profile name: ";
            std::string name; std::getline(std::cin,name);
            name=trim(name);
            if(name.empty()||name.find('/')!=std::string::npos){
                std::cout<<ERR<<"Invalid name.\n"<<RESET; pressEnter(); continue;
            }
            bool exists=false;
            for(auto& p:profiles) if(p==name){exists=true;break;}
            if(exists){std::cout<<WARN<<"Already exists.\n"<<RESET;pressEnter();continue;}
            profiles.push_back(name);
            saveProfileList(profiles);
            mkdirP(profileDir(name));
            std::cout<<SUCCESS<<"Profile '"<<name<<"' created.\n"<<RESET;
            pressEnter();
        } else if(cmd=='s'){
            std::cout<<"Switch to profile name: ";
            std::string name; std::getline(std::cin,name); name=trim(name);
            bool found=false;
            for(auto& p:profiles) if(p==name){found=true;break;}
            if(!found){std::cout<<ERR<<"Not found.\n"<<RESET;pressEnter();continue;}
            // Save current, reload new
            saveAll(projects,tasks);
            saveConfig();
            gActiveProfile=name;
            saveActiveProfile(name);
            initPaths(name);
            loadConfig();
            gTheme=makeTheme(gConfig.theme);
            projects.clear(); tasks.clear();
            loadAll(projects,tasks);
            std::cout<<SUCCESS<<"Switched to '"<<name<<"'.\n"<<RESET;
            pressEnter(); return;
        } else if(cmd=='X'){
            std::cout<<"Delete profile name (cannot delete active): ";
            std::string name; std::getline(std::cin,name); name=trim(name);
            if(name==gActiveProfile){
                std::cout<<ERR<<"Cannot delete active profile.\n"<<RESET;pressEnter();continue;
            }
            profiles.erase(std::remove(profiles.begin(),profiles.end(),name),profiles.end());
            saveProfileList(profiles);
            std::cout<<WARN<<"Profile removed from list. Data files not deleted.\n"<<RESET;
            pressEnter();
        } else if(cmd=='b') return;
    }
}

// ── Options ───────────────────────────────────────────────────────────────────

void optionsScreen(std::vector<Project>& projects, std::vector<Task>& tasks){
    while(true){
        clearScreen(); printHeader("⚙  Options");
        std::cout<<"\n"
                 <<ACCENT<<"  1"<<RESET<<"  Default priority       ["<<gConfig.defaultPriority<<"]\n"
                 <<ACCENT<<"  2"<<RESET<<"  Show done tasks        ["<<(gConfig.showDone?"yes":"no")<<"]\n"
                 <<ACCENT<<"  3"<<RESET<<"  Confirm before delete  ["<<(gConfig.confirmDelete?"yes":"no")<<"]\n"
                 <<ACCENT<<"  4"<<RESET<<"  Theme                  ["<<gConfig.theme<<"]\n"
                 <<ACCENT<<"  5"<<RESET<<"  Login PIN              ["<<(gConfig.loginEnabled?"enabled":"disabled")<<"]\n"
                 <<ACCENT<<"  6"<<RESET<<"  Pomodoro work minutes  ["<<gConfig.pomodoroWork<<"]\n"
                 <<ACCENT<<"  7"<<RESET<<"  Pomodoro break minutes ["<<gConfig.pomodoroBreak<<"]\n"
                 <<ACCENT<<"  8"<<RESET<<"  Pomodoro long break    ["<<gConfig.pomodoroLong<<"]\n"
                 <<ACCENT<<"  P"<<RESET<<"  Profiles\n"
                 <<ACCENT<<"  b"<<RESET<<"  Back\n"
                 <<BOLD<<"\nOption: "<<RESET;

        char cmd; std::cin>>cmd; std::cin.ignore(10000,'\n');
        if(cmd=='1'){
            std::cout<<"Priority (l/m/h): ";
            std::string s; std::getline(std::cin,s);
            if(!s.empty()) gConfig.defaultPriority=parsePriority(s);
        } else if(cmd=='2'){ gConfig.showDone=!gConfig.showDone;
        } else if(cmd=='3'){ gConfig.confirmDelete=!gConfig.confirmDelete;
        } else if(cmd=='4'){
            std::cout<<"Theme (default/solarized/nord/mono): ";
            std::string s; std::getline(std::cin,s);
            if(s=="default"||s=="solarized"||s=="nord"||s=="mono"){
                gConfig.theme=s; gTheme=makeTheme(s);
            } else std::cout<<ERR<<"Unknown theme.\n"<<RESET;
        } else if(cmd=='5'){
            if(gConfig.loginEnabled){
                std::cout<<"Disable login? (y/N): "; char c; std::cin>>c; std::cin.ignore(10000,'\n');
                if(c=='y'||c=='Y') setupPin(true);
            } else setupPin(false);
        } else if(cmd=='6'){
            std::cout<<"Work minutes [25]: "; std::string s; std::getline(std::cin,s);
            try{if(!s.empty())gConfig.pomodoroWork=std::stoi(s);}catch(...){}
        } else if(cmd=='7'){
            std::cout<<"Break minutes [5]: "; std::string s; std::getline(std::cin,s);
            try{if(!s.empty())gConfig.pomodoroBreak=std::stoi(s);}catch(...){}
        } else if(cmd=='8'){
            std::cout<<"Long break minutes [15]: "; std::string s; std::getline(std::cin,s);
            try{if(!s.empty())gConfig.pomodoroLong=std::stoi(s);}catch(...){}
        } else if(cmd=='P'){
            profilesMenu(projects,tasks); continue;
        } else if(cmd=='b'){
            saveConfig(); return;
        }
        saveConfig();
        pressEnter();
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// TASK SCREEN
// ══════════════════════════════════════════════════════════════════════════════

void printTaskList(const std::vector<Task>& tasks, int projectId,
                   const std::string& filter, const std::string& sortMode){
    std::cout<<"\n";
    std::vector<const Task*> shown;
    int total=0,done=0,high=0;
    for(auto& t:tasks){
        if(t.projectId!=projectId||t.parentId!=0) continue;
        total++; if(t.done)done++; if(t.priority=="high"&&!t.done)high++;
        if(filter=="active"&&t.done) continue;
        if(filter=="done"  &&!t.done) continue;
        if(filter=="high"  &&(t.priority!="high"||t.done)) continue;
        if(filter=="today" &&!(dueDaysDiff(t.due)==0||dueDaysDiff(t.due)==-1)) continue;
        if(filter=="blocked"&&(!isBlocked(t,tasks)||t.done)) continue;
        if(!filter.empty()&&filter[0]=='#'&&t.tags.find(filter)==std::string::npos) continue;
        if(!gConfig.showDone&&t.done) continue;
        shown.push_back(&t);
    }

    if(sortMode=="priority"){
        std::sort(shown.begin(),shown.end(),[](const Task* a,const Task* b){
            if(a->pinned!=b->pinned) return a->pinned>b->pinned;
            auto r=[](const std::string& p)->int{
                if(p=="high")return 0;
                if(p=="medium")return 1;
                return 2;
            };
            return r(a->priority)<r(b->priority);
        });
    } else if(sortMode=="due"){
        std::sort(shown.begin(),shown.end(),[](const Task* a,const Task* b){
            if(a->pinned!=b->pinned) return a->pinned>b->pinned;
            std::string da=a->due.empty()?"9999-99-99":a->due;
            std::string db=b->due.empty()?"9999-99-99":b->due;
            return da<db;
        });
    } else {
        std::stable_sort(shown.begin(),shown.end(),[](const Task* a,const Task* b){
            return a->pinned>b->pinned;
        });
    }

    for(auto* t:shown){
        std::string status=t->done?GREEN+"✔"+RESET:" ";
        std::string style =t->done?DIM:"";
        bool blocked=isBlocked(*t,tasks);

        std::cout<<style
                 <<std::setw(4)<<t->id<<"  ["<<status<<"]  "
                 <<(t->pinned?"📌 ":"   ")
                 <<priColour(t->priority)<<priTag(t->priority)<<RESET
                 <<"  "<<(blocked?RED+"⛔ "+RESET:"")
                 <<style<<t->text<<RESET;

        // Streak
        if(t->streak>=3) std::cout<<YELLOW<<"  🔥"<<t->streak<<RESET;

        // Pomodoros
        if(t->pomodoros>0) std::cout<<CYAN<<"  ["<<t->pomodoros<<"🍅]"<<RESET;

        // Time: est vs actual
        if(t->estMinutes>0){
            std::string col=(t->minutes>t->estMinutes)?RED:GREEN;
            std::cout<<DIM<<" est:"<<t->estMinutes<<"m"<<RESET
                     <<col<<minutesToHMS(t->minutes)<<RESET;
        } else {
            std::cout<<minutesToHMS(t->minutes);
        }

        // Recur
        if(!t->recur.empty())
            std::cout<<CYAN<<" ↻"<<t->recur[0]<<RESET;

        std::cout<<dueLabel(t->due,t->done)<<colorTags(t->tags)<<"\n";

        // Notes preview
        if(!t->notes.empty()){
            std::string fl=t->notes;
            size_t nl=fl.find('\n');
            if(nl!=std::string::npos) fl=fl.substr(0,nl)+"…";
            std::cout<<DIM<<"       ↳ "<<fl<<RESET<<"\n";
        }

        // Blocked-by info
        if(blocked){
            std::cout<<DIM<<"       ⛔ blocked by: "<<t->blockedBy<<RESET<<"\n";
        }

        // Subtasks
        for(auto& s:tasks){
            if(s.projectId!=projectId||s.parentId!=t->id) continue;
            if(!gConfig.showDone&&s.done) continue;
            std::cout<<(s.done?DIM:"")
                     <<"       └ "<<std::setw(3)<<s.id<<"  ["
                     <<(s.done?GREEN+"✔"+RESET:"○")<<"]  "
                     <<(s.done?DIM:"")+s.text+RESET
                     <<dueLabel(s.due,s.done)<<"\n";
        }
    }
    if(shown.empty()) std::cout<<DIM<<"  (no tasks)\n"<<RESET;
    std::cout<<DIM<<"\n  Total:"<<total<<"  Active:"<<(total-done)
             <<"  Done:"<<done<<"  High:"<<high<<"\n"<<RESET;
}

void printTaskMenu(const std::string& filter,const std::string& sort){
    std::cout<<BOLD<<"\n── Commands ─────────────────────────────────────────\n"<<RESET
             <<"  "<<ACCENT<<"a"<<RESET<<"  Add task (interactive)\n"
             <<"  "<<ACCENT<<"q"<<RESET<<"  Quick-add  (q \"text !high due:2026-06-10 #tag ~weekly\")\n"
             <<"  "<<ACCENT<<"A"<<RESET<<"  Add subtask\n"
             <<"  "<<ACCENT<<"d"<<RESET<<"  Done/undone toggle\n"
             <<"  "<<ACCENT<<"B"<<RESET<<"  Bulk done\n"
             <<"  "<<ACCENT<<"r"<<RESET<<"  Remove task\n"
             <<"  "<<ACCENT<<"e"<<RESET<<"  Edit text\n"
             <<"  "<<ACCENT<<"N"<<RESET<<"  Edit notes\n"
             <<"  "<<ACCENT<<"p"<<RESET<<"  Priority\n"
             <<"  "<<ACCENT<<"D"<<RESET<<"  Due date\n"
             <<"  "<<ACCENT<<"T"<<RESET<<"  Tags\n"
             <<"  "<<ACCENT<<"R"<<RESET<<"  Recurrence\n"
             <<"  "<<ACCENT<<"P"<<RESET<<"  Pin/unpin\n"
             <<"  "<<ACCENT<<"K"<<RESET<<"  Dependencies (blocked-by)\n"
             <<"  "<<ACCENT<<"E"<<RESET<<"  Time estimate\n"
             <<"  "<<ACCENT<<"t"<<RESET<<"  Timer\n"
             <<"  "<<ACCENT<<"F"<<RESET<<"  🍅 Pomodoro\n"
             <<"  "<<ACCENT<<"m"<<RESET<<"  Move to project\n"
             <<"  "<<ACCENT<<"c"<<RESET<<"  Archive done tasks\n"
             <<"  "<<ACCENT<<"V"<<RESET<<"  Browse archive\n"
             <<"  "<<ACCENT<<"u"<<RESET<<"  Undo last delete\n"
             <<"  "<<ACCENT<<"f"<<RESET<<"  Filter ["<<filter<<"]\n"
             <<"  "<<ACCENT<<"o"<<RESET<<"  Sort   ["<<sort<<"]\n"
             <<"  "<<ACCENT<<"b"<<RESET<<"  Back\n"
             <<"─────────────────────────────────────────────────────\n"
             <<BOLD<<"Command: "<<RESET;
}

// ── Task action implementations ───────────────────────────────────────────────

void addTask(std::vector<Task>& tasks,std::vector<Project>& projects,
             int projectId,int parentId=0){
    if(parentId>0){
        Task* par=findTask(tasks,parentId);
        if(!par){std::cout<<ERR<<"Parent not found.\n"<<RESET;return;}
        std::cout<<"Subtask for ["<<parentId<<"] \""<<par->text<<"\"\n";
    }
    std::string text,pri,due,tags,recurStr,estStr;
    std::cout<<(parentId>0?"Subtask: ":"Description: ");
    std::cin.ignore(10000,'\n'); std::getline(std::cin,text);
    if(text.empty()){std::cout<<ERR<<"Cancelled.\n"<<RESET;return;}
    std::cout<<"Priority (l/m/h) ["<<gConfig.defaultPriority[0]<<"]: ";
    std::getline(std::cin,pri);
    std::cout<<"Due date (YYYY-MM-DD or blank): ";
    std::getline(std::cin,due);
    if(!due.empty()&&!isValidDate(due)){
        std::cout<<WARN<<"Invalid date, skipped.\n"<<RESET;due="";
    }
    std::cout<<"Tags: "; std::getline(std::cin,tags);
    std::cout<<"Estimate (minutes, or blank): "; std::getline(std::cin,estStr);
    int est=0; try{if(!estStr.empty())est=std::stoi(estStr);}catch(...){}
    if(parentId==0){
        std::cout<<"Recurrence (d/w/m or blank): "; std::getline(std::cin,recurStr);
    }
    Task t;
    t.id=nextTaskId(tasks); t.projectId=projectId; t.parentId=parentId;
    t.text=text; t.priority=parsePriority(pri);
    t.due=due; t.tags=tags; t.estMinutes=est; t.recur=parseRecur(recurStr);
    tasks.push_back(t);
    saveAll(projects,tasks);
    std::cout<<SUCCESS<<"Added.\n"<<RESET;
}

void quickAdd(std::vector<Task>& tasks,std::vector<Project>& projects,
              int projectId){
    std::cout<<"Quick-add: ";
    std::cin.ignore(10000,'\n');
    std::string input; std::getline(std::cin,input);
    if(input.empty()){std::cout<<ERR<<"Cancelled.\n"<<RESET;return;}
    QuickTask q=parseQuick(input);
    if(q.text.empty()){std::cout<<ERR<<"No task text found.\n"<<RESET;return;}
    Task t;
    t.id=nextTaskId(tasks); t.projectId=projectId;
    t.text=q.text; t.priority=q.priority;
    t.due=q.due; t.tags=q.tags; t.recur=q.recur; t.estMinutes=q.estMinutes;
    tasks.push_back(t);
    saveAll(projects,tasks);
    std::cout<<SUCCESS<<"Added: \""<<t.text<<"\"  ["<<t.priority<<"]"
             <<(t.due.empty()?"":" due:"+t.due)
             <<(t.tags.empty()?"":" "+t.tags)<<"\n"<<RESET;
}

void toggleDone(std::vector<Task>& tasks,std::vector<Project>& projects){
    int id; std::cout<<"Task ID: "; std::cin>>id;
    Task* t=findTask(tasks,id);
    if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
    if(t->timerRunning){
        t->minutes+=(int)difftime(time(nullptr),t->timerStart)/60;
        t->timerRunning=false;
    }
    t->done=!t->done;
    if(t->done){
        updateStreak(*t);
        if(!t->recur.empty()){
            std::vector<Project> tmpProj;
            spawnNextRecurrence(tasks,tmpProj,*t);
        }
    }
    saveAll(projects,tasks);
    std::cout<<SUCCESS<<(t->done?"Done.\n":"Active.\n")<<RESET;
}

void bulkDone(std::vector<Task>& tasks,std::vector<Project>& projects){
    std::cout<<"IDs (space-separated): ";
    std::cin.ignore(10000,'\n');
    std::string line; std::getline(std::cin,line);
    std::istringstream ss(line); int id,count=0;
    while(ss>>id){
        Task* t=findTask(tasks,id);
        if(t&&!t->done){
            t->done=true; updateStreak(*t); count++;
            if(!t->recur.empty()){
                std::vector<Project> tmp;
                spawnNextRecurrence(tasks,tmp,*t);
            }
        }
    }
    saveAll(projects,tasks);
    std::cout<<SUCCESS<<count<<" done.\n"<<RESET;
}

void removeTask(std::vector<Task>& tasks,std::vector<Project>& projects){
    int id; std::cout<<"Task ID: "; std::cin>>id;
    if(gConfig.confirmDelete){
        Task* t=findTask(tasks,id);
        if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
        std::cout<<"Delete \""<<t->text<<"\"? (y/N): "; char c; std::cin>>c;
        if(c!='y'&&c!='Y'){std::cout<<DIM<<"Cancelled.\n"<<RESET;return;}
    }
    for(auto it=tasks.begin();it!=tasks.end();++it){
        if(it->id==id){
            gUndoTask=*it; tasks.erase(it);
            tasks.erase(std::remove_if(tasks.begin(),tasks.end(),
                [id](const Task& t){return t.parentId==id;}),tasks.end());
            saveAll(projects,tasks);
            std::cout<<SUCCESS<<"Removed. (u to undo)\n"<<RESET; return;
        }
    }
    std::cout<<ERR<<"Not found.\n"<<RESET;
}

void undoDelete(std::vector<Task>& tasks,std::vector<Project>& projects){
    if(!gUndoTask){std::cout<<WARN<<"Nothing to undo.\n"<<RESET;return;}
    gUndoTask->id=nextTaskId(tasks);
    tasks.push_back(*gUndoTask); gUndoTask.reset();
    saveAll(projects,tasks);
    std::cout<<SUCCESS<<"Restored.\n"<<RESET;
}

void editTask(std::vector<Task>& tasks,std::vector<Project>& projects){
    int id; std::cout<<"Task ID: "; std::cin>>id;
    std::cin.ignore(10000,'\n');
    Task* t=findTask(tasks,id);
    if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
    std::cout<<"New text ["<<t->text<<"]: ";
    std::string s; std::getline(std::cin,s);
    if(!s.empty()) t->text=s;
    saveAll(projects,tasks);
    std::cout<<SUCCESS<<"Updated.\n"<<RESET;
}

void editNotes(std::vector<Task>& tasks,std::vector<Project>& projects){
    int id; std::cout<<"Task ID: "; std::cin>>id;
    std::cin.ignore(10000,'\n');
    Task* t=findTask(tasks,id);
    if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
    if(!t->notes.empty()) std::cout<<DIM<<"Current:\n"<<t->notes<<"\n"<<RESET;
    std::cout<<"New notes (blank line to finish):\n";
    std::string all,line;
    while(std::getline(std::cin,line)&&!line.empty()){
        if(!all.empty()) all+="\n";
        all+=line;
    }
    t->notes=all;
    saveAll(projects,tasks); std::cout<<SUCCESS<<"Saved.\n"<<RESET;
}

void setEstimate(std::vector<Task>& tasks,std::vector<Project>& projects){
    int id; std::cout<<"Task ID: "; std::cin>>id;
    std::cin.ignore(10000,'\n');
    Task* t=findTask(tasks,id);
    if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
    std::cout<<"Estimated minutes ["<<t->estMinutes<<"]: ";
    std::string s; std::getline(std::cin,s);
    try{if(!s.empty())t->estMinutes=std::stoi(s);}catch(...){}
    saveAll(projects,tasks); std::cout<<SUCCESS<<"Set.\n"<<RESET;
}

void changePriority(std::vector<Task>& tasks,std::vector<Project>& projects){
    int id; std::cout<<"Task ID: "; std::cin>>id;
    std::cin.ignore(10000,'\n');
    Task* t=findTask(tasks,id);
    if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
    std::cout<<"Priority (l/m/h): "; std::string s; std::getline(std::cin,s);
    t->priority=parsePriority(s);
    saveAll(projects,tasks); std::cout<<SUCCESS<<"Updated.\n"<<RESET;
}

void setDueDate(std::vector<Task>& tasks,std::vector<Project>& projects){
    int id; std::cout<<"Task ID: "; std::cin>>id;
    std::cin.ignore(10000,'\n');
    Task* t=findTask(tasks,id);
    if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
    std::cout<<"Due (YYYY-MM-DD, blank=clear) ["<<t->due<<"]: ";
    std::string s; std::getline(std::cin,s);
    if(s.empty()){t->due="";std::cout<<SUCCESS<<"Cleared.\n"<<RESET;}
    else if(isValidDate(s)){t->due=s;std::cout<<SUCCESS<<"Set.\n"<<RESET;}
    else std::cout<<ERR<<"Invalid format.\n"<<RESET;
    saveAll(projects,tasks);
}

void editTags(std::vector<Task>& tasks,std::vector<Project>& projects){
    int id; std::cout<<"Task ID: "; std::cin>>id;
    std::cin.ignore(10000,'\n');
    Task* t=findTask(tasks,id);
    if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
    std::cout<<"Tags ["<<t->tags<<"]: "; std::string s; std::getline(std::cin,s);
    t->tags=s; saveAll(projects,tasks); std::cout<<SUCCESS<<"Updated.\n"<<RESET;
}

void setRecurrence(std::vector<Task>& tasks,std::vector<Project>& projects){
    int id; std::cout<<"Task ID: "; std::cin>>id;
    std::cin.ignore(10000,'\n');
    Task* t=findTask(tasks,id);
    if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
    if(t->due.empty()) std::cout<<WARN<<"Note: no due date set — recurrence needs it.\n"<<RESET;
    std::cout<<"Recurrence (d/w/m, blank=none) ["<<t->recur<<"]: ";
    std::string s; std::getline(std::cin,s);
    t->recur=parseRecur(s);
    saveAll(projects,tasks); std::cout<<SUCCESS<<"Set.\n"<<RESET;
}

void togglePin(std::vector<Task>& tasks,std::vector<Project>& projects){
    int id; std::cout<<"Task ID: "; std::cin>>id;
    Task* t=findTask(tasks,id);
    if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
    t->pinned=!t->pinned;
    saveAll(projects,tasks);
    std::cout<<SUCCESS<<(t->pinned?"📌 Pinned.\n":"Unpinned.\n")<<RESET;
}

void timerToggle(std::vector<Task>& tasks,std::vector<Project>& projects){
    int id; std::cout<<"Task ID: "; std::cin>>id;
    Task* t=findTask(tasks,id);
    if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
    if(t->timerRunning){
        int e=(int)difftime(time(nullptr),t->timerStart)/60;
        t->minutes+=e; t->timerRunning=false;
        saveAll(projects,tasks);
        std::cout<<SUCCESS<<"Stopped. +"<<e<<"m (total:"<<t->minutes<<"m)\n"<<RESET;
    } else {
        t->timerRunning=true; t->timerStart=time(nullptr);
        std::cout<<SUCCESS<<"Timer started.\n"<<RESET;
    }
}

void moveTask(std::vector<Task>& tasks,const std::vector<Project>& projects){
    int id; std::cout<<"Task ID: "; std::cin>>id;
    Task* t=findTask(tasks,id);
    if(!t){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
    std::cout<<"\n  0  📥 Inbox\n";
    for(auto& p:projects) std::cout<<"  "<<p.id<<"  📁 "<<p.name<<"\n";
    int dest; std::cout<<"Move to: "; std::cin>>dest;
    if(dest!=0&&!findProject(projects,dest)){
        std::cout<<ERR<<"Not found.\n"<<RESET;return;
    }
    t->projectId=dest; std::cout<<SUCCESS<<"Moved.\n"<<RESET;
}

// ── Task screen loop ──────────────────────────────────────────────────────────

void taskScreen(std::vector<Project>& projects,std::vector<Task>& tasks,int projectId){
    std::string filter="all",sortMode="id";
    const Project* proj=findProject(projects,projectId);
    std::string title=(projectId==0)?"📥 Inbox":"📁 "+(proj?proj->name:"?");

    while(true){
        clearScreen(); printHeader(title);
        std::cout<<DIM<<"  Filter:"<<filter<<"  Sort:"<<sortMode<<"\n"<<RESET;
        printTaskList(tasks,projectId,filter,sortMode);
        printTaskMenu(filter,sortMode);

        char cmd; std::cin>>cmd;
        switch(cmd){
            case 'a': addTask(tasks,projects,projectId);                          break;
            case 'q': quickAdd(tasks,projects,projectId);                         break;
            case 'A': {
                int pid; std::cout<<"Parent task ID: "; std::cin>>pid;
                addTask(tasks,projects,projectId,pid);
            }                                                                      break;
            case 'd': toggleDone(tasks,projects);                                 break;
            case 'B': bulkDone(tasks,projects);                                   break;
            case 'r': removeTask(tasks,projects);                                 break;
            case 'u': undoDelete(tasks,projects);                                 break;
            case 'e': editTask(tasks,projects);                                   break;
            case 'N': editNotes(tasks,projects);                                  break;
            case 'p': changePriority(tasks,projects);                             break;
            case 'D': setDueDate(tasks,projects);                                 break;
            case 'T': editTags(tasks,projects);                                   break;
            case 'R': setRecurrence(tasks,projects);                              break;
            case 'P': togglePin(tasks,projects);                                  break;
            case 'K': setDependencies(tasks,projects);                            break;
            case 'E': setEstimate(tasks,projects);                                break;
            case 't': timerToggle(tasks,projects);                                break;
            case 'F': pomodoroSession(tasks,projects);                            break;
            case 'm': moveTask(tasks,projects); saveAll(projects,tasks);          break;
            case 'c': archiveDone(tasks,projects,projectId);                      break;
            case 'V': browseArchive();                                            break;
            case 'f': {
                std::cout<<"Filter (all/active/done/high/today/blocked/#tag): ";
                std::cin.ignore(10000,'\n'); std::getline(std::cin,filter);
            }                                                                      break;
            case 'o': {
                std::cout<<"Sort (id/priority/due): ";
                std::cin.ignore(10000,'\n'); std::getline(std::cin,sortMode);
            }                                                                      break;
            case 'b': return;
            default:  std::cout<<ERR<<"Unknown command.\n"<<RESET;               break;
        }
        pressEnter();
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// PROJECT SCREEN
// ══════════════════════════════════════════════════════════════════════════════

void printProjectList(const std::vector<Project>& projects,const std::vector<Task>& tasks){
    std::cout<<"\n";
    auto row=[&](int pid,const std::string& label,const std::string& icon){
        int total=0,done=0,overdue=0;
        for(auto& t:tasks){
            if(t.projectId!=pid) continue;
            total++; if(t.done)done++;
            if(!t.done&&dueDaysDiff(t.due)==-1)overdue++;
        }
        int pct=(total>0)?(done*100/total):0;
        int filled=pct/10;
        std::string bar="[";
        for(int i=0;i<10;i++) bar+=(i<filled?"█":"░");
        bar+="]";
        std::cout<<"  "<<std::setw(3)<<pid<<"  "<<BOLD<<icon<<" "<<label<<RESET
                 <<DIM<<"  "<<bar<<" "<<pct<<"%"
                 <<"  ("<<(total-done)<<" active / "<<total<<" total)";
        if(overdue>0) std::cout<<"  "<<RED<<"⚠"<<overdue<<RESET;
        std::cout<<"\n";
    };
    row(0,"Inbox","📥");
    for(auto& p:projects) row(p.id,p.name,"📁");
    if(projects.empty()) std::cout<<DIM<<"\n  No projects. Press 'n'.\n"<<RESET;
}

void newProject(std::vector<Project>& projects,std::vector<Task>& tasks){
    std::cout<<"Project name: "; std::cin.ignore(10000,'\n');
    std::string name; std::getline(std::cin,name);
    if(name.empty()){std::cout<<ERR<<"Cancelled.\n"<<RESET;return;}
    projects.push_back({nextProjId(projects),name});
    saveAll(projects,tasks); std::cout<<SUCCESS<<"Created.\n"<<RESET;
}

void renameProject(std::vector<Project>& projects,std::vector<Task>& tasks){
    int id; std::cout<<"Project ID: "; std::cin>>id;
    std::cin.ignore(10000,'\n');
    for(auto& p:projects) if(p.id==id){
        std::cout<<"New name ["<<p.name<<"]: ";
        std::string s; std::getline(std::cin,s);
        if(!s.empty()) p.name=s;
        saveAll(projects,tasks); std::cout<<SUCCESS<<"Renamed.\n"<<RESET; return;
    }
    std::cout<<ERR<<"Not found.\n"<<RESET;
}

void deleteProject(std::vector<Project>& projects,std::vector<Task>& tasks){
    int id; std::cout<<"Project ID: "; std::cin>>id;
    if(id==0){std::cout<<ERR<<"Cannot delete Inbox.\n"<<RESET;return;}
    auto it=std::remove_if(projects.begin(),projects.end(),[id](auto& p){return p.id==id;});
    if(it==projects.end()){std::cout<<ERR<<"Not found.\n"<<RESET;return;}
    int moved=0;
    for(auto& t:tasks) if(t.projectId==id){t.projectId=0;moved++;}
    projects.erase(it,projects.end());
    saveAll(projects,tasks);
    std::cout<<SUCCESS<<"Deleted. "<<moved<<" task(s) → Inbox.\n"<<RESET;
}

void projectScreen(std::vector<Project>& projects,std::vector<Task>& tasks){
    while(true){
        clearScreen(); printHeader("Projects");
        printProjectList(projects,tasks);
        std::cout<<BOLD<<"\n── Commands ─────────────────────────────────────────\n"<<RESET
                 <<"  "<<ACCENT<<"<id>"<<RESET<<"  Open project\n"
                 <<"  "<<ACCENT<<"n"   <<RESET<<"    New project\n"
                 <<"  "<<ACCENT<<"R"   <<RESET<<"    Rename project\n"
                 <<"  "<<ACCENT<<"X"   <<RESET<<"    Delete project\n"
                 <<"  "<<ACCENT<<"/"   <<RESET<<"    Search all tasks\n"
                 <<"  "<<ACCENT<<"W"   <<RESET<<"    Weekly agenda\n"
                 <<"  "<<ACCENT<<"G"   <<RESET<<"    Weekly review\n"
                 <<"  "<<ACCENT<<"E"   <<RESET<<"    Export to markdown\n"
                 <<"  "<<ACCENT<<"S"   <<RESET<<"    Statistics\n"
                 <<"  "<<ACCENT<<"O"   <<RESET<<"    Options / settings\n"
                 <<"  "<<ACCENT<<"q"   <<RESET<<"    Quit\n"
                 <<"─────────────────────────────────────────────────────\n"
                 <<BOLD<<"Command: "<<RESET;

        std::string input; std::cin>>input;
        bool isNum=!input.empty()&&
                   std::all_of(input.begin(),input.end(),::isdigit);
        if(isNum){
            int id=std::stoi(input);
            if(id==0){taskScreen(projects,tasks,0);continue;}
            if(findProject(projects,id)) taskScreen(projects,tasks,id);
            else{std::cout<<ERR<<"Not found.\n"<<RESET;pressEnter();}
            continue;
        }

        char cmd=input[0];
        switch(cmd){
            case 'n': newProject(projects,tasks);             break;
            case 'R': renameProject(projects,tasks);          break;
            case 'X': deleteProject(projects,tasks);          break;
            case '/': searchTasks(tasks,projects);            break;
            case 'W': weeklyAgenda(tasks,projects);           break;
            case 'G': weeklyReview(tasks,projects);           break;
            case 'E':
                std::cin.ignore(10000,'\n');
                exportTasks(tasks,projects);
                pressEnter(); continue;
            case 'S': showStats(tasks,projects);              break;
            case 'O': optionsScreen(projects,tasks);          break;
            case 'q':
                std::cout<<ACCENT<<"\nBye! 🚀\n"<<RESET;
                return;
            default:
                std::cout<<ERR<<"Unknown command.\n"<<RESET;
        }
        pressEnter();
    }
}

// ── CLI mode ──────────────────────────────────────────────────────────────────
// ./todo add "task text !high due:2026-06-10"
// ./todo done 5
// ./todo list
// ./todo list inbox
// ./todo list <projectId>

void cliMode(int argc, char* argv[],
             std::vector<Project>& projects, std::vector<Task>& tasks){
    std::string cmd=argv[1];
    if(cmd=="add"&&argc>=3){
        std::string input;
        for(int i=2;i<argc;i++){if(i>2)input+=" ";input+=argv[i];}
        QuickTask q=parseQuick(input);
        if(q.text.empty()){std::cerr<<"Error: no task text.\n";return;}
        Task t;
        t.id=nextTaskId(tasks); t.projectId=0;
        t.text=q.text; t.priority=q.priority;
        t.due=q.due; t.tags=q.tags; t.recur=q.recur; t.estMinutes=q.estMinutes;
        tasks.push_back(t);
        saveAll(projects,tasks);
        std::cout<<"Added #"<<t.id<<": "<<t.text<<"\n";

    } else if(cmd=="done"&&argc>=3){
        try{
            int id=std::stoi(argv[2]);
            Task* t=findTask(tasks,id);
            if(!t){std::cerr<<"Task "<<id<<" not found.\n";return;}
            t->done=true; updateStreak(*t);
            if(!t->recur.empty()){
                std::vector<Project> tmp;
                spawnNextRecurrence(tasks,tmp,*t);
            }
            saveAll(projects,tasks);
            std::cout<<"Done: "<<t->text<<"\n";
        }catch(...){std::cerr<<"Invalid ID.\n";}

    } else if(cmd=="list"){
        int filterProj=-1; // -1 = all
        if(argc>=3){
            if(std::string(argv[2])=="inbox") filterProj=0;
            else try{filterProj=std::stoi(argv[2]);}catch(...){}
        }
        for(auto& t:tasks){
            if(t.done||t.parentId!=0) continue;
            if(filterProj!=-1&&t.projectId!=filterProj) continue;
            const Project* p=findProject(projects,t.projectId);
            std::string proj=(t.projectId==0)?"Inbox":(p?p->name:"?");
            std::cout<<std::setw(4)<<t.id<<"  "
                     <<priTag(t.priority)<<"  "<<t.text
                     <<(t.due.empty()?"":" ["+t.due+"]")
                     <<"  ("<<proj<<")\n";
        }
    } else {
        std::cerr<<"Usage:\n"
                 <<"  todo add \"text !high due:YYYY-MM-DD #tag ~weekly\"\n"
                 <<"  todo done <id>\n"
                 <<"  todo list [inbox|<projectId>]\n";
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]){
    // Load active profile
    mkdirP(profileBaseDir());
    gActiveProfile=loadActiveProfile();
    initPaths(gActiveProfile);
    loadConfig();
    gTheme=makeTheme(gConfig.theme);

    std::vector<Project> projects;
    std::vector<Task>    tasks;
    loadAll(projects,tasks);

    // CLI mode
    if(argc>=2){
        cliMode(argc,argv,projects,tasks);
        return 0;
    }

    // TUI mode
    if(gConfig.loginEnabled && !loginScreen()) return 1;

    int overdue=0;
    for(auto& t:tasks) if(!t.done&&dueDaysDiff(t.due)==-1) overdue++;

    clearScreen(); printHeader();
    if(overdue>0)
        std::cout<<BG_RED<<BOLD<<"  ⚠  "<<overdue<<" overdue task(s)!\n"<<RESET;
    std::cout<<DIM<<"  Data: "<<gConfig.dataPath<<"\n"<<RESET;

    pressEnter();
    projectScreen(projects,tasks);
    return 0;
}