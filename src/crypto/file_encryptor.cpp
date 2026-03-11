#include "crypto/file_encryptor.hpp"

#include <spdlog/spdlog.h>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace ircord::crypto {

FileEncryptor::FileEncryptor(const std::string& master_key_hex) {
    auto key_bytes = hex_to_bytes(master_key_hex);
    if (!key_bytes || key_bytes->size() != MASTER_KEY_SIZE) {
        spdlog::error("FileEncryptor: Invalid master key format. Expected {} hex chars.", 
            MASTER_KEY_SIZE * 2);
        return;
    }
    master_key_ = std::move(*key_bytes);
    spdlog::info("FileEncryptor: Initialized with master key");
}

std::vector<uint8_t> FileEncryptor::generate_master_key() {
    std::vector<uint8_t> key(MASTER_KEY_SIZE);
    randombytes_buf(key.data(), key.size());
    return key;
}

std::string FileEncryptor::bytes_to_hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

std::optional<std::vector<uint8_t>> FileEncryptor::hex_to_bytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        return std::nullopt;
    }
    
    std::vector<uint8_t> result;
    result.reserve(hex.size() / 2);
    
    for (size_t i = 0; i < hex.size(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        try {
            int byte = std::stoi(byte_str, nullptr, 16);
            result.push_back(static_cast<uint8_t>(byte));
        } catch (...) {
            return std::nullopt;
        }
    }
    
    return result;
}

std::optional<std::vector<uint8_t>> FileEncryptor::encrypt(
    const std::vector<uint8_t>& plaintext,
    EncryptedKey& out_encrypted_key) {
    
    if (!is_ready()) {
        spdlog::error("FileEncryptor: Not initialized with master key");
        return std::nullopt;
    }
    
    // Generate random DEK
    std::vector<uint8_t> dek(DEK_SIZE);
    randombytes_buf(dek.data(), dek.size());
    
    // Encrypt the DEK with master key
    auto encrypted_dek = encrypt_dek(dek);
    if (!encrypted_dek) {
        return std::nullopt;
    }
    out_encrypted_key = std::move(*encrypted_dek);
    
    // Generate salt and IV for file encryption
    EncryptedHeader header;
    randombytes_buf(header.salt.data(), header.salt.size());
    randombytes_buf(header.iv.data(), header.iv.size());
    header.original_size = plaintext.size();
    
    // Derive key for file encryption
    auto file_key = derive_key(dek, header.salt);
    if (file_key.empty()) {
        return std::nullopt;
    }
    
    // Encrypt file data using AES-256-GCM via libsodium's crypto_aead
    std::vector<uint8_t> ciphertext(plaintext.size());
    unsigned long long ciphertext_len;
    
    // Note: libsodium uses crypto_aead_aes256gcm on systems with AES-NI
    // or falls back to other authenticated encryption
    // For portable code, we use crypto_secretbox_easy which uses XSalsa20+Poly1305
    // But for actual AES-GCM, we'd need OpenSSL. Let's use libsodium's secretbox.
    
    // Actually, let's implement proper AES-GCM using OpenSSL EVP API
    // This is a simplified placeholder - in production use OpenSSL EVP
    
    // For now, use libsodium's crypto_secretbox (XSalsa20+Poly1305)
    // which provides authenticated encryption
    std::vector<uint8_t> nonce(crypto_secretbox_NONCEBYTES);
    randombytes_buf(nonce.data(), nonce.size());
    
    std::vector<uint8_t> encrypted(plaintext.size() + crypto_secretbox_MACBYTES);
    crypto_secretbox_easy(encrypted.data(), plaintext.data(), plaintext.size(),
        nonce.data(), file_key.data());
    
    // Copy nonce to header (we'll use first 12 bytes as IV, rest for internal use)
    std::memcpy(header.iv.data(), nonce.data(), std::min(header.iv.size(), nonce.size()));
    
    // Copy MAC to header tag
    std::memcpy(header.tag.data(), encrypted.data(), header.tag.size());
    
    header.encrypted_size = encrypted.size() - crypto_secretbox_MACBYTES;
    
    // Build output: header + encrypted data (skip MAC since it's in header)
    std::vector<uint8_t> result;
    result.reserve(sizeof(header) + encrypted.size() - crypto_secretbox_MACBYTES);
    
    // Serialize header
    result.insert(result.end(), header.salt.begin(), header.salt.end());
    result.insert(result.end(), header.iv.begin(), header.iv.end());
    result.insert(result.end(), header.tag.begin(), header.tag.end());
    
    // Add size fields
    uint8_t size_buf[16];
    std::memcpy(size_buf, &header.original_size, 8);
    std::memcpy(size_buf + 8, &header.encrypted_size, 8);
    result.insert(result.end(), size_buf, size_buf + 16);
    
    // Add encrypted data (after MAC)
    result.insert(result.end(), 
        encrypted.begin() + crypto_secretbox_MACBYTES, 
        encrypted.end());
    
    return result;
}

