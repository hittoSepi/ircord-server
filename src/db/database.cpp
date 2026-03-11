#include "database.hpp"

#include <spdlog/spdlog.h>

namespace ircord::db {

Database::Database(const std::string& path)
    : db_(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE)
{
    db_.exec("PRAGMA journal_mode=WAL");
    db_.exec("PRAGMA foreign_keys=ON");
    execute_schema();
    spdlog::info("Database opened: {}", path);
}

void Database::execute_schema() {
    db_.exec(
        "CREATE TABLE IF NOT EXISTS users ("
        "  user_id       TEXT PRIMARY KEY,"
        "  identity_pub  BLOB NOT NULL,"
        "  password_hash BLOB,"           // Argon2id hash (NULL if password auth not used)
        "  created_at    INTEGER NOT NULL"
        ")"
    );

    db_.exec(
        "CREATE TABLE IF NOT EXISTS signed_prekeys ("
        "  user_id     TEXT NOT NULL REFERENCES users(user_id),"
        "  spk_pub     BLOB NOT NULL,"
        "  spk_sig     BLOB NOT NULL,"
        "  uploaded_at INTEGER NOT NULL,"
        "  PRIMARY KEY (user_id)"
        ")"
    );

    db_.exec(
        "CREATE TABLE IF NOT EXISTS one_time_prekeys ("
        "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  user_id  TEXT NOT NULL REFERENCES users(user_id),"
        "  opk_pub  BLOB NOT NULL,"
        "  used     INTEGER NOT NULL DEFAULT 0"
        ")"
    );

    db_.exec(
        "CREATE TABLE IF NOT EXISTS offline_messages ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  recipient_id TEXT NOT NULL,"
        "  payload      BLOB NOT NULL,"
        "  stored_at    INTEGER NOT NULL,"
        "  expires_at   INTEGER NOT NULL"
        ")"
    );

    spdlog::debug("Database schema initialized");
}

} // namespace ircord::db
