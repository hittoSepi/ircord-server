#pragma once

#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>

namespace ircord::security {

/**
 * Virus scanner using ClamAV daemon (clamd).
 * 
 * ClamAV is an open-source antivirus engine for detecting trojans,
 * viruses, malware & other malicious threats.
 * 
 * This scanner connects to clamd via Unix socket or TCP and uses
 * the INSTREAM command for scanning file data.
 */
class VirusScanner {
public:
    // Scan result
    struct ScanResult {
        bool clean = false;              // True if no threats found
        bool error = false;              // True if scan failed
        std::string virus_name;          // Name of detected threat (if not clean)
        std::string error_message;       // Error description (if error)
        std::chrono::milliseconds scan_time{0};  // Time taken to scan
    };

    // Connection type
    enum class ConnectionType {
        UnixSocket,   // Unix domain socket (Linux/macOS)
        Tcp           // TCP socket (cross-platform)
    };

    // Constructor - use Unix socket
    explicit VirusScanner(const std::string& socket_path);
    
    // Constructor - use TCP
    VirusScanner(const std::string& host, uint16_t port);

    /**
     * Check if clamd is available and responding.
     */
    bool is_available();

    /**
     * Scan data buffer for viruses.
     * @param data The data to scan
     * @return ScanResult with clean/error status
     */
    ScanResult scan(const std::vector<uint8_t>& data);

    /**
     * Get ClamAV version information.
     */
    std::optional<std::string> get_version();

    /**
     * Ping the daemon to check if alive.
     */
    bool ping();

    /**
     * Get connection type.
     */
    ConnectionType connection_type() const { return type_; }

    /**
     * Get last error message.
     */
    const std::string& last_error() const { return last_error_; }

    /**
     * Set connection timeout.
     */
    void set_timeout(std::chrono::milliseconds timeout) { timeout_ = timeout; }

    /**
     * Set maximum scan size (clamd will truncate larger files).
     * Default: 100MB
     */
    void set_max_scan_size(size_t max_size) { max_scan_size_ = max_size; }

private:
    ConnectionType type_;
    std::string socket_path_;    // For Unix socket
    std::string host_;           // For TCP
    uint16_t port_ = 0;          // For TCP
    
    std::string last_error_;
    std::chrono::milliseconds timeout_{5000};  // 5 second default timeout
    size_t max_scan_size_ = 100 * 1024 * 1024;  // 100MB

#ifdef _WIN32
    // Windows socket handle
    using SocketHandle = uintptr_t;
#else
    // Unix socket handle
    using SocketHandle = int;
#endif

    // Connect to clamd
    SocketHandle connect_to_daemon();
    void close_socket(SocketHandle sock);
    
    // Send command and receive response
    std::optional<std::string> send_command(const std::string& cmd);
    std::optional<std::string> send_instream(const std::vector<uint8_t>& data);
    
    // Parse scan result from clamd response
    ScanResult parse_scan_result(const std::string& response);
    
    // Platform-specific socket operations
    bool set_socket_timeout(SocketHandle sock, std::chrono::milliseconds timeout);
};

/**
 * Singleton scanner manager for server-wide virus scanning.
 */
class VirusScannerManager {
public:
    static VirusScannerManager& instance();

    /**
     * Initialize scanner with socket path or TCP endpoint.
     * Returns false if clamd is not available.
     */
    bool initialize(const std::string& socket_path);
    bool initialize(const std::string& host, uint16_t port);

    /**
     * Check if scanner is initialized and available.
     */
    bool is_available() const;

    /**
     * Scan data. Returns clean result if scanner not available.
     */
    VirusScanner::ScanResult scan(const std::vector<uint8_t>& data);

    /**
     * Enable/disable scanning (for maintenance, testing).
     */
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool is_enabled() const { return enabled_; }

    /**
     * Get statistics.
     */
    struct Stats {
        uint64_t files_scanned = 0;
        uint64_t threats_found = 0;
        uint64_t scan_errors = 0;
        std::chrono::milliseconds total_scan_time{0};
    };
    Stats get_stats() const;
    void reset_stats();

private:
    VirusScannerManager() = default;
    
    std::unique_ptr<VirusScanner> scanner_;
    std::atomic<bool> enabled_{true};
    std::atomic<bool> available_{false};
    
    Stats stats_;
    mutable std::mutex stats_mutex_;
};

} // namespace ircord::security
