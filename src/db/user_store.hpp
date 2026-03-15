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
    
    // Password authentication
    /**
     * Set password for a user (hashed with Argon2id).
     * @return true if successful
     */
    bool set_password(const std::string& user_id, const std::string& password);
    
    /**
     * Verify password for a user.
     * @return true if password is correct
     */
    bool verify_password(const std::string& user_id, const std::string& password);
    
    /**
     * Update password (requires old password verification).
     * @return true if old password was correct and new password was set
     */
    bool update_password(const std::string& user_id, 
                         const std::string& old_password,
                         const std::string& new_password);
    
    /**
     * Check if user has a password set.
     */
    bool has_password(const std::string& user_id);

    /**
     * Update identity public key for a user (used for key recovery).
     */
    void update_identity_key(const std::string& user_id,
                              const std::vector<uint8_t>& new_identity_pub);

    void clear_key_material(const std::string& user_id);

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
