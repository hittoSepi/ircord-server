#pragma once

#include <string>
#include <vector>
#include <array>
#include <optional>
#include <sodium.h>

namespace ircord::crypto {

/**
 * File encryption at rest using AES-256-GCM via libsodium.
 * Each file gets a unique data encryption key (DEK) which is encrypted
 * with the master key and stored alongside the file metadata.
 */
class FileEncryptor {
public:
    // Key sizes
    static constexpr size_t MASTER_KEY_SIZE = 32;      // 256 bits
    static constexpr size_t DEK_SIZE = 32;             // 256 bits
    static constexpr size_t IV_SIZE = 12;              // 96 bits for GCM
    static constexpr size_t TAG_SIZE = 16;             // 128 bits for GCM
    static constexpr size_t SALT_SIZE = 32;            // 256 bits

    // Encrypted file header
    struct EncryptedHeader {
        std::array<uint8_t, SALT_SIZE> salt;           // Salt for key derivation
        std::array<uint8_t, IV_SIZE> iv;               // Initialization vector
        std::array<uint8_t, TAG_SIZE> tag;             // GCM authentication tag
        uint64_t original_size = 0;                     // Original file size
        uint64_t encrypted_size = 0;                    // Encrypted data size
    };

    // Key encapsulation
    struct EncryptedKey {
        std::vector<uint8_t> ciphertext;                // Encrypted DEK
        std::array<uint8_t, IV_SIZE> iv;               // IV for DEK encryption
        std::array<uint8_t, TAG_SIZE> tag;             // Auth tag for DEK
    };

    /**
     * Initialize with master key from environment or config.
     * Key should be 32 bytes (64 hex chars).
     */
    explicit FileEncryptor(const std::string& master_key_hex);
    
    /**
     * Generate a random master key.
     */
    static std::vector<uint8_t> generate_master_key();
    
    /**
     * Convert bytes to hex string.
     */
    static std::string bytes_to_hex(const std::vector<uint8_t>& bytes);
    
    /**
     * Convert hex string to bytes.
     */
    static std::optional<std::vector<uint8_t>> hex_to_bytes(const std::string& hex);

    /**
     * Encrypt file data.
     * Returns encrypted header + encrypted data.
     */
    std::optional<std::vector<uint8_t>> encrypt(
        const std::vector<uint8_t>& plaintext,
        EncryptedKey& out_encrypted_key);

    /**
     * Decrypt file data.
     * Requires the encrypted key from encryption.
     */
    std::optional<std::vector<uint8_t>> decrypt(
        const std::vector<uint8_t>& ciphertext_with_header,
        const EncryptedKey& encrypted_key);

    /**
     * Encrypt a data encryption key (DEK) with the master key.
     */
    std::optional<EncryptedKey> encrypt_dek(const std::vector<uint8_t>& dek);

    /**
     * Decrypt a data encryption key (DEK).
     */
    std::optional<std::vector<uint8_t>> decrypt_dek(const EncryptedKey& encrypted_key);

    /**
     * Serialize encrypted key to bytes for database storage.
     */
    static std::vector<uint8_t> serialize_encrypted_key(const EncryptedKey& key);

    /**
     * Deserialize encrypted key from bytes.
     */
    static std::optional<EncryptedKey> deserialize_encrypted_key(const std::vector<uint8_t>& data);

    /**
     * Check if encryption is available (master key loaded successfully).
     */
    bool is_ready() const { return !master_key_.empty(); }

private:
    std::vector<uint8_t> master_key_;
    
    // Derive encryption key from master key + salt
    std::vector<uint8_t> derive_key(
        const std::vector<uint8_t>& master_key,
        const std::array<uint8_t, SALT_SIZE>& salt);
    
    // Generate random bytes
    static void random_bytes(void* buf, size_t len);
};

} // namespace ircord::crypto
