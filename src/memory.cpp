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
        "CREATE INDEX IF NOT EXISTS actions_by_session ON actions(session_id, id);"
        "CREATE TABLE IF NOT EXISTS summaries ("
        "  session_id TEXT PRIMARY KEY,"
        "  character TEXT NOT NULL,"
        "  counts TEXT NOT NULL,"
        "  last_error TEXT"
        ");";
    char* error = nullptr;
    const int rc = sqlite3_exec(impl_->db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string message = error != nullptr ? error : "sqlite exec";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

SessionMemory::~SessionMemory() { close(); }

namespace {

void exec_sql(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc == SQLITE_OK) return;
    std::string message = error != nullptr ? error : "sqlite exec";
    sqlite3_free(error);
    throw std::runtime_error(message);
}

struct Transaction {
    sqlite3* db = nullptr;
    bool open = false;

    explicit Transaction(sqlite3* database) : db(database) {
        exec_sql(db, "BEGIN");
        open = true;
    }

    void commit() {
        exec_sql(db, "COMMIT");
        open = false;
    }

    ~Transaction() {
        if (open) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
};

void fold_overflow(sqlite3* db, const std::string& session_id, const std::string& character) {
    sqlite3_stmt* count_stmt = nullptr;
    const char* count_sql = "SELECT COUNT(*) FROM actions WHERE session_id = ?";
    check(sqlite3_prepare_v2(db, count_sql, -1, &count_stmt, nullptr), db, "sqlite prepare");
    sqlite3_bind_text(count_stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(count_stmt), db, "sqlite count");
    const int count = sqlite3_column_int(count_stmt, 0);
    sqlite3_finalize(count_stmt);
    if (count <= kMaxDone) return;

    const int overflow = count - kMaxDone;
    sqlite3_stmt* select_stmt = nullptr;
    const char* select_sql =
        "SELECT id, action, error FROM actions WHERE session_id = ? ORDER BY id ASC LIMIT ?";
    check(sqlite3_prepare_v2(db, select_sql, -1, &select_stmt, nullptr), db, "sqlite prepare");
    sqlite3_bind_text(select_stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(select_stmt, 2, overflow);

    Json counts = Json::object();
    std::string last_error;
    bool saw_error = false;
    sqlite3_int64 last_id = 0;
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(select_stmt)) == SQLITE_ROW) {
        last_id = sqlite3_column_int64(select_stmt, 0);
        const auto* action = reinterpret_cast<const char*>(sqlite3_column_text(select_stmt, 1));
        const std::string name = action != nullptr ? action : "";
        counts[name] = counts.value(name, 0) + 1;
        if (sqlite3_column_type(select_stmt, 2) != SQLITE_NULL) {
            const auto* error = reinterpret_cast<const char*>(sqlite3_column_text(select_stmt, 2));
            if (error != nullptr && error[0] != '\0') {
                last_error = error;
                saw_error = true;
            }
        }
    }
    sqlite3_finalize(select_stmt);
    if (step != SQLITE_DONE) check(step, db, "sqlite fold");
    if (last_id == 0) return;

    sqlite3_stmt* previous = nullptr;
    const char* previous_sql = "SELECT counts, last_error FROM summaries WHERE session_id = ?";
    check(sqlite3_prepare_v2(db, previous_sql, -1, &previous, nullptr), db, "sqlite prepare");
    sqlite3_bind_text(previous, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    const int previous_rc = sqlite3_step(previous);
    if (previous_rc == SQLITE_ROW) {
        const auto* stored = reinterpret_cast<const char*>(sqlite3_column_text(previous, 0));
        if (stored != nullptr) {
            const Json existing = Json::parse(stored);
            if (existing.is_object()) {
                for (auto it = existing.begin(); it != existing.end(); ++it) {
                    counts[it.key()] = counts.value(it.key(), 0) + it.value().get<int>();
                }
            }
        }
        if (!saw_error && sqlite3_column_type(previous, 1) != SQLITE_NULL) {
            const auto* error = reinterpret_cast<const char*>(sqlite3_column_text(previous, 1));
            if (error != nullptr) last_error = error;
        }
    } else if (previous_rc != SQLITE_DONE) {
        sqlite3_finalize(previous);
        check(previous_rc, db, "sqlite summary");
    }
    sqlite3_finalize(previous);

    sqlite3_stmt* upsert = nullptr;
    const char* upsert_sql =
        "INSERT INTO summaries (session_id, character, counts, last_error) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(session_id) DO UPDATE SET character = excluded.character, counts = excluded.counts, "
        "last_error = excluded.last_error";
    check(sqlite3_prepare_v2(db, upsert_sql, -1, &upsert, nullptr), db, "sqlite prepare");
    sqlite3_bind_text(upsert, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(upsert, 2, character.c_str(), -1, SQLITE_TRANSIENT);
    const std::string counts_text = counts.dump();
    sqlite3_bind_text(upsert, 3, counts_text.c_str(), -1, SQLITE_TRANSIENT);
    if (last_error.empty()) sqlite3_bind_null(upsert, 4);
    else sqlite3_bind_text(upsert, 4, last_error.c_str(), -1, SQLITE_TRANSIENT);
    const int upsert_rc = sqlite3_step(upsert);
    sqlite3_finalize(upsert);
    check(upsert_rc, db, "sqlite summary");

    sqlite3_stmt* remove = nullptr;
    const char* remove_sql = "DELETE FROM actions WHERE session_id = ? AND id <= ?";
    check(sqlite3_prepare_v2(db, remove_sql, -1, &remove, nullptr), db, "sqlite prepare");
    sqlite3_bind_text(remove, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(remove, 2, last_id);
    const int remove_rc = sqlite3_step(remove);
    sqlite3_finalize(remove);
    check(remove_rc, db, "sqlite delete");
}

Json summary_entry(sqlite3* db, const std::string& session_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT counts, last_error FROM summaries WHERE session_id = ?";
    check(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "sqlite prepare");
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    Json entry = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* stored = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        entry = Json{{"summary", stored != nullptr ? Json::parse(stored) : Json::object()}};
        if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
            const auto* error = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (error != nullptr && error[0] != '\0') entry["lastError"] = error;
        }
    }
    sqlite3_finalize(stmt);
    return entry;
}

}  // namespace

void SessionMemory::record(const std::string& session_id, const std::string& character, const Json& entry) {
    Json args = Json::object();
    if (entry.contains("args") && entry.at("args").is_object()) args = entry.at("args");
    const std::string action = entry.value("action", "");
    const bool ok = entry.value("ok", false);
    std::string error;
    const bool has_error = entry.contains("error") && entry.at("error").is_string() && !entry.at("error").get<std::string>().empty();
    if (has_error) error = entry.at("error").get<std::string>();

    std::lock_guard lock(impl_->mu);
    if (impl_->db == nullptr) return;
    Transaction tx(impl_->db);
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
    fold_overflow(impl_->db, session_id, character);
    tx.commit();
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
    const Json summary = summary_entry(impl_->db, session_id);
    if (summary.is_object()) result.push_back(summary);
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

void SessionMemory::forget(const std::string& session_id) {
    std::lock_guard lock(impl_->mu);
    if (impl_->db == nullptr) return;
    Transaction tx(impl_->db);
    sqlite3_stmt* actions = nullptr;
    const char* actions_sql = "DELETE FROM actions WHERE session_id = ?";
    check(sqlite3_prepare_v2(impl_->db, actions_sql, -1, &actions, nullptr), impl_->db, "sqlite prepare");
    sqlite3_bind_text(actions, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    const int actions_rc = sqlite3_step(actions);
    sqlite3_finalize(actions);
    check(actions_rc, impl_->db, "sqlite delete");

    sqlite3_stmt* summary = nullptr;
    const char* summary_sql = "DELETE FROM summaries WHERE session_id = ?";
    check(sqlite3_prepare_v2(impl_->db, summary_sql, -1, &summary, nullptr), impl_->db, "sqlite prepare");
    sqlite3_bind_text(summary, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    const int summary_rc = sqlite3_step(summary);
    sqlite3_finalize(summary);
    check(summary_rc, impl_->db, "sqlite delete");
    tx.commit();
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
