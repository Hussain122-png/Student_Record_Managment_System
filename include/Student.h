#pragma once

#include <string>
#include <sstream>
#include <stdexcept>

struct Student {
    int         id    = 0;
    std::string name;
    int         age   = 0;
    std::string grade;

    Student() = default;
    Student(int id, std::string name, int age, std::string grade)
        : id(id), name(std::move(name)), age(age), grade(std::move(grade)) {}

    // ── Serialisation ──────────────────────────────────────────────────────────
    std::string toCSV() const {
        return std::to_string(id) + "," + escapeCsv(name) + "," +
               std::to_string(age) + "," + escapeCsv(grade);
    }

    std::string toJSON() const {
        return "{\"id\":"    + std::to_string(id)  +
               ",\"name\":\"" + jsonEscape(name)   +
               "\",\"age\":"  + std::to_string(age) +
               ",\"grade\":\"" + jsonEscape(grade) + "\"}";
    }

    // ── Validation ─────────────────────────────────────────────────────────────
    bool isValid() const {
        return id > 0 && !name.empty() && age > 0 && age < 150 && !grade.empty();
    }

    // ── Helpers ────────────────────────────────────────────────────────────────
    static std::string escapeCsv(const std::string& s) {
        if (s.find(',') == std::string::npos &&
            s.find('"') == std::string::npos &&
            s.find('\n') == std::string::npos) return s;
        std::string r = "\"";
        for (char c : s) { if (c == '"') r += '"'; r += c; }
        r += '"';
        return r;
    }

    static std::string jsonEscape(const std::string& s) {
        std::string r;
        for (char c : s) {
            switch (c) {
                case '"':  r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n";  break;
                case '\r': r += "\\r";  break;
                case '\t': r += "\\t";  break;
                default:   r += c;
            }
        }
        return r;
    }

    bool operator==(const Student& o) const { return id == o.id; }
    bool operator< (const Student& o) const { return id <  o.id; }
};
