#include "file_store.hpp"
#include <spdlog/spdlog.h>

namespace ircord::db {

FileStore::FileStore(Database& db)
    : db_(db)
{
    // Create tables if not exist
    std::lock_guard<std::mutex> lock(db_.mutex());
    
    // Files table
    db_.get().exec(R"(
        CREATE TABLE IF NOT EXISTS files (
            file_id TEXT PRIMARY KEY,
            filename TEXT NOT NULL,
            file_size INTEGER NOT NULL,
            mime_type TEXT,
            sender_id TEXT NOT NULL,
            recipient_id TEXT,
            channel_id TEXT,
            uploaded_at INTEGER NOT NULL,
            expires_at INTEGER NOT NULL,
            file_checksum BLOB,
            storage_path TEXT NOT NULL,
            is_complete INTEGER NOT NULL DEFAULT 0
        )
    )");

    // Chunks table
    db_.get().exec(R"(
        CREATE TABLE IF NOT EXISTS file_chunks (
            file_id TEXT NOT NULL,
            chunk_index INTEGER NOT NULL,
            chunk_data BLOB NOT NULL,
            chunk_size INTEGER NOT NULL,
            PRIMARY KEY (file_id, chunk_index),
            FOREIGN KEY (file_id) REFERENCES files(file_id) ON DELETE CASCADE
        )
    )");

    // Indexes
    db_.get().exec("CREATE INDEX IF NOT EXISTS idx_files_recipient ON files(recipient_id)");
    db_.get().exec("CREATE INDEX IF NOT EXISTS idx_files_channel ON files(channel_id)");
    db_.get().exec("CREATE INDEX IF NOT EXISTS idx_files_expires ON files(expires_at)");
    
    spdlog::info("FileStore initialized");
}

bool FileStore::createFile(const FileMetadata& metadata) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    
    try {
        SQLite::Statement insert(db_.get(), R"(
            INSERT INTO files (file_id, filename, file_size, mime_type, sender_id,
                             recipient_id, channel_id, uploaded_at, expires_at,
                             storage_path, is_complete)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )");
        
        int64_t expires = metadata.expires_at > 0 ? metadata.expires_at : nowUnix() + kDefaultTtlSeconds;
        
        insert.bind(1, metadata.file_id);
        insert.bind(2, metadata.filename);
        insert.bind(3, static_cast<int64_t>(metadata.file_size));
        insert.bind(4, metadata.mime_type);
        insert.bind(5, metadata.sender_id);
        insert.bind(6, metadata.recipient_id);
        insert.bind(7, metadata.channel_id);
        insert.bind(8, metadata.uploaded_at > 0 ? metadata.uploaded_at : nowUnix());
        insert.bind(9, expires);
        insert.bind(10, metadata.storage_path);
        insert.bind(11, metadata.is_complete ? 1 : 0);
        
        insert.exec();
        
        spdlog::debug("FileStore: created file record {}", metadata.file_id);
        return true;
    } catch (const SQLite::Exception& e) {
        spdlog::error("FileStore::createFile failed: {}", e.what());
        return false;
    }
}

std::optional<FileMetadata> FileStore::getFile(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    
    try {
        SQLite::Statement query(db_.get(), 
            "SELECT * FROM files WHERE file_id = ?");
        query.bind(1, file_id);
        
        if (query.executeStep()) {
            FileMetadata meta;
            meta.file_id = query.getColumn(0).getString();
            meta.filename = query.getColumn(1).getString();
            meta.file_size = static_cast<uint64_t>(query.getColumn(2).getInt64());
            meta.mime_type = query.getColumn(3).getString();
            meta.sender_id = query.getColumn(4).getString();
            meta.recipient_id = query.getColumn(5).getString();
            meta.channel_id = query.getColumn(6).getString();
            meta.uploaded_at = query.getColumn(7).getInt64();
            meta.expires_at = query.getColumn(8).getInt64();
            
            auto checksum = query.getColumn(9);
            if (!checksum.isNull()) {
                const uint8_t* data = reinterpret_cast<const uint8_t*>(checksum.getBlob());
                meta.file_checksum.assign(data, data + checksum.getBytes());
            }
            
            meta.storage_path = query.getColumn(10).getString();
            meta.is_complete = query.getColumn(11).getInt() != 0;
            
            return meta;
        }
    } catch (const SQLite::Exception& e) {
        spdlog::error("FileStore::getFile failed: {}", e.what());
    }
    
    return std::nullopt;
}

