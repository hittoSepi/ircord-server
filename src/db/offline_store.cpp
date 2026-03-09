#include "offline_store.hpp"

#include <spdlog/spdlog.h>

#include <chrono>

namespace ircord::db {

namespace {
int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // anonymous namespace

OfflineStore::OfflineStore(Database& db)
    : db_(db)
{}

void OfflineStore::save(const std::string& recipient_id,
                         const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    const int64_t now     = now_unix();
    const int64_t expires = now + kTtlSeconds;

    SQLite::Statement q(db_.get(),
        "INSERT INTO offline_messages (recipient_id, payload, stored_at, expires_at)"
        " VALUES (?, ?, ?, ?)");
    q.bind(1, recipient_id);
    q.bind(2, payload.data(), static_cast<int>(payload.size()));
    q.bind(3, now);
    q.bind(4, expires);
    q.exec();

    spdlog::debug("OfflineStore: saved message for {}", recipient_id);
}

std::vector<std::vector<uint8_t>> OfflineStore::fetch_and_delete(
    const std::string& recipient_id)
{
    std::lock_guard<std::mutex> lock(db_.mutex());
    const int64_t now = now_unix();

    std::vector<std::vector<uint8_t>> messages;

    try {
        SQLite::Statement q(db_.get(),
            "SELECT id, payload FROM offline_messages"
            " WHERE recipient_id = ? AND expires_at > ?"
            " ORDER BY id ASC");
        q.bind(1, recipient_id);
        q.bind(2, now);

        while (q.executeStep()) {
            auto blob = q.getColumn(1);
            const uint8_t* data = reinterpret_cast<const uint8_t*>(blob.getBlob());
            messages.emplace_back(data, data + blob.getBytes());
        }

        // Delete fetched (and expired) messages
        SQLite::Statement del(db_.get(),
            "DELETE FROM offline_messages WHERE recipient_id = ?");
        del.bind(1, recipient_id);
        del.exec();

        if (!messages.empty()) {
            spdlog::debug("OfflineStore: delivered {} offline messages for {}",
                messages.size(), recipient_id);
        }
    } catch (const SQLite::Exception& e) {
        spdlog::error("OfflineStore::fetch_and_delete failed: {}", e.what());
    }

    return messages;
}

} // namespace ircord::db
