#pragma once

#include <string>
#include <mutex>
#include <fstream>
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

enum class LogLevel { DEBUG, INFO, WARNING, ERROR };

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    // ── Configuration ──────────────────────────────────────────────────────────
    // consoleLevel : minimum level printed to stdout
    // fileLevel    : minimum level written to the log file (independent)
    //
    // Common pattern:
    //   Server: both DEBUG  → verbose everywhere
    //   Client: console=WARNING (quiet UI), file=INFO (full audit trail)
    void setConsoleLevel(LogLevel level) { consoleLevel_ = level; }
    void setFileLevel   (LogLevel level) { fileLevel_    = level; }

    // Convenience: set both at once (backward-compatible)
    void setLevel(LogLevel level) {
        consoleLevel_ = level;
        fileLevel_    = level;
    }

    void setLogFile(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fileStream_.is_open()) fileStream_.close();
        fileStream_.open(path, std::ios::app);
        if (!fileStream_.is_open())
            std::cerr << "[Logger] WARNING: could not open log file: " << path << "\n";
    }

    // ── Logging ────────────────────────────────────────────────────────────────
    void log(LogLevel level, const std::string& msg,
             const std::string& component = "SYSTEM") {
        bool toConsole = (level >= consoleLevel_);
        bool toFile    = (level >= fileLevel_) && fileStream_.is_open();
        if (!toConsole && !toFile) return;

        std::lock_guard<std::mutex> lock(mutex_);
        if (toConsole) {
            std::cout << formatEntry(level, component, msg, /*colour=*/true) << "\n";
        }
        if (toFile) {
            fileStream_ << formatEntry(level, component, msg, /*colour=*/false) << "\n";
            fileStream_.flush(); // ensure it hits disk immediately
        }
    }

    void debug  (const std::string& m, const std::string& c = "SYSTEM") { log(LogLevel::DEBUG,   m, c); }
    void info   (const std::string& m, const std::string& c = "SYSTEM") { log(LogLevel::INFO,    m, c); }
    void warning(const std::string& m, const std::string& c = "SYSTEM") { log(LogLevel::WARNING, m, c); }
    void error  (const std::string& m, const std::string& c = "SYSTEM") { log(LogLevel::ERROR,   m, c); }

private:
    Logger()
        : consoleLevel_(LogLevel::DEBUG)
        , fileLevel_(LogLevel::DEBUG)
    {}
    ~Logger() { if (fileStream_.is_open()) fileStream_.close(); }

    std::string levelToStrColour(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG:   return "\033[36mDEBUG\033[0m";
            case LogLevel::INFO:    return "\033[32mINFO \033[0m";
            case LogLevel::WARNING: return "\033[33mWARN \033[0m";
            case LogLevel::ERROR:   return "\033[31mERROR\033[0m";
        }
        return "UNKNW";
    }
    std::string levelToStrPlain(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG:   return "DEBUG";
            case LogLevel::INFO:    return "INFO ";
            case LogLevel::WARNING: return "WARN ";
            case LogLevel::ERROR:   return "ERROR";
        }
        return "UNKNW";
    }

    std::string formatEntry(LogLevel level, const std::string& comp,
                            const std::string& msg, bool colour) {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()) % 1000;
        std::ostringstream ss;
        ss << "[" << std::put_time(std::localtime(&t), "%H:%M:%S")
           << "." << std::setw(3) << std::setfill('0') << ms.count() << "] "
           << "[" << (colour ? levelToStrColour(level) : levelToStrPlain(level)) << "] "
           << "[" << comp << "] " << msg;
        return ss.str();
    }

    std::mutex    mutex_;
    std::ofstream fileStream_;
    LogLevel      consoleLevel_;
    LogLevel      fileLevel_;
};

#define LOG_DEBUG(msg, ...)   Logger::instance().debug(msg,   ##__VA_ARGS__)
#define LOG_INFO(msg, ...)    Logger::instance().info(msg,    ##__VA_ARGS__)
#define LOG_WARN(msg, ...)    Logger::instance().warning(msg, ##__VA_ARGS__)
#define LOG_ERROR(msg, ...)   Logger::instance().error(msg,   ##__VA_ARGS__)
