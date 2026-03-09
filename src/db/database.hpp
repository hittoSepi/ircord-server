#pragma once

#include <SQLiteCpp/SQLiteCpp.h>
#include <mutex>
#include <string>

namespace ircord::db {

class Database {
public:
    explicit Database(const std::string& path);

    SQLite::Database& get() { return db_; }
    std::mutex& mutex() { return mutex_; }

private:
    void execute_schema();

    SQLite::Database db_;
    std::mutex mutex_;
};

} // namespace ircord::db
