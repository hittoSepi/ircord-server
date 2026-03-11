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

bool OfflineStore::save(const std::string& recipient_id,
                         const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    
    // Check per-user limit
    try {
        SQLite::Statement count_q(db_.get(),
            "SELECT COUNT(*) FROM offline_messages WHERE recipient_id = ?");
        count_q.bind(1, recipient_id);
        count_q.executeStep();
        int user_count = count_q.getColumn(0).getInt();
        
        if (user_count >= kMaxMessagesPerUser) {
            spdlog::warn("OfflineStore: per-user limit reached for {}", recipient_id);
            return false;
        }
    } catch (const SQLite::Exception& e) {
        spdlog::error("OfflineStore: failed to check user count: {}", e.what());
    }
    
    // Check global limit (if reached, cleanup oldest messages)
    try {
        SQLite::Statement total_q(db_.get(),
            "SELECT COUNT(*) FROM offline_messages");
        total_q.executeStep();
        int total_count = total_q.getColumn(0).getInt();
        
        if (total_count >= kMaxTotalMessages) {
            spdlog::warn("OfflineStore: global limit reached ({}), dropping oldest message", 
                total_count);
            // Delete oldest message globally
            SQLite::Statement del_oldest(db_.get(),
                "DELETE FROM offline_messages WHERE id = (SELECT MIN(id) FROM offline_messages)");
            del_oldest.exec();
        }
    } catch (const SQLite::Exception& e) {
        spdlog::error("OfflineStore: failed to check total count: {}", e.what());
    }
    
    const int64_t now     = now_unix();
    const int64_t expires = now + kTtlSeconds;

    try {
        SQLite::Statement q(db_.get(),
            "INSERT INTO offline_messages (recipient_id, payload, stored_at, expires_at)"
            " VALUES (?, ?, ?, ?)");
        q.bind(1, recipient_id);
        q.bind(2, payload.data(), static_cast<int>(payload.size()));
        q.bind(3, now);
        q.bind(4, expires);
        q.exec();

        spdlog::debug("OfflineStore: saved message for {}", recipient_id);
        return true;
    } catch (const SQLite::Exception& e) {
        spdlog::error("OfflineStore::save failed: {}", e.what());
        return false;
    }
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

int OfflineStore::cleanup_expired() {
    std::lock_guard<std::mutex> lock(db_.mutex());
    const int64_t now = now_unix();
    try {
        SQLite::Statement del(db_.get(),
            "DELETE FROM offline_messages WHERE expires_at <= ?");
        del.bind(1, now);
        del.exec();
        int removed = db_.get().getChanges();
        if (removed > 0) {
            spdlog::info("OfflineStore: cleaned up {} expired messages", removed);
        }
        return removed;
    } catch (const SQLite::Exception& e) {
        spdlog::error("OfflineStore::cleanup_expired failed: {}", e.what());
        return 0;
    }
}

int OfflineStore::get_pending_count(const std::string& recipient_id) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    try {
        SQLite::Statement q(db_.get(),
            "SELECT COUNT(*) FROM offline_messages WHERE recipient_id = ? AND expires_at > ?");
        q.bind(1, recipient_id);
        q.bind(2, now_unix());
        q.executeStep();
        return q.getColumn(0).getInt();
    } catch (const SQLite::Exception& e) {
        spdlog::error("OfflineStore::get_pending_count failed: {}", e.what());
        return 0;
    }
}

int OfflineStore::get_total_count() {
    std::lock_guard<std::mutex> lock(db_.mutex());
    try {
        SQLite::Statement q(db_.get(),
            "SELECT COUNT(*) FROM offline_messages");
        q.executeStep();
        return q.getColumn(0).getInt();
    } catch (const SQLite::Exception& e) {
        spdlog::error("OfflineStore::get_total_count failed: {}", e.what());
        return 0;
    }
}

OfflineStore::Stats OfflineStore::get_stats() {
    std::lock_guard<std::mutex> lock(db_.mutex());
    Stats stats;
    
    try {
        // Total messages
        SQLite::Statement total_q(db_.get(),
            "SELECT COUNT(*) FROM offline_messages");
        total_q.executeStep();
        stats.total_messages = total_q.getColumn(0).getInt();
        
        // Unique recipients
        SQLite::Statement recipients_q(db_.get(),
            "SELECT COUNT(DISTINCT recipient_id) FROM offline_messages");
        recipients_q.executeStep();
        stats.total_recipients = recipients_q.getColumn(0).getInt();
        
        // Oldest message age
        SQLite::Statement oldest_q(db_.get(),
            "SELECT MIN(stored_at) FROM offline_messages");
        if (oldest_q.executeStep() && !oldest_q.getColumn(0).isNull()) {
            int64_t oldest = oldest_q.getColumn(0).getInt64();
            stats.oldest_message_age_sec = now_unix() - oldest;
        }
    } catch (const SQLite::Exception& e) {
        spdlog::error("OfflineStore::get_stats failed: {}", e.what());
    }
    
    return stats;
}

} // namespace ircord::db
