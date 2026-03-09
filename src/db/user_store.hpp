#pragma once

#include "database.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ircord::db {

struct User {
    std::string user_id;
    std::vector<uint8_t> identity_pub;  // Ed25519 32 bytes
    int64_t created_at = 0;
};

struct SignedPrekey {
    std::vector<uint8_t> spk_pub;
    std::vector<uint8_t> spk_sig;
};

class UserStore {
public:
    explicit UserStore(Database& db);

    std::optional<User> find_by_id(const std::string& user_id);
    void insert(const User& user);

    void upsert_signed_prekey(const std::string& user_id,
                               const std::vector<uint8_t>& spk_pub,
                               const std::vector<uint8_t>& spk_sig);

    std::optional<SignedPrekey> get_signed_prekey(const std::string& user_id);

    void store_opk(const std::string& user_id, const std::vector<uint8_t>& opk_pub);

    // Returns one unused OPK and marks it used; returns empty if none available
    std::vector<uint8_t> consume_opk(const std::string& user_id);

private:
    Database& db_;
};

} // namespace ircord::db
