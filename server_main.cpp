/**
 * server_main.cpp
 * ──────────────────────────────────────────────────────────────────────────────
 * Student Record Management System – Server
 *
 * Starts a WebSocket server, loads students.csv, and handles CRUD messages.
 * Every mutation is:
 *   1. Applied to the in-memory DB
 *   2. Persisted to CSV
 *   3. Broadcast to all connected clients
 */

#include "Logger.h"
#include "StudentDB.h"
#include "WebSocketServer.h"
#include "JsonUtil.h"

#include <iostream>
#include <string>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <sstream>

static std::atomic<bool> gRunning{true};

// ── Signal handler ─────────────────────────────────────────────────────────────
void onSignal(int) { gRunning = false; }

// ── Message dispatcher ─────────────────────────────────────────────────────────
void handleMessage(WebSocketServer& wss, StudentDB& db,
                   int clientId, const std::string& msg) {

    LOG_DEBUG("Received from client " + std::to_string(clientId) +
              ": " + msg.substr(0,120), "SRV");

    std::string action = json::extractStr(msg, "action");

    try {
        if (action == "list") {
            // Send full list to requesting client only
            auto students = db.listAll();
            std::string resp = json::makeList(students);
            wss.send(clientId, resp);
            return;
        }

        if (action == "search") {
            std::string q = json::extractStr(msg, "query");
            auto results = db.search(q);
            std::string resp = "{" + json::fieldStr("action","search_result") + "," +
                               json::field("students", json::studentArray(results)) + "}";
            wss.send(clientId, resp);
            return;
        }

        if (action == "sort") {
            std::string field = json::extractStr(msg, "field");
            std::string dir   = json::extractStr(msg, "dir");
            if (field.empty()) field = "id";
            double sortMs = db.sort(field, dir != "desc");
            auto students = db.listAll();
            std::string resp = json::makeList(students);
            wss.broadcast(resp);   // sorted view for all
            return;
        }

        if (action == "create") {
            std::string sJson = json::extractRaw(msg, "student");
            Student parsed    = json::parseStudent(sJson.empty() ? msg : sJson);
            if (parsed.name.empty())  throw std::invalid_argument("name is required");
            if (parsed.age <= 0)      throw std::invalid_argument("valid age is required");
            if (parsed.grade.empty()) throw std::invalid_argument("grade is required");

            Student created = db.create(parsed.name, parsed.age, parsed.grade);
            db.saveToFile();
            std::string resp = json::makeCreate(created);
            wss.broadcast(resp);
            return;
        }

        if (action == "update") {
            std::string sJson = json::extractRaw(msg, "student");
            Student parsed    = json::parseStudent(sJson.empty() ? msg : sJson);
            if (parsed.id <= 0) throw std::invalid_argument("id is required for update");

            auto result = db.update(parsed.id, parsed.name, parsed.age, parsed.grade);
            if (!result) {
                wss.send(clientId, json::makeError("Student id=" +
                         std::to_string(parsed.id) + " not found"));
                return;
            }
            db.saveToFile();
            std::string resp = json::makeUpdate(*result);
            wss.broadcast(resp);
            return;
        }

        if (action == "delete") {
            int id = json::extractInt(msg, "id");
            if (id <= 0) throw std::invalid_argument("id is required for delete");

            if (!db.remove(id)) {
                wss.send(clientId, json::makeError("Student id=" +
                         std::to_string(id) + " not found"));
                return;
            }
            db.saveToFile();
            std::string resp = json::makeDelete(id);
            wss.broadcast(resp);
            return;
        }

        // Client sends full dataset on startup (action: "init")
        if (action == "init") {
            // Just acknowledge; server has its own data
            wss.send(clientId, json::makeAck("Server initialised"));
            // Push current data to the new client
            auto students = db.listAll();
            wss.send(clientId, json::makeList(students));
            return;
        }

        LOG_WARN("Unknown action: " + action + " from client " +
                 std::to_string(clientId), "SRV");
        wss.send(clientId, json::makeError("Unknown action: " + action));

    } catch (const std::exception& e) {
        LOG_ERROR(std::string("Error handling '") + action + "': " + e.what(), "SRV");
        wss.send(clientId, json::makeError(e.what()));
    }
}