bool FileStore::markComplete(const std::string& file_id, const std::vector<uint8_t>& checksum) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    
    try {
        SQLite::Statement update(db_.get(), R"(
            UPDATE files SET is_complete = 1, file_checksum = ?
            WHERE file_id = ?
        )");
        
        update.bind(1, checksum.data(), static_cast<int>(checksum.size()));
        update.bind(2, file_id);
        
        update.exec();
        
        spdlog::debug("FileStore: marked file {} as complete", file_id);
        return true;
    } catch (const SQLite::Exception& e) {
        spdlog::error("FileStore::markComplete failed: {}", e.what());
        return false;
    }
}

bool FileStore::deleteFile(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    
    try {
        SQLite::Statement del(db_.get(), "DELETE FROM files WHERE file_id = ?");
        del.bind(1, file_id);
        del.exec();
        
        spdlog::debug("FileStore: deleted file {}", file_id);
        return true;
    } catch (const SQLite::Exception& e) {
        spdlog::error("FileStore::deleteFile failed: {}", e.what());
        return false;
    }
}

std::vector<FileMetadata> FileStore::listFiles(
    const std::string& recipient_id,
    const std::string& channel_id,
    int limit)
{
    std::lock_guard<std::mutex> lock(db_.mutex());
    std::vector<FileMetadata> results;
    
    try {
        std::string sql = "SELECT * FROM files WHERE 1=1";
        if (!recipient_id.empty()) sql += " AND recipient_id = ?";
        if (!channel_id.empty()) sql += " AND channel_id = ?";
        sql += " AND expires_at > ? ORDER BY uploaded_at DESC LIMIT ?";
        
        SQLite::Statement query(db_.get(), sql);
        
        int param = 1;
        if (!recipient_id.empty()) query.bind(param++, recipient_id);
        if (!channel_id.empty()) query.bind(param++, channel_id);
        query.bind(param++, nowUnix());
        query.bind(param++, limit);
        
        while (query.executeStep()) {
            FileMetadata meta;
            meta.file_id = query.getColumn(0).getString();
            meta.filename = query.getColumn(1).getString();
            meta.file_size = static_cast<uint64_t>(query.getColumn(2).getInt64());
            meta.mime_type = query.getColumn(3).getString();
            meta.sender_id = query.getColumn(4).getString();
            meta.recipient_id = query.getColumn(5).getString();
            meta.channel_id = query.getColumn(6).getString();
            meta.uploaded_at = query.getColumn(7).getInt64();
            meta.expires_at = query.getColumn(8).getInt64();
            meta.is_complete = query.getColumn(11).getInt() != 0;
            results.push_back(std::move(meta));
        }
    } catch (const SQLite::Exception& e) {
        spdlog::error("FileStore::listFiles failed: {}", e.what());
    }
    
    return results;
}

bool FileStore::storeChunk(
    const std::string& file_id,
    uint32_t chunk_index,
    const std::vector<uint8_t>& data)
{
    std::lock_guard<std::mutex> lock(db_.mutex());
    
    try {
        SQLite::Statement insert(db_.get(), R"(
            INSERT OR REPLACE INTO file_chunks (file_id, chunk_index, chunk_data, chunk_size)
            VALUES (?, ?, ?, ?)
        )");
        
        insert.bind(1, file_id);
        insert.bind(2, static_cast<int>(chunk_index));
        insert.bind(3, data.data(), static_cast<int>(data.size()));
        insert.bind(4, static_cast<int>(data.size()));
        
        insert.exec();
        
        spdlog::debug("FileStore: stored chunk {} for file {}", chunk_index, file_id);
        return true;
    } catch (const SQLite::Exception& e) {
        spdlog::error("FileStore::storeChunk failed: {}", e.what());
        return false;
    }
}

