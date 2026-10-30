#include "memory.hpp"

#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <sqlite3.h>

namespace wsi {
namespace {
constexpr int kMaxDone = 50;

void check(int rc, sqlite3* db, const char* what) {
    if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) return;
    const char* message = db != nullptr ? sqlite3_errmsg(db) : what;
    throw std::runtime_error(std::string(what) + ": " + (message != nullptr ? message : "sqlite error"));
}
}  // namespace

struct SessionMemory::Impl {
    sqlite3* db = nullptr;
    mutable std::mutex mu;

    ~Impl() {
        if (db != nullptr) sqlite3_close(db);
    }
};

SessionMemory::SessionMemory(std::string path) : impl_(std::make_unique<Impl>()) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    check(sqlite3_open_v2(path.c_str(), &impl_->db, flags, nullptr), nullptr, "sqlite open");
    sqlite3_busy_timeout(impl_->db, 5000);
    const char* sql =
        "CREATE TABLE IF NOT EXISTS actions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id TEXT NOT NULL,"
        "  character TEXT NOT NULL,"
        "  action TEXT NOT NULL,"
        "  args TEXT NOT NULL,"
        "  ok INTEGER NOT NULL,"
        "  error TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS actions_by_session ON actions(session_id, id);";
    char* error = nullptr;
    const int rc = sqlite3_exec(impl_->db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string message = error != nullptr ? error : "sqlite exec";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

SessionMemory::~SessionMemory() { close(); }

void SessionMemory::record(const std::string& session_id, const std::string& character, const Json& entry) {
    Json args = Json::object();
    if (entry.contains("args") && entry.at("args").is_object()) args = entry.at("args");
    const std::string action = entry.value("action", "");
    const bool ok = entry.value("ok", false);
    std::string error;
    const bool has_error = entry.contains("error") && entry.at("error").is_string() && !entry.at("error").get<std::string>().empty();
    if (has_error) error = entry.at("error").get<std::string>();

    std::lock_guard lock(impl_->mu);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO actions (session_id, character, action, args, ok, error) VALUES (?, ?, ?, ?, ?, ?)";
    check(sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr), impl_->db, "sqlite prepare");
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, character.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, action.c_str(), -1, SQLITE_TRANSIENT);
    const std::string args_text = args.dump();
    sqlite3_bind_text(stmt, 4, args_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, ok ? 1 : 0);
    if (has_error) sqlite3_bind_text(stmt, 6, error.c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 6);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    check(rc, impl_->db, "sqlite insert");
}

Json SessionMemory::done(const std::string& session_id) const {
    std::lock_guard lock(impl_->mu);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT action, args, ok, error FROM ("
        "  SELECT id, action, args, ok, error FROM actions"
        "  WHERE session_id = ?"
        "  ORDER BY id DESC"
        "  LIMIT ?"
        ") ORDER BY id ASC";
    check(sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr), impl_->db, "sqlite prepare");
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, kMaxDone);
    Json result = Json::array();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* action = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const auto* args = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const int ok = sqlite3_column_int(stmt, 2);
        Json entry = Json{
            {"action", action != nullptr ? action : ""},
            {"args", args != nullptr ? Json::parse(args) : Json::object()},
            {"ok", ok == 1},
        };
        if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
            const auto* error = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            if (error != nullptr && error[0] != '\0') entry["error"] = error;
        }
        result.push_back(std::move(entry));
    }
    sqlite3_finalize(stmt);
    return result;
}

void SessionMemory::close() {
    if (impl_ == nullptr || impl_->db == nullptr) return;
    std::lock_guard lock(impl_->mu);
    sqlite3_close(impl_->db);
    impl_->db = nullptr;
}

Json view_with_done(const Json& view, const Json& done) {
    if (view.is_object()) {
        Json copy = view;
        copy["done"] = done;
        return copy;
    }
    return Json{{"view", view}, {"done", done}};
}

std::string default_memory_path() {
    return (std::filesystem::current_path() / ".wsi" / "agent-memory.sqlite").string();
}

}  // namespace wsi
