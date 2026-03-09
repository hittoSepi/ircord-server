#pragma once

#include <string>
#include <cstdint>

namespace ircord {

struct ServerConfig {
    // [server]
    std::string host = "0.0.0.0";
    uint16_t port = 6667;
    std::string log_level = "info";
    int max_connections = 100;

    // [tls]
    std::string tls_cert_file;
    std::string tls_key_file;

    // [limits]
    size_t max_message_bytes = 65536;        // 64 KB
    int ping_interval_sec = 30;
    int ping_timeout_sec = 60;
};

class ConfigLoader {
public:
    // Load config from TOML file
    // Throws std::runtime_error on file not found or parse error
    static ServerConfig load(const std::string& config_path);

    // Load with default values if file doesn't exist
    static ServerConfig load_or_default(const std::string& config_path);

    // Validate config has all required fields
    // Throws std::runtime_error if validation fails
    static void validate(const ServerConfig& config);
};

} // namespace ircord
