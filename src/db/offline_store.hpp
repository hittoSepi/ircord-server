#pragma once

#include "database.hpp"

#include <string>
#include <vector>

namespace ircord::db {

class OfflineStore {
public:
    explicit OfflineStore(Database& db);

    // Store serialized Envelope bytes for recipient (TTL = 7 days)
    // Returns false if queue limit reached for this user
    bool save(const std::string& recipient_id, const std::vector<uint8_t>& payload);

    // Fetch all pending messages for recipient and delete them from DB
    std::vector<std::vector<uint8_t>> fetch_and_delete(const std::string& recipient_id);

    // Delete all messages whose expires_at is in the past
    int cleanup_expired();

    // Get count of pending messages for a recipient
    int get_pending_count(const std::string& recipient_id);

    // Get total count of all offline messages
    int get_total_count();

    // Statistics
    struct Stats {
        int total_messages = 0;
        int total_recipients = 0;
        int64_t oldest_message_age_sec = 0;
    };
    Stats get_stats();

private:
    Database& db_;
    static constexpr int64_t kTtlSeconds = 7 * 24 * 3600;  // 7 days
    static constexpr int kMaxMessagesPerUser = 100;        // Per-user limit
    static constexpr int kMaxTotalMessages = 100000;       // Global limit
};

} // namespace ircord::db
