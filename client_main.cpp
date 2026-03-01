/**
 * client_main.cpp
 * ──────────────────────────────────────────────────────────────────────────────
 * Student Record Management System – C++ CLI Client
 *
 * Threading model:
 *   Main thread    — menu loop; for list/search/sort it WAITS for the response
 *                    before redrawing the menu, so results never appear under
 *                    the prompt.
 *   WS read thread — receives frames, renders them, signals the main thread.
 *
 * Synchronisation:
 *   expectResponse() sets responseReady_ = false and records what action to
 *   wait for. signalResponse() sets responseReady_ = true and notifies.
 *   waitForResponse() blocks until responseReady_ is true OR 5-s timeout.
 *
 *   Using a bool flag (not "pendingAction_ is empty") avoids the race where
 *   the server replies before waitForResponse() is even called — in that case
 *   responseReady_ is already true and waitForResponse() returns immediately
 *   with the table already printed above.
 */

#include "Logger.h"
#include "StudentDB.h"
#include "WebSocketClient.h"
#include "JsonUtil.h"
#include "CSVParser.h"

#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <unistd.h>

// ── Console colours ────────────────────────────────────────────────────────────
#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_CYAN   "\033[36m"
#define C_GREEN  "\033[32m"
#define C_YELLOW "\033[33m"
#define C_RED    "\033[31m"
#define C_BLUE   "\033[34m"

// ── Response synchronisation ──────────────────────────────────────────────────
static std::mutex              responseMtx_;
static std::condition_variable responseCv_;
static bool                    responseReady_  = true;   // true = no pending wait
static std::string             awaitedAction_;           // action we're waiting for

// Call BEFORE sending the request. Sets the flag to "not ready yet".
void expectResponse(const std::string& action) {
    std::lock_guard<std::mutex> lk(responseMtx_);
    awaitedAction_  = action;
    responseReady_  = false;
}

// Called by WS thread once the matching response is displayed.
// Also called on error or disconnect to unblock a stuck wait.
void signalResponse() {
    {
        std::lock_guard<std::mutex> lk(responseMtx_);
        responseReady_ = true;
        awaitedAction_.clear();
    }
    responseCv_.notify_all();
}

// Blocks until signalResponse() fires OR 5-second timeout.
// Returns immediately if no expectResponse() is pending.
void waitForResponse() {
    std::unique_lock<std::mutex> lk(responseMtx_);
    responseCv_.wait_for(lk, std::chrono::seconds(5),
                         [] { return responseReady_; });
}

// ── Output ─────────────────────────────────────────────────────────────────────
// One mutex for all console output. The WS thread acquires it when printing;
// the main thread acquires it when printing the menu.
static std::mutex coutMtx_;

// Used outside of any lock (startup messages, menu). Takes the lock itself.
void safePrint(const std::string& s) {
    std::lock_guard<std::mutex> lk(coutMtx_);
    std::cout << s << std::flush;
}

// ── Pretty-print a student table ───────────────────────────────────────────────
// Must be called while coutMtx_ is already held by the caller.
// Never calls safePrint (that would re-acquire the non-reentrant mutex).
void printTableLocked(const std::vector<Student>& students) {
    std::cout << "\n"
              << C_CYAN << std::left
              << std::setw(6)  << "ID"
              << std::setw(26) << "Name"
              << std::setw(6)  << "Age"
              << std::setw(8)  << "Grade"
              << C_RESET << "\n"
              << std::string(46, '-') << "\n";
    for (const auto& s : students)
        std::cout << std::left
                  << std::setw(6)  << s.id
                  << std::setw(26) << s.name
                  << std::setw(6)  << s.age
                  << std::setw(8)  << s.grade << "\n";
    std::cout << std::string(46, '-') << "\n"
              << C_YELLOW << students.size() << " record(s)"
              << C_RESET << "\n" << std::flush;
}

// ── Parse JSON array "[{...},{...}]" → vector<Student> ─────────────────────────
std::vector<Student> parseStudentArray(const std::string& arr) {
    std::vector<Student> result;
    int    depth  = 0;
    size_t start  = 0;
    bool   inStr  = false;
    bool   escape = false;

    for (size_t i = 0; i < arr.size(); ++i) {
        char c = arr[i];
        if (escape)      { escape = false; continue; }
        if (c == '\\')   { escape = true;  continue; }
        if (c == '"')    { inStr  = !inStr; continue; }
        if (inStr) continue;

        if (c == '{') {
            if (!depth) start = i;
            ++depth;
        } else if (c == '}') {
            if (!--depth) {
                Student s = json::parseStudent(arr.substr(start, i - start + 1));
                if (s.id > 0) result.push_back(s);
            }
        }
    }
    return result;
}

