#pragma once

#include "Student.h"
#include "CSVParser.h"
#include "Logger.h"
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>
#include <optional>
#include <functional>
#include <sstream>
#include <chrono>
#include <stdexcept>

class StudentDB {
public:
    // ── Statistics emitted after bulk operations ───────────────────────────────
    struct Stats {
        size_t totalRecords = 0;
        double loadTimeMs   = 0.0;
        double sortTimeMs   = 0.0;
        double saveTimeMs   = 0.0;
    };

    // inputPath  = CSV to READ from on startup (never written to)
    // outputPath = CSV to WRITE to after every CRUD operation
    explicit StudentDB(std::string inputPath, std::string outputPath)
        : inputCsvPath_(std::move(inputPath)),
          outputCsvPath_(std::move(outputPath)) {}

    // ── Lifecycle ──────────────────────────────────────────────────────────────
    Stats loadFromFile() {
        std::lock_guard<std::mutex> lk(mutex_);
        auto result  = CSVParser::load(inputCsvPath_);
        students_    = std::move(result.students);
        nextId_      = maxId() + 1;
        Stats s;
        s.totalRecords = students_.size();
        s.loadTimeMs   = result.parseTimeMs;
        return s;
    }

    double saveToFile() {
        // caller must NOT hold lock – save acquires it briefly to copy
        std::vector<Student> snap;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            snap = students_;
        }
        double ms = CSVParser::save(outputCsvPath_, snap);
        return ms;
    }

    // ── CREATE ─────────────────────────────────────────────────────────────────
    Student create(const std::string& name, int age, const std::string& grade) {
        std::lock_guard<std::mutex> lk(mutex_);
        Student s(nextId_++, name, age, grade);
        if (!s.isValid()) throw std::invalid_argument("Invalid student data");
        students_.push_back(s);
        LOG_INFO("Created student id=" + std::to_string(s.id) + " name=" + s.name, "DB");
        return s;
    }

    // Insert with explicit id (used when client transmits records on startup)
    void insertOrReplace(const Student& s) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = findById_(s.id);
        if (it != students_.end()) *it = s;
        else students_.push_back(s);
        if (s.id >= nextId_) nextId_ = s.id + 1;
    }

    // ── READ ───────────────────────────────────────────────────────────────────
    std::vector<Student> listAll() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return students_;
    }

    std::optional<Student> findById(int id) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = findById_(id);
        if (it == students_.end()) return std::nullopt;
        return *it;
    }

    std::vector<Student> search(const std::string& query) const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::string q = toLower(query);
        std::vector<Student> res;
        for (const auto& s : students_) {
            if (toLower(s.name).find(q) != std::string::npos ||
                std::to_string(s.id) == query)
                res.push_back(s);
        }
        return res;
    }

    // ── UPDATE ─────────────────────────────────────────────────────────────────
    std::optional<Student> update(int id,
                                  const std::string& name,
                                  int age,
                                  const std::string& grade) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = findById_(id);
        if (it == students_.end()) return std::nullopt;
        if (!name.empty())  it->name  = name;
        if (age > 0)        it->age   = age;
        if (!grade.empty()) it->grade = grade;
        LOG_INFO("Updated student id=" + std::to_string(id), "DB");
        return *it;
    }

    // ── DELETE ─────────────────────────────────────────────────────────────────
    bool remove(int id) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = findById_(id);
        if (it == students_.end()) return false;
        students_.erase(it);
        LOG_INFO("Deleted student id=" + std::to_string(id), "DB");
        return true;
    }

    // ── SORT ───────────────────────────────────────────────────────────────────
    double sort(const std::string& field, bool ascending = true) {
        auto t0 = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(mutex_);

        auto cmp = [&](const Student& a, const Student& b) -> bool {
            bool less;
            if      (field == "name")  less = a.name  < b.name;
            else if (field == "age")   less = a.age   < b.age;
            else if (field == "grade") less = a.grade < b.grade;
            else                       less = a.id    < b.id;
            return ascending ? less : !less;
        };
        std::sort(students_.begin(), students_.end(), cmp);

        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
        LOG_INFO("Sorted by '" + field + "' (" +
                 (ascending ? "asc" : "desc") + ") in " +
                 fmtMs(ms) + " ms", "DB");
        return ms;
    }

    // ── JSON snapshot ──────────────────────────────────────────────────────────
    std::string toJSON() const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::ostringstream ss;
        ss << "[";
        for (size_t i = 0; i < students_.size(); ++i) {
            if (i) ss << ",";
            ss << students_[i].toJSON();
        }
        ss << "]";
        return ss.str();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return students_.size();
    }

private:
    // ── Internals (caller holds mutex_) ───────────────────────────────────────
    using Iter = std::vector<Student>::iterator;
    using CIter = std::vector<Student>::const_iterator;

    Iter findById_(int id) {
        return std::find_if(students_.begin(), students_.end(),
                            [id](const Student& s){ return s.id == id; });
    }
    CIter findById_(int id) const {
        return std::find_if(students_.begin(), students_.end(),
                            [id](const Student& s){ return s.id == id; });
    }

    int maxId() const {
        if (students_.empty()) return 0;
        return std::max_element(students_.begin(), students_.end(),
               [](const Student& a, const Student& b){ return a.id < b.id; })->id;
    }

    static std::string toLower(const std::string& s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(), ::tolower);
        return r;
    }

    static std::string fmtMs(double ms) {
        std::ostringstream ss; ss.precision(3); ss << std::fixed << ms;
        return ss.str();
    }

    mutable std::mutex   mutex_;
    std::vector<Student> students_;
    int                  nextId_ = 1;
    std::string          inputCsvPath_;
    std::string          outputCsvPath_;
};
