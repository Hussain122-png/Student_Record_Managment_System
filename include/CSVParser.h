#pragma once

#include "Student.h"
#include "Logger.h"
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <chrono>

class CSVParser {
public:
    struct ParseResult {
        std::vector<Student> students;
        double               parseTimeMs = 0.0;
        int                  rowsRead    = 0;
        int                  rowsSkipped = 0;
    };

    // ── Load ───────────────────────────────────────────────────────────────────
    static ParseResult load(const std::string& filepath) {
        ParseResult result;
        auto t0 = now();

        std::ifstream file(filepath);
        if (!file.is_open())
            throw std::runtime_error("Cannot open file: " + filepath);

        std::string line;
        int lineNum = 0;

        // skip header
        if (!std::getline(file, line)) {
            LOG_WARN("CSV file is empty: " + filepath, "CSV");
            return result;
        }
        lineNum++;

        while (std::getline(file, line)) {
            lineNum++;
            if (line.empty()) continue;
            try {
                Student s = parseLine(line);
                if (s.isValid()) {
                    result.students.push_back(s);
                    result.rowsRead++;
                } else {
                    LOG_WARN("Invalid record at line " + std::to_string(lineNum), "CSV");
                    result.rowsSkipped++;
                }
            } catch (const std::exception& e) {
                LOG_WARN("Parse error at line " + std::to_string(lineNum) +
                         ": " + e.what(), "CSV");
                result.rowsSkipped++;
            }
        }

        result.parseTimeMs = elapsed(t0);
        LOG_INFO("Loaded " + std::to_string(result.rowsRead) + " records in " +
                 fmt(result.parseTimeMs) + " ms", "CSV");
        return result;
    }

    // ── Save ───────────────────────────────────────────────────────────────────
    static double save(const std::string& filepath, const std::vector<Student>& students) {
        auto t0 = now();

        std::ofstream file(filepath, std::ios::trunc);
        if (!file.is_open())
            throw std::runtime_error("Cannot write file: " + filepath);

        file << "id,name,age,grade\n";
        for (const auto& s : students)
            file << s.toCSV() << "\n";

        double ms = elapsed(t0);
        LOG_INFO("Saved " + std::to_string(students.size()) + " records in " +
                 fmt(ms) + " ms", "CSV");
        return ms;
    }

private:
    static Student parseLine(const std::string& line) {
        auto fields = splitCSV(line);
        if (fields.size() < 4)
            throw std::runtime_error("Expected 4 fields, got " +
                                     std::to_string(fields.size()));
        Student s;
        s.id    = std::stoi(trim(fields[0]));
        s.name  = trim(fields[1]);
        s.age   = std::stoi(trim(fields[2]));
        s.grade = trim(fields[3]);
        return s;
    }

    // RFC 4180-compliant CSV splitter
    static std::vector<std::string> splitCSV(const std::string& line) {
        std::vector<std::string> fields;
        std::string field;
        bool inQuotes = false;

        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (inQuotes) {
                if (c == '"') {
                    if (i + 1 < line.size() && line[i+1] == '"') { field += '"'; ++i; }
                    else inQuotes = false;
                } else {
                    field += c;
                }
            } else {
                if (c == '"') inQuotes = true;
                else if (c == ',') { fields.push_back(field); field.clear(); }
                else field += c;
            }
        }
        fields.push_back(field);
        return fields;
    }

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    }

    using Tp = std::chrono::steady_clock::time_point;
    static Tp   now()            { return std::chrono::steady_clock::now(); }
    static double elapsed(Tp t0) {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0).count();
    }
    static std::string fmt(double ms) {
        std::ostringstream ss; ss.precision(3); ss << std::fixed << ms;
        return ss.str();
    }
};