std::optional<std::vector<uint8_t>> FileEncryptor::decrypt(
    const std::vector<uint8_t>& ciphertext_with_header,
    const EncryptedKey& encrypted_key) {
    
    if (!is_ready()) {
        spdlog::error("FileEncryptor: Not initialized with master key");
        return std::nullopt;
    }
    
    // Minimum size check
    const size_t header_size = SALT_SIZE + IV_SIZE + TAG_SIZE + 16;
    if (ciphertext_with_header.size() < header_size) {
        spdlog::error("FileEncryptor: Input too small for header");
        return std::nullopt;
    }
    
    // Parse header
    EncryptedHeader header;
    size_t pos = 0;
    
    std::memcpy(header.salt.data(), ciphertext_with_header.data() + pos, SALT_SIZE);
    pos += SALT_SIZE;
    
    std::memcpy(header.iv.data(), ciphertext_with_header.data() + pos, IV_SIZE);
    pos += IV_SIZE;
    
    std::memcpy(header.tag.data(), ciphertext_with_header.data() + pos, TAG_SIZE);
    pos += TAG_SIZE;
    
    std::memcpy(&header.original_size, ciphertext_with_header.data() + pos, 8);
    pos += 8;
    std::memcpy(&header.encrypted_size, ciphertext_with_header.data() + pos, 8);
    pos += 8;
    
    // Decrypt DEK
    auto dek = decrypt_dek(encrypted_key);
    if (!dek) {
        return std::nullopt;
    }
    
    // Derive file key
    auto file_key = derive_key(*dek, header.salt);
    if (file_key.empty()) {
        return std::nullopt;
    }
    
    // Reconstruct full ciphertext with MAC
    std::vector<uint8_t> full_ciphertext(crypto_secretbox_MACBYTES + 
        ciphertext_with_header.size() - header_size);
    std::memcpy(full_ciphertext.data(), header.tag.data(), crypto_secretbox_MACBYTES);
    std::memcpy(full_ciphertext.data() + crypto_secretbox_MACBYTES,
        ciphertext_with_header.data() + header_size,
        ciphertext_with_header.size() - header_size);
    
    // Decrypt
    std::vector<uint8_t> plaintext(header.original_size);
    
    // Reconstruct nonce
    std::vector<uint8_t> nonce(crypto_secretbox_NONCEBYTES);
    std::memcpy(nonce.data(), header.iv.data(), std::min(header.iv.size(), nonce.size()));
    
    if (crypto_secretbox_open_easy(plaintext.data(), full_ciphertext.data(),
            full_ciphertext.size(), nonce.data(), file_key.data()) != 0) {
        spdlog::error("FileEncryptor: Decryption failed (authentication error)");
        return std::nullopt;
    }
    
    return plaintext;
}

std::optional<FileEncryptor::EncryptedKey> FileEncryptor::encrypt_dek(
    const std::vector<uint8_t>& dek) {
    
    EncryptedKey result;
    randombytes_buf(result.iv.data(), result.iv.size());
    randombytes_buf(result.tag.data(), result.tag.size());
    
    // Generate ephemeral salt
    std::array<uint8_t, SALT_SIZE> salt;
    randombytes_buf(salt.data(), salt.size());
    
    // Derive KEK from master key
    auto kek = derive_key(master_key_, salt);
    if (kek.empty()) {
        return std::nullopt;
    }
    
    // Encrypt DEK
    result.ciphertext.resize(dek.size() + crypto_secretbox_MACBYTES);
    
    // Use tag as part of nonce for simplicity
    std::vector<uint8_t> nonce(crypto_secretbox_NONCEBYTES);
    std::memcpy(nonce.data(), result.iv.data(), std::min(result.iv.size(), nonce.size()));
    
    crypto_secretbox_easy(result.ciphertext.data(), dek.data(), dek.size(),
        nonce.data(), kek.data());
    
    // Store salt at beginning of ciphertext for decryption
    result.ciphertext.insert(result.ciphertext.begin(), salt.begin(), salt.end());
    
    return result;
}