std::optional<std::vector<uint8_t>> FileStore::getChunk(
    const std::string& file_id,
    uint32_t chunk_index)
{
    std::lock_guard<std::mutex> lock(db_.mutex());
    
    try {
        SQLite::Statement query(db_.get(),
            "SELECT chunk_data FROM file_chunks WHERE file_id = ? AND chunk_index = ?");
        query.bind(1, file_id);
        query.bind(2, static_cast<int>(chunk_index));
        
        if (query.executeStep()) {
            auto blob = query.getColumn(0);
            const uint8_t* data = reinterpret_cast<const uint8_t*>(blob.getBlob());
            return std::vector<uint8_t>(data, data + blob.getBytes());
        }
    } catch (const SQLite::Exception& e) {
        spdlog::error("FileStore::getChunk failed: {}", e.what());
    }
    
    return std::nullopt;
}

bool FileStore::hasChunk(const std::string& file_id, uint32_t chunk_index) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    
    try {
        SQLite::Statement query(db_.get(),
            "SELECT 1 FROM file_chunks WHERE file_id = ? AND chunk_index = ?");
        query.bind(1, file_id);
        query.bind(2, static_cast<int>(chunk_index));
        
        return query.executeStep();
    } catch (const SQLite::Exception& e) {
        spdlog::error("FileStore::hasChunk failed: {}", e.what());
        return false;
    }
}

std::vector<uint32_t> FileStore::getMissingChunks(
    const std::string& file_id,
    uint32_t total_chunks)
{
    std::lock_guard<std::mutex> lock(db_.mutex());
    std::vector<uint32_t> missing;
    
    try {
        SQLite::Statement query(db_.get(),
            "SELECT chunk_index FROM file_chunks WHERE file_id = ?");
        query.bind(1, file_id);
        
        std::vector<bool> has(total_chunks, false);
        while (query.executeStep()) {
            uint32_t idx = static_cast<uint32_t>(query.getColumn(0).getInt());
            if (idx < total_chunks) has[idx] = true;
        }
        
        for (uint32_t i = 0; i < total_chunks; ++i) {
            if (!has[i]) missing.push_back(i);
        }
    } catch (const SQLite::Exception& e) {
        spdlog::error("FileStore::getMissingChunks failed: {}", e.what());
    }
    
    return missing;
}

uint64_t FileStore::getReceivedBytes(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    
    try {
        SQLite::Statement query(db_.get(),
            "SELECT COALESCE(SUM(chunk_size), 0) FROM file_chunks WHERE file_id = ?");
        query.bind(1, file_id);
        
        if (query.executeStep()) {
            return static_cast<uint64_t>(query.getColumn(0).getInt64());
        }
    } catch (const SQLite::Exception& e) {
        spdlog::error("FileStore::getReceivedBytes failed: {}", e.what());
    }
    
    return 0;
}

int FileStore::cleanupExpired() {
    std::lock_guard<std::mutex> lock(db_.mutex());
    
    try {
        SQLite::Statement del(db_.get(),
            "DELETE FROM files WHERE expires_at <= ?");
        del.bind(1, nowUnix());
        del.exec();
        
        int removed = db_.get().getChanges();
        if (removed > 0) {
            spdlog::info("FileStore: cleaned up {} expired files", removed);
        }
        return removed;
    } catch (const SQLite::Exception& e) {
        spdlog::error("FileStore::cleanupExpired failed: {}", e.what());
        return 0;
    }
}

FileStore::Stats FileStore::getStats() {
    std::lock_guard<std::mutex> lock(db_.mutex());
    Stats stats;
    
    try {
        SQLite::Statement query1(db_.get(), 
            "SELECT COUNT(*), COALESCE(SUM(file_size), 0) FROM files");
        if (query1.executeStep()) {
            stats.total_files = query1.getColumn(0).getInt();
            stats.total_bytes = static_cast<uint64_t>(query1.getColumn(1).getInt64());
        }
        
        SQLite::Statement query2(db_.get(),
            "SELECT COUNT(*) FROM files WHERE expires_at <= ?");
        query2.bind(1, nowUnix());
        if (query2.executeStep()) {
            stats.expired_files = query2.getColumn(0).getInt();
        }
        
        SQLite::Statement query3(db_.get(),
            "SELECT COUNT(*) FROM files WHERE is_complete = 0");
        if (query3.executeStep()) {
            stats.incomplete_files = query3.getColumn(0).getInt();
        }
    } catch (const SQLite::Exception& e) {
        spdlog::error("FileStore::getStats failed: {}", e.what());
    }
    
    return stats;
}

} // namespace ircord::db
