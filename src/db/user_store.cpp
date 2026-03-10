#include "user_store.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <stdexcept>

namespace ircord::db {

namespace {
int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // anonymous namespace

UserStore::UserStore(Database& db)
    : db_(db)
{}

std::optional<User> UserStore::find_by_id(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    try {
        SQLite::Statement q(db_.get(),
            "SELECT user_id, identity_pub, created_at FROM users WHERE user_id = ?");
        q.bind(1, user_id);
        if (q.executeStep()) {
            User u;
            u.user_id   = q.getColumn(0).getString();
            auto blob   = q.getColumn(1);
            const uint8_t* data = reinterpret_cast<const uint8_t*>(blob.getBlob());
            u.identity_pub.assign(data, data + blob.getBytes());
            u.created_at = q.getColumn(2).getInt64();
            return u;
        }
        return std::nullopt;
    } catch (const SQLite::Exception& e) {
        spdlog::error("UserStore::find_by_id failed: {}", e.what());
        return std::nullopt;
    }
}

void UserStore::insert(const User& user) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    SQLite::Statement q(db_.get(),
        "INSERT INTO users (user_id, identity_pub, created_at) VALUES (?, ?, ?)");
    q.bind(1, user.user_id);
    q.bind(2, user.identity_pub.data(), static_cast<int>(user.identity_pub.size()));
    q.bind(3, user.created_at);
    q.exec();
    spdlog::debug("UserStore: inserted user {}", user.user_id);
}

void UserStore::upsert_signed_prekey(const std::string& user_id,
                                      const std::vector<uint8_t>& spk_pub,
                                      const std::vector<uint8_t>& spk_sig) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    SQLite::Statement q(db_.get(),
        "INSERT OR REPLACE INTO signed_prekeys (user_id, spk_pub, spk_sig, uploaded_at)"
        " VALUES (?, ?, ?, ?)");
    q.bind(1, user_id);
    q.bind(2, spk_pub.data(), static_cast<int>(spk_pub.size()));
    q.bind(3, spk_sig.data(), static_cast<int>(spk_sig.size()));
    q.bind(4, now_unix());
    q.exec();
    spdlog::debug("UserStore: upserted SPK for user {}", user_id);
}

std::optional<SignedPrekey> UserStore::get_signed_prekey(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    try {
        SQLite::Statement q(db_.get(),
            "SELECT spk_pub, spk_sig FROM signed_prekeys WHERE user_id = ?");
        q.bind(1, user_id);
        if (q.executeStep()) {
            SignedPrekey spk;
            auto pub = q.getColumn(0);
            auto sig = q.getColumn(1);
            const uint8_t* pub_data = reinterpret_cast<const uint8_t*>(pub.getBlob());
            const uint8_t* sig_data = reinterpret_cast<const uint8_t*>(sig.getBlob());
            spk.spk_pub.assign(pub_data, pub_data + pub.getBytes());
            spk.spk_sig.assign(sig_data, sig_data + sig.getBytes());
            return spk;
        }
        return std::nullopt;
    } catch (const SQLite::Exception& e) {
        spdlog::error("UserStore::get_signed_prekey failed: {}", e.what());
        return std::nullopt;
    }
}

void UserStore::store_opk(const std::string& user_id, const std::vector<uint8_t>& opk_pub) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    SQLite::Statement q(db_.get(),
        "INSERT INTO one_time_prekeys (user_id, opk_pub, used) VALUES (?, ?, 0)");
    q.bind(1, user_id);
    q.bind(2, opk_pub.data(), static_cast<int>(opk_pub.size()));
    q.exec();
}

std::vector<uint8_t> UserStore::consume_opk(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    try {
        SQLite::Statement q(db_.get(),
            "SELECT id, opk_pub FROM one_time_prekeys"
            " WHERE user_id = ? AND used = 0 LIMIT 1");
        q.bind(1, user_id);
        if (!q.executeStep()) {
            return {};
        }
        int64_t id = q.getColumn(0).getInt64();
        auto blob  = q.getColumn(1);
        const uint8_t* data = reinterpret_cast<const uint8_t*>(blob.getBlob());
        std::vector<uint8_t> opk(data, data + blob.getBytes());

        SQLite::Statement upd(db_.get(),
            "UPDATE one_time_prekeys SET used = 1 WHERE id = ?");
        upd.bind(1, static_cast<int64_t>(id));
        upd.exec();

        return opk;
    } catch (const SQLite::Exception& e) {
        spdlog::error("UserStore::consume_opk failed: {}", e.what());
        return {};
    }
}

} // namespace ircord::db