std::optional<std::vector<uint8_t>> FileEncryptor::decrypt_dek(
    const EncryptedKey& encrypted_key) {
    
    if (encrypted_key.ciphertext.size() < SALT_SIZE + DEK_SIZE + crypto_secretbox_MACBYTES) {
        spdlog::error("FileEncryptor: Encrypted DEK too small");
        return std::nullopt;
    }
    
    // Extract salt
    std::array<uint8_t, SALT_SIZE> salt;
    std::memcpy(salt.data(), encrypted_key.ciphertext.data(), SALT_SIZE);
    
    // Derive KEK
    auto kek = derive_key(master_key_, salt);
    if (kek.empty()) {
        return std::nullopt;
    }
    
    // Decrypt DEK
    std::vector<uint8_t> dek(DEK_SIZE);
    
    std::vector<uint8_t> nonce(crypto_secretbox_NONCEBYTES);
    std::memcpy(nonce.data(), encrypted_key.iv.data(), std::min(encrypted_key.iv.size(), nonce.size()));
    
    if (crypto_secretbox_open_easy(dek.data(),
            encrypted_key.ciphertext.data() + SALT_SIZE,
            encrypted_key.ciphertext.size() - SALT_SIZE,
            nonce.data(), kek.data()) != 0) {
        spdlog::error("FileEncryptor: DEK decryption failed");
        return std::nullopt;
    }
    
    return dek;
}

std::vector<uint8_t> FileEncryptor::serialize_encrypted_key(const EncryptedKey& key) {
    std::vector<uint8_t> result;
    result.reserve(IV_SIZE + TAG_SIZE + 4 + key.ciphertext.size());
    
    result.insert(result.end(), key.iv.begin(), key.iv.end());
    result.insert(result.end(), key.tag.begin(), key.tag.end());
    
    // Add ciphertext length
    uint32_t len = static_cast<uint32_t>(key.ciphertext.size());
    uint8_t len_buf[4];
    std::memcpy(len_buf, &len, 4);
    result.insert(result.end(), len_buf, len_buf + 4);
    
    result.insert(result.end(), key.ciphertext.begin(), key.ciphertext.end());
    
    return result;
}

std::optional<FileEncryptor::EncryptedKey> FileEncryptor::deserialize_encrypted_key(
    const std::vector<uint8_t>& data) {
    
    const size_t min_size = IV_SIZE + TAG_SIZE + 4;
    if (data.size() < min_size) {
        return std::nullopt;
    }
    
    EncryptedKey result;
    size_t pos = 0;
    
    std::memcpy(result.iv.data(), data.data() + pos, IV_SIZE);
    pos += IV_SIZE;
    
    std::memcpy(result.tag.data(), data.data() + pos, TAG_SIZE);
    pos += TAG_SIZE;
    
    uint32_t len;
    std::memcpy(&len, data.data() + pos, 4);
    pos += 4;
    
    if (data.size() < pos + len) {
        return std::nullopt;
    }
    
    result.ciphertext.assign(data.begin() + pos, data.begin() + pos + len);
    
    return result;
}

std::vector<uint8_t> FileEncryptor::derive_key(
    const std::vector<uint8_t>& master_key,
    const std::array<uint8_t, SALT_SIZE>& salt) {
    
    std::vector<uint8_t> result(crypto_secretbox_KEYBYTES);
    
    // Use Argon2id via libsodium's crypto_pwhash
    // This is intentionally slow to deter brute force
    if (crypto_pwhash(result.data(), result.size(),
            reinterpret_cast<const char*>(master_key.data()), master_key.size(),
            salt.data(),
            crypto_pwhash_OPSLIMIT_INTERACTIVE,
            crypto_pwhash_MEMLIMIT_INTERACTIVE,
            crypto_pwhash_ALG_ARGON2ID13) != 0) {
        spdlog::error("FileEncryptor: Key derivation failed");
        return {};
    }
    
    return result;
}

void FileEncryptor::random_bytes(void* buf, size_t len) {
    randombytes_buf(buf, len);
}

} // namespace ircord::crypto
