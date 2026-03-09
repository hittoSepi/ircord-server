#pragma once

#include "database.hpp"

#include <string>
#include <vector>

namespace ircord::db {

class OfflineStore {
public:
    explicit OfflineStore(Database& db);

    // Store serialized Envelope bytes for recipient (TTL = 7 days)
    void save(const std::string& recipient_id, const std::vector<uint8_t>& payload);

    // Fetch all pending messages for recipient and delete them from DB
    std::vector<std::vector<uint8_t>> fetch_and_delete(const std::string& recipient_id);

    // Delete all messages whose expires_at is in the past
    int cleanup_expired();

private:
    Database& db_;
    static constexpr int64_t kTtlSeconds = 7 * 24 * 3600;
};

} // namespace ircord::db