// ── Performance report ─────────────────────────────────────────────────────────
void printPerfReport(const StudentDB::Stats& stats,
                     const WebSocketServer::Metrics& wssMetrics) {
    std::cout << "\n\033[1;36m╔══════════════════════════════════════════╗\033[0m\n"
              << "\033[1;36m║       Performance Metrics Report         ║\033[0m\n"
              << "\033[1;36m╚══════════════════════════════════════════╝\033[0m\n";

    auto row = [](const std::string& k, const std::string& v) {
        std::cout << "  \033[33m" << k << "\033[0m: " << v << "\n";
    };

    row("Records processed",   std::to_string(stats.totalRecords));
    row("Load & parse time",   std::to_string(stats.loadTimeMs).substr(0,6) + " ms");
    row("Sort time",           std::to_string(stats.sortTimeMs).substr(0,6) + " ms  (last op)");
    row("CSV save time",       std::to_string(stats.saveTimeMs).substr(0,6) + " ms  (last op)");
    row("WebSocket TX time",   std::to_string(wssMetrics.lastSendMs).substr(0,6) + " ms  (last send)");
    row("Broadcast time",      std::to_string(wssMetrics.lastBroadcastMs).substr(0,6) + " ms  (last broadcast)");
    row("Messages sent",       std::to_string(wssMetrics.totalMessagesSent.load()));
    row("Messages received",   std::to_string(wssMetrics.totalMessagesReceived.load()));
    row("Bytes transmitted",   std::to_string(wssMetrics.totalBytesTransmitted.load()));
    row("Active clients",      std::to_string(wssMetrics.activeClients.load()));
    std::cout << "\n";
}

// ── main ───────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    Logger::instance().setLevel(LogLevel::DEBUG);
    Logger::instance().setLogFile("server.log");

    uint16_t    port    = 9001;
    std::string inputCsv  = "../data/students_input.csv";
    std::string outputCsv = "../data/students_output.csv";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port"  && i+1 < argc) port    = (uint16_t)std::stoi(argv[++i]);
        if (a == "--input"  && i+1 < argc) inputCsv  = argv[++i];
        if (a == "--output" && i+1 < argc) outputCsv = argv[++i];
    }

    LOG_INFO("═══════════════════════════════════════════", "SRV");
    LOG_INFO("  Student Record Management System - Server", "SRV");
    LOG_INFO("═══════════════════════════════════════════", "SRV");

    // ── Load data ──────────────────────────────────────────────────────────────
    StudentDB db(inputCsv, outputCsv);
    StudentDB::Stats stats;
    try {
        stats = db.loadFromFile();
        LOG_INFO("Loaded " + std::to_string(stats.totalRecords) +
                 " students from " + inputCsv + " (output: " + outputCsv + ")", "SRV");
    } catch (const std::exception& e) {
        LOG_WARN("Could not load CSV: " + std::string(e.what()) +
                 " – starting with empty DB", "SRV");
    }

    // ── Start WebSocket server ─────────────────────────────────────────────────
    WebSocketServer wss(port);

    wss.onConnect([&db, &wss](int clientId) {
        LOG_INFO("Client " + std::to_string(clientId) + " connected – sending initial data", "SRV");
        auto students = db.listAll();
        wss.send(clientId, json::makeList(students));
    });

    wss.onDisconnect([](int clientId) {
        LOG_INFO("Client " + std::to_string(clientId) + " disconnected", "SRV");
    });

    wss.onMessage([&db, &wss](int clientId, const std::string& msg) {
        handleMessage(wss, db, clientId, msg);
    });

    if (!wss.start()) {
        LOG_ERROR("Failed to start WebSocket server", "SRV");
        return 1;
    }

    std::cout << "\n\033[1;32m✓ Server running at ws://127.0.0.1:" << port << "\033[0m\n"
              << "\033[1;32m✓ Open web/index.html in your browser\033[0m\n"
              << "\033[1;32m✓ Or run: ./client\033[0m\n"
              << "  Press Ctrl+C to stop.\n\n";

    // ── Main loop ──────────────────────────────────────────────────────────────
    while (gRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG_INFO("Shutting down...", "SRV");
    printPerfReport(stats, wss.metrics());
    wss.stop();
    LOG_INFO("Goodbye.", "SRV");
    return 0;
}
