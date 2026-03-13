#pragma once

#include <string>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <toml.hpp>


namespace ircord {

	inline const std::string &default_config_path = "./server.toml";
	inline const std::string &__PATH__ = std::filesystem::path( "/" ).string();

	inline const std::string &server_toml_template =
		R"(# IRCord v.0.10.0  -  Server Configuration

[server]
# Server bind address (0.0.0.0 = all interfaces)
host = "0.0.0.0"

# Server port (6667 is the historical IRC port)
port = 6667

# Log level: debug, info, warn, error
log_level = "info"

# Maximum concurrent connections
max_connections = 100

# Message of the Day (optional)
# Supports single string or array for multiline:
# motd = "Welcome to IRCord!"
# motd = ["Welcome to IRCord!", "Please read the rules."]
motd = ""

[tls]
# TLS certificate file (required)
# Generate self-signed for testing:
#   openssl req -x509 -newkey ed25519 -keyout server.key -out server.crt -days 365 -nodes
cert_file = "./certs/server.crt"

# TLS private key file (required)
key_file = "./certs/server.key"

[database]
# SQLite database file path
path = "./ircord.db"

[limits]
# Maximum message size in bytes (64 KB default)
max_message_bytes = 65536

# Ping interval in seconds (server sends PING)
ping_interval_sec = 30

# Ping timeout in seconds (disconnect if no PONG)
ping_timeout_sec = 60

# Maximum messages per second per authenticated user (rate limiting)
msg_rate_per_sec = 20

# Maximum connection attempts per minute per IP address
conn_rate_per_min = 10
)";


	inline std::string get_default_config_path() {

		const char *env_config = std::getenv( "IRCORD_CONFIG" );
		if ( env_config && env_config[0] != '\0' ) {
			return std::string(env_config);
		}
		return default_config_path;
	}

	inline bool file_exists( const std::string &path ) {
		return std::filesystem::exists( std::filesystem::path( path ) );
	}


	inline static void create_config_file( const std::string filename ) {

		if ( file_exists( filename ) ) {
			return;
		}

		std::ofstream config_file( std::filesystem::path( filename ).c_str() );
		config_file << std::string( server_toml_template );
		config_file.close();
	}


struct ServerConfig {
    // [server]
    std::string host = "0.0.0.0";
    uint16_t port = 6667;
    std::string log_level = "info";
    int max_connections = 100;

    // [tls]
    std::string tls_cert_file;
    std::string tls_key_file;

    // [database]
    std::string db_path = "./ircord.db";

    // [limits]
    size_t max_message_bytes = 65536;        // 64 KB
    int ping_interval_sec = 30;
    int ping_timeout_sec = 60;
    int msg_rate_per_sec = 20;               // max messages/sec per authenticated user
    int conn_rate_per_min = 10;              // max connection attempts/min per IP
    int commands_per_min = 30;               // max commands/min per user
    int joins_per_min = 5;                   // max channel joins/min per user
    int abuse_threshold = 5;                 // violations before ban
    int abuse_window_min = 10;               // violation window in minutes
    int ban_duration_min = 30;               // ban duration in minutes
    
    // [security]
    std::string file_encryption_key;         // Master key for file encryption (64 hex chars)
    
    // [antivirus]
    std::string clamav_socket;               // Unix socket path for clamd
    std::string clamav_host = "127.0.0.1";   // TCP host for clamd
    uint16_t clamav_port = 0;                // TCP port for clamd (0 = disabled)
    
    // [motd]
    std::string motd;                        // Message of the Day (empty = disabled)
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
