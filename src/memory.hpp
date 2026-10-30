#pragma once

#include "wsi/json.hpp"

#include <memory>
#include <string>

struct sqlite3;

namespace wsi {

class SessionMemory {
 public:
    explicit SessionMemory(std::string path);
    ~SessionMemory();

    SessionMemory(const SessionMemory&) = delete;
    SessionMemory& operator=(const SessionMemory&) = delete;

    void record(const std::string& session_id, const std::string& character, const Json& entry);
    Json done(const std::string& session_id) const;
    void close();

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Json view_with_done(const Json& view, const Json& done);
std::string default_memory_path();

}  // namespace wsi
