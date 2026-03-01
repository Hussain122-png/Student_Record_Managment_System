#pragma once
// Minimal JSON builder + enough parsing for our protocol.
// Not a general-purpose JSON library – handles the fixed message schemas used here.

#include "Student.h"
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <stdexcept>

namespace json {

// ── Helpers ───────────────────────────────────────────────────────────────────
inline std::string str(const std::string& v) {
    return "\"" + Student::jsonEscape(v) + "\"";
}
// fieldStr: key + quoted string value
inline std::string fieldStr(const std::string& k, const std::string& v) {
    return str(k) + ":" + str(v);
}
// field: key + raw JSON value (object, array, number, bool)
inline std::string field(const std::string& k, const std::string& v) {
    return str(k) + ":" + v;
}
inline std::string field(const std::string& k, int v) {
    return str(k) + ":" + std::to_string(v);
}
inline std::string field(const std::string& k, double v) {
    std::ostringstream ss; ss.precision(3); ss << std::fixed << v;
    return str(k) + ":" + ss.str();
}

// ── Message builders ──────────────────────────────────────────────────────────
inline std::string studentArray(const std::vector<Student>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) { if(i) s+=","; s+=v[i].toJSON(); }
    return s + "]";
}

inline std::string makeList(const std::vector<Student>& students) {
    return "{" + fieldStr("action","list") + "," +
           field("students", studentArray(students)) + "}";
}
inline std::string makeCreate(const Student& s) {
    return "{" + fieldStr("action","created") + "," +
           field("student", s.toJSON()) + "}";
}
inline std::string makeUpdate(const Student& s) {
    return "{" + fieldStr("action","updated") + "," +
           field("student", s.toJSON()) + "}";
}
inline std::string makeDelete(int id) {
    return "{" + fieldStr("action","deleted") + "," + field("id", id) + "}";
}
inline std::string makeError(const std::string& msg) {
    return "{" + fieldStr("action","error") + "," + fieldStr("message", msg) + "}";
}
inline std::string makeAck(const std::string& msg) {
    return "{" + fieldStr("action","ack") + "," + fieldStr("message", msg) + "}";
}
inline std::string makeStats(size_t records, double loadMs, double sortMs,
                             double saveMs, double txMs, double broadcastMs) {
    return "{" + fieldStr("action","stats") + "," +
           field("records",(int)records) + "," +
           field("loadMs",loadMs) + "," +
           field("sortMs",sortMs) + "," +
           field("saveMs",saveMs) + "," +
           field("txMs",txMs) + "," +
           field("broadcastMs",broadcastMs) + "}";
}

// ── Minimal parser ────────────────────────────────────────────────────────────
// Extracts the value of a string key from a flat JSON object (no nesting).
inline std::string extractStr(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    // skip whitespace and colon
    while (pos < json.size() && (json[pos]==' '||json[pos]==':')) ++pos;
    if (pos >= json.size()) return {};
    if (json[pos] == '"') {
        ++pos;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos+1 < json.size()) { ++pos; }
            val += json[pos++];
        }
        return val;
    }
    // number / boolean / null
    size_t end = pos;
    while (end < json.size() && json[end]!=',' && json[end]!='}') ++end;
    std::string v = json.substr(pos, end-pos);
    // trim
    v.erase(v.begin(), std::find_if(v.begin(),v.end(),[](char c){return !isspace(c);}));
    v.erase(std::find_if(v.rbegin(),v.rend(),[](char c){return !isspace(c);}).base(),v.end());
    return v;
}

inline int extractInt(const std::string& json, const std::string& key, int def=0) {
    auto s = extractStr(json, key);
    if (s.empty()) return def;
    try { return std::stoi(s); } catch(...) { return def; }
}

// Extract the raw value (object or array) for a key.
// Correctly skips string content so embedded brackets/braces inside
// quoted names or grades don't corrupt the depth counter.
inline std::string extractRaw(const std::string& j, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = j.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < j.size() && (j[pos]==' ' || j[pos]==':')) ++pos;
    if (pos >= j.size()) return {};

    char open = j[pos];
    if (open == '{' || open == '[') {
        char close = (open == '{') ? '}' : ']';
        int    depth  = 0;
        size_t start  = pos;
        bool   inStr  = false;
        bool   escape = false;
        while (pos < j.size()) {
            char c = j[pos];
            if (escape)      { escape = false; }
            else if (c=='\\') { if (inStr) escape = true; }
            else if (c=='"') { inStr = !inStr; }
            else if (!inStr) {
                if      (c == open)  { ++depth; }
                else if (c == close) { --depth; if (!depth) return j.substr(start, pos-start+1); }
            }
            ++pos;
        }
        return {};
    }
    return extractStr(j, key);
}

// Parse a simple student JSON object
inline Student parseStudent(const std::string& json) {
    Student s;
    s.id    = extractInt(json, "id");
    s.name  = extractStr(json, "name");
    s.age   = extractInt(json, "age");
    s.grade = extractStr(json, "grade");
    return s;
}

} // namespace json
