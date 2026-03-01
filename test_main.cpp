/**
 * test_main.cpp
 * ──────────────────────────────────────────────────────────────────────────────
 * Basic unit tests for StudentDB, CSVParser, Crypto, and JsonUtil.
 * No external test framework – just plain assertions.
 */

#include "Logger.h"
#include "Student.h"
#include "CSVParser.h"
#include "StudentDB.h"
#include "Crypto.h"
#include "JsonUtil.h"

#include <cassert>
#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>

// ── Helpers ───────────────────────────────────────────────────────────────────
static int passed = 0;
static int failed = 0;

#define CHECK(cond, name)                                       \
    do {                                                        \
        if (cond) {                                             \
            std::cout << "  \033[32m✓ " << (name) << "\033[0m\n"; \
            ++passed;                                           \
        } else {                                                \
            std::cout << "  \033[31m✗ " << (name) << "\033[0m\n"; \
            ++failed;                                           \
        }                                                       \
    } while(0)

void writeTmpCSV(const std::string& path) {
    std::ofstream f(path);
    f << "id,name,age,grade\n"
      << "1,Alice,20,A\n"
      << "2,Bob,21,B+\n"
      << "3,Carol,19,A-\n";
}

// ── Test suites ───────────────────────────────────────────────────────────────

void testStudent() {
    std::cout << "\n\033[1mStudent model\033[0m\n";
    Student s(1, "Alice", 20, "A");
    CHECK(s.toCSV() == "1,Alice,20,A", "toCSV basic");
    CHECK(s.toJSON().find("\"name\":\"Alice\"") != std::string::npos, "toJSON name");
    CHECK(s.isValid(), "isValid");

    Student bad(0, "", -1, "");
    CHECK(!bad.isValid(), "invalid student");

    // CSV escape
    Student esc(2, "O'Brien, Jr.", 22, "B");
    std::string csv = esc.toCSV();
    CHECK(csv.find('"') != std::string::npos, "CSV escapes comma in name");
}

void testCrypto() {
    std::cout << "\n\033[1mCrypto (SHA-1 + Base64)\033[0m\n";

    // SHA-1 known test vector: SHA1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d
    auto digest = sha1("abc");
    std::string hex;
    for (unsigned char c : digest) {
        char buf[3]; snprintf(buf, 3, "%02x", (unsigned)c);
        hex += buf;
    }
    CHECK(hex == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA1('abc')");

    // Base64 known vector: base64("Man") = "TWFu"
    CHECK(base64Encode("Man") == "TWFu", "Base64 'Man'");
    CHECK(base64Encode("hello") == "aGVsbG8=", "Base64 'hello'");
    CHECK(base64Decode("aGVsbG8=") == "hello", "Base64 decode");

    // WebSocket accept key
    std::string key    = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string accept = wsAcceptKey(key);
    CHECK(accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "wsAcceptKey RFC example");
}

void testCSVParser() {
    std::cout << "\n\033[1mCSVParser\033[0m\n";

    const char* path = "/tmp/test_students.csv";
    writeTmpCSV(path);

    auto result = CSVParser::load(path);
    CHECK(result.students.size() == 3,   "loaded 3 records");
    CHECK(result.rowsSkipped == 0,        "no rows skipped");
    CHECK(result.students[0].name == "Alice", "first name");
    CHECK(result.students[1].age  == 21,      "second age");
    CHECK(result.students[2].grade == "A-",   "third grade");
    CHECK(result.parseTimeMs >= 0.0,           "parse time recorded");

    // Save and reload
    const char* out = "/tmp/test_out.csv";
    CSVParser::save(out, result.students);
    auto result2 = CSVParser::load(out);
    CHECK(result2.students.size() == 3, "reload after save");

    std::remove(path);
    std::remove(out);
}

void testStudentDB() {
    std::cout << "\n\033[1mStudentDB CRUD\033[0m\n";

    const char* path = "/tmp/test_db.csv";
    writeTmpCSV(path);

    StudentDB db(path, path);
    auto stats = db.loadFromFile();
    CHECK(stats.totalRecords == 3, "loaded 3 records");

    // Create
    Student created = db.create("Dave", 23, "B");
    CHECK(created.id >= 4,           "create assigns id");
    CHECK(created.name == "Dave",    "create name");
    CHECK(db.size() == 4,            "size after create");

    // Read
    auto found = db.findById(created.id);
    CHECK(found.has_value(),         "findById found");
    CHECK(found->name == "Dave",     "findById correct name");

    auto notFound = db.findById(9999);
    CHECK(!notFound.has_value(),     "findById returns nullopt for missing");

    // Search
    auto results = db.search("alice");
    CHECK(results.size() == 1,       "search by name (case-insensitive)");
    CHECK(results[0].name == "Alice","search result correct");

    // Update
    auto updated = db.update(created.id, "David", 24, "A");
    CHECK(updated.has_value(),        "update returns student");
    CHECK(updated->name == "David",   "update name");
    CHECK(updated->age == 24,         "update age");

    // Sort
    double sortMs = db.sort("name");
    auto all = db.listAll();
    CHECK(sortMs >= 0.0,              "sort time >= 0");
    CHECK(all.front().name <= all.back().name, "sorted by name ascending");

    // Delete
    bool deleted = db.remove(1);
    CHECK(deleted,               "delete returns true");
    CHECK(db.size() == 3,        "size after delete");
    bool notDeleted = db.remove(9999);
    CHECK(!notDeleted,           "delete non-existent returns false");

    // Persist
    db.saveToFile();
    StudentDB db2(path, path);
    db2.loadFromFile();
    CHECK(db2.size() == 3,       "persisted count matches after reload");

    std::remove(path);
}

void testJsonUtil() {
    std::cout << "\n\033[1mJsonUtil\033[0m\n";

    Student s(5, "Eva Martinez", 20, "A+");
    std::string j = json::makeCreate(s);
    CHECK(j.find("\"action\":\"created\"") != std::string::npos, "makeCreate action");
    CHECK(j.find("Eva Martinez") != std::string::npos,           "makeCreate name");

    std::string del = json::makeDelete(5);
    CHECK(json::extractInt(del, "id") == 5, "extractInt id");

    std::string err = json::makeError("not found");
    CHECK(json::extractStr(err, "message") == "not found", "extractStr message");

    // parseStudent
    Student parsed = json::parseStudent("{\"id\":7,\"name\":\"Frank\",\"age\":22,\"grade\":\"B\"}");
    CHECK(parsed.id    == 7,       "parseStudent id");
    CHECK(parsed.name  == "Frank", "parseStudent name");
    CHECK(parsed.age   == 22,      "parseStudent age");
    CHECK(parsed.grade == "B",     "parseStudent grade");
}

// ── main ───────────────────────────────────────────────────────────────────────
int main() {
    Logger::instance().setLevel(LogLevel::ERROR); // suppress during tests

    std::cout << "\033[1;36m\n══════════════════════════════════\n"
              << "  Student Record System – Tests\n"
              << "══════════════════════════════════\033[0m\n";

    testStudent();
    testCrypto();
    testCSVParser();
    testStudentDB();
    testJsonUtil();

    std::cout << "\n\033[1m──────────────────────────────────\033[0m\n"
              << "  Passed: \033[32m" << passed << "\033[0m  "
              << "Failed: \033[31m" << failed << "\033[0m\n\n";

    return failed > 0 ? 1 : 0;
}