// ── Handle all incoming server messages ───────────────────────────────────────
// Runs in the WS background thread.
// Acquires coutMtx_ once for the entire render, then signals the main thread.
void onServerMessage(const std::string& msg) {
    std::string action = json::extractStr(msg, "action");

    std::lock_guard<std::mutex> lk(coutMtx_);

    if (action == "list" || action == "sort_result") {
        auto students = parseStudentArray(json::extractRaw(msg, "students"));
        std::cout << C_BOLD << "\n  All Students:" << C_RESET;
        printTableLocked(students);
        signalResponse();
        return;
    }

    if (action == "search_result") {
        auto students = parseStudentArray(json::extractRaw(msg, "students"));
        std::cout << C_BOLD << "\n  Search Results:" << C_RESET;
        if (students.empty())
            std::cout << C_YELLOW << "\n  No matching students found.\n" << C_RESET;
        else
            printTableLocked(students);
        signalResponse();
        return;
    }

    if (action == "created") {
        Student s = json::parseStudent(json::extractRaw(msg, "student"));
        std::cout << C_GREEN
                  << "\n  ✓ Created: [" << s.id << "] " << s.name
                  << "  age=" << s.age << "  grade=" << s.grade
                  << C_RESET << "\n" << std::flush;
        return;
    }

    if (action == "updated") {
        Student s = json::parseStudent(json::extractRaw(msg, "student"));
        std::cout << C_BLUE
                  << "\n  ✓ Updated: [" << s.id << "] " << s.name
                  << "  age=" << s.age << "  grade=" << s.grade
                  << C_RESET << "\n" << std::flush;
        return;
    }

    if (action == "deleted") {
        std::cout << C_RED
                  << "\n  ✓ Deleted: id=" << json::extractInt(msg, "id")
                  << C_RESET << "\n" << std::flush;
        return;
    }

    if (action == "error") {
        std::cout << C_RED
                  << "\n  ✗ Error: " << json::extractStr(msg, "message")
                  << C_RESET << "\n" << std::flush;
        signalResponse();   // unblock main thread if it was waiting
        return;
    }

    if (action == "ack") {
        // Silently ignore "Server initialised" acks — they clutter startup
        return;
    }

    // Unknown
    std::cout << C_YELLOW << "\n  [Server] "
              << msg.substr(0, 200) << C_RESET << "\n" << std::flush;
}

// ── Input helpers ──────────────────────────────────────────────────────────────
std::string prompt(const std::string& label) {
    std::string val;
    while (val.empty()) {
        std::cout << "  " << label << ": ";
        std::getline(std::cin, val);
        val.erase(0, val.find_first_not_of(" \t"));
        val.erase(val.find_last_not_of(" \t") + 1);
    }
    return val;
}

std::string promptOpt(const std::string& label) {
    std::cout << "  " << label << " (blank=skip): ";
    std::string val;
    std::getline(std::cin, val);
    val.erase(0, val.find_first_not_of(" \t"));
    if (!val.empty()) val.erase(val.find_last_not_of(" \t") + 1);
    return val;
}

void printMenu() {
    std::lock_guard<std::mutex> lk(coutMtx_);
    std::cout << "\n" << C_BOLD C_CYAN
              << "┌─────────────────────────────────────┐\n"
              << "│  Student Record CLI Client           │\n"
              << "├─────────────────────────────────────┤\n"
              << "│  1) List all students                │\n"
              << "│  2) Search students                  │\n"
              << "│  3) Sort students                    │\n"
              << "│  4) Create student                   │\n"
              << "│  5) Update student                   │\n"
              << "│  6) Delete student                   │\n"
              << "│  7) Show performance metrics         │\n"
              << "│  0) Quit                             │\n"
              << "└─────────────────────────────────────┘\n"
              << C_RESET << "  Choice: " << std::flush;
}

