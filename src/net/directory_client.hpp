#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include "../config.hpp"

namespace ircord {

// Client for registering with and pinging the IRCord directory service
class DirectoryClient : public std::enable_shared_from_this<DirectoryClient> {
public:
    DirectoryClient(boost::asio::io_context& io_context, const ServerConfig& config);
    ~DirectoryClient();

    // Start the directory client - registers and begins periodic pings
    void start();

    // Stop the directory client
    void stop();

    // Check if directory client is enabled and running
    bool is_enabled() const { return enabled_; }

    // Get last error message (if any)
    std::string last_error() const { return last_error_; }

private:
    // Register with the directory service
    void register_server();

    // Send periodic ping to directory
    void send_ping();

    // Schedule next ping
    void schedule_next_ping();

    // Build HTTP POST request string
    std::string build_http_request(const std::string& path, const std::string& json_body);

    // Perform HTTP POST request (supports both plain TCP and TLS)
    void post_request(const std::string& path, const std::string& json_body,
                      std::function<void(bool, const std::string&)> callback);

    boost::asio::io_context& io_context_;
    std::unique_ptr<boost::asio::steady_timer> timer_;
    
    ServerConfig config_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> registered_{false};
    std::atomic<bool> running_{false};
    std::string last_error_;
    std::string server_id_;  // Assigned by directory on registration
    
    // Parsed directory URL components
    std::string directory_host_;
    std::string directory_path_;
    uint16_t directory_port_;
    bool directory_use_ssl_;
};

} // namespace ircord
