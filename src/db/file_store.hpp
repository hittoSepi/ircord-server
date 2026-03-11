#pragma once

#include "database.hpp"

#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace ircord::db {

/**
 * File metadata for stored uploads.
 */
struct FileMetadata {
    std::string file_id;
    std::string filename;
    uint64_t file_size = 0;
    std::string mime_type;
    std::string sender_id;
    std::string recipient_id;  // Empty for channel uploads
    std::string channel_id;    // Empty for DMs
    int64_t uploaded_at = 0;   // Unix timestamp
    int64_t expires_at = 0;    // Expiration timestamp
    std::vector<uint8_t> file_checksum;  // SHA-256
    std::string storage_path;  // Filesystem path
    bool is_complete = false;  // All chunks received
};

/**
 * File chunk information.
 */
struct FileChunkInfo {
    std::string file_id;
    uint32_t chunk_index = 0;
    uint32_t chunk_size = 0;
    std::vector<uint8_t> checksum;  // Chunk SHA-256
};

/**
 * Store for managing file uploads and metadata.
 */
class FileStore {
public:
    explicit FileStore(Database& db);

    // ============================================================================
    // File Metadata
    // ============================================================================

    /**
     * Create a new file record.
     * @return true if created successfully
     */
    bool createFile(const FileMetadata& metadata);

    /**
     * Get file metadata by ID.
     * @return Metadata if found
     */
    std::optional<FileMetadata> getFile(const std::string& file_id);

    /**
     * Update file as complete (all chunks received).
     */
    bool markComplete(const std::string& file_id, const std::vector<uint8_t>& checksum);

    /**
     * Delete a file record and its chunks.
     */
    bool deleteFile(const std::string& file_id);

    /**
     * List files for a recipient (DM or channel).
     */
    std::vector<FileMetadata> listFiles(
        const std::string& recipient_id = "",
        const std::string& channel_id = "",
        int limit = 100);

    // ============================================================================
    // Chunk Management
    // ============================================================================

    /**
     * Store a file chunk.
     * @return true if stored successfully
     */
    bool storeChunk(
        const std::string& file_id,
        uint32_t chunk_index,
        const std::vector<uint8_t>& data);

    /**
     * Get a file chunk.
     * @return Chunk data if found
     */
    std::optional<std::vector<uint8_t>> getChunk(
        const std::string& file_id,
        uint32_t chunk_index);

    /**
     * Check if a chunk exists.
     */
    bool hasChunk(const std::string& file_id, uint32_t chunk_index);

    /**
     * Get list of missing chunk indices for a file.
     */
    std::vector<uint32_t> getMissingChunks(
        const std::string& file_id,
        uint32_t total_chunks);

    /**
     * Get received bytes for a file.
     */
    uint64_t getReceivedBytes(const std::string& file_id);

    // ============================================================================
    // Cleanup
    // ============================================================================

    /**
     * Delete expired files and their chunks.
     * @return Number of files deleted
     */
    int cleanupExpired();

    /**
     * Get storage statistics.
     */
    struct Stats {
        int total_files = 0;
        uint64_t total_bytes = 0;
        int expired_files = 0;
        int incomplete_files = 0;
    };
    Stats getStats();

private:
    Database& db_;
    static constexpr int64_t kDefaultTtlSeconds = 7 * 24 * 3600;  // 7 days

    int64_t nowUnix() const {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

} // namespace ircord::db