// ── main ───────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::string logFile = "client_" + std::to_string(::getpid()) + ".log";
    Logger::instance().setConsoleLevel(LogLevel::WARNING);
    Logger::instance().setFileLevel(LogLevel::INFO);
    Logger::instance().setLogFile(logFile);

    std::string host    = "127.0.0.1";
    uint16_t    port    = 9001;
    std::string inputCsv  = "data/students_input.csv";
    std::string outputCsv = "data/students_output.csv";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i+1 < argc) host    = argv[++i];
        if (a == "--port" && i+1 < argc) port    = (uint16_t)std::stoi(argv[++i]);
        if (a == "--input"  && i+1 < argc) inputCsv  = argv[++i];
        if (a == "--output" && i+1 < argc) outputCsv = argv[++i];
    }

    safePrint(std::string(C_BOLD C_CYAN)
              + "\n  Student Record Management System – CLI Client\n"
              + C_RESET
              + C_YELLOW "  PID " + std::to_string(::getpid())
              + "  |  Log: " + logFile + C_RESET "\n");

    // Load local CSV for performance metrics display only
    StudentDB localDB(inputCsv, outputCsv);
    StudentDB::Stats stats;
    try {
        stats = localDB.loadFromFile();
        safePrint(std::string(C_GREEN) + "  Loaded " +
                  std::to_string(stats.totalRecords) +
                  " records from " + inputCsv + C_RESET + "\n");
    } catch (...) {
        safePrint(std::string(C_YELLOW)
                  + "  No local CSV found – will work from server data\n"
                  + C_RESET);
    }

    // ── WebSocket client ───────────────────────────────────────────────────────
    WebSocketClient wsClient;

    wsClient.onConnect([&]() {
        // Server automatically sends the full student list on every connect.
        // We just show a connected message — the list will arrive momentarily
        // from the server and render itself via onServerMessage.
        safePrint(std::string(C_GREEN)
                  + "\n  ✓ Connected to ws://" + host + ":"
                  + std::to_string(port) + C_RESET + "\n"
                  + C_YELLOW "  Loading student list from server...\n" C_RESET);
    });

    wsClient.onMessage([](const std::string& msg) {
        onServerMessage(msg);
    });

    wsClient.onDisconnect([]() {
        safePrint(C_RED "\n  Server disconnected.\n" C_RESET);
        signalResponse();   // unblock any pending wait
    });

    safePrint("  Connecting to ws://" + host + ":" + std::to_string(port) + "...\n");
    if (!wsClient.connect(host, port)) {
        std::cerr << C_RED "  Failed to connect. Is the server running?\n" C_RESET;
        return 1;
    }

    // Wait for the initial list from the server to display before showing menu.
    // The server sends it automatically in connHandler on every new connection.
    expectResponse("list");
    waitForResponse();      // blocks until onServerMessage("list") fires

    // ── Interactive menu loop ──────────────────────────────────────────────────
    while (wsClient.isConnected()) {
        printMenu();
        std::string choice;
        std::getline(std::cin, choice);

        if (choice == "1") {
            expectResponse("list");
            wsClient.send("{\"action\":\"list\"}");
            waitForResponse();
        }
        else if (choice == "2") {
            std::string q = prompt("Search query");
            expectResponse("search_result");
            wsClient.send("{\"action\":\"search\",\"query\":\""
                          + Student::jsonEscape(q) + "\"}");
            waitForResponse();
        }
        else if (choice == "3") {
            std::cout << "  Sort field [id/name/age/grade]: ";
            std::string f; std::getline(std::cin, f);
            if (f.empty()) f = "id";
            std::cout << "  Direction  [asc/desc]:          ";
            std::string d; std::getline(std::cin, d);
            if (d.empty()) d = "asc";
            expectResponse("list");
            wsClient.send("{\"action\":\"sort\",\"field\":\"" + f
                          + "\",\"dir\":\"" + d + "\"}");
            waitForResponse();
        }
        else if (choice == "4") {
            std::string name  = prompt("Name");
            std::string ageS  = prompt("Age");
            std::string grade = prompt("Grade");
            int age = 0; try { age = std::stoi(ageS); } catch (...) {}
            wsClient.send("{\"action\":\"create\",\"student\":{\"name\":\""
                          + Student::jsonEscape(name) + "\",\"age\":"
                          + std::to_string(age) + ",\"grade\":\""
                          + Student::jsonEscape(grade) + "\"}}");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        else if (choice == "5") {
            std::string idS   = prompt("Student ID to update");
            std::string name  = promptOpt("New name");
            std::string ageS  = promptOpt("New age");
            std::string grade = promptOpt("New grade");
            int id = 0;  try { id  = std::stoi(idS);  } catch (...) {}
            int age = 0; try { age = std::stoi(ageS); } catch (...) {}
            wsClient.send("{\"action\":\"update\",\"student\":{\"id\":"
                          + std::to_string(id) + ",\"name\":\""
                          + Student::jsonEscape(name) + "\",\"age\":"
                          + std::to_string(age) + ",\"grade\":\""
                          + Student::jsonEscape(grade) + "\"}}");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        else if (choice == "6") {
            std::string idS = prompt("Student ID to delete");
            int id = 0; try { id = std::stoi(idS); } catch (...) {}
            wsClient.send("{\"action\":\"delete\",\"id\":"
                          + std::to_string(id) + "}");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        else if (choice == "7") {
            std::lock_guard<std::mutex> lk(coutMtx_);
            std::cout << "\n" C_CYAN "── Local Performance Metrics ──\n" C_RESET;
            std::cout << "  Records in local CSV : " << stats.totalRecords << "\n";
            std::cout << "  CSV load time        : " << stats.loadTimeMs   << " ms\n";
        }
        else if (choice == "0" || std::cin.eof()) {
            break;
        }
        else {
            safePrint(C_RED "  Invalid choice.\n" C_RESET);
        }
    }

    safePrint(C_YELLOW "\n  Disconnecting...\n" C_RESET);
    wsClient.disconnect();
    safePrint(C_GREEN "  Goodbye.\n" C_RESET);
    return 0;
}
