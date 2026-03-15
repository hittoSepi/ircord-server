#pragma once

#include "reserved_identity.hpp"

#include <boost/asio/io_context.hpp>
#include <sodium.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <functional>

// Forward declarations
namespace ircord {
class Server;
}

namespace ircord::admin {

/**
 * @brief Represents the server owner/admin identity.
 * 
 * This is an internal identity that:
 * - Always has user_id "admin"
 * - Is always online
 * - Runs inside the server process
 * - Has elevated privileges
 * - Can send messages without network connection
 */
class ServerOwner {
public:
    // Presence status
    enum class PresenceStatus {
        Online,
        Away,
        Busy
    };
    
    struct Presence {
        std::string user_id;
        PresenceStatus status;
        std::string status_message;
    };
    
    // Admin command result
    struct CommandResult {
        bool success;
        std::string message;
    };
    
    /**
     * @brief Construct the server owner.
     * @param server Reference to the main server
     * @param key_path Path to Ed25519 private key file
     */
    ServerOwner(Server& server, const std::string& key_path = "./keys/admin.ed25519");
    ~ServerOwner();
    
    // Non-copyable, non-movable
    ServerOwner(const ServerOwner&) = delete;
    ServerOwner& operator=(const ServerOwner&) = delete;
    ServerOwner(ServerOwner&&) = delete;
    ServerOwner& operator=(ServerOwner&&) = delete;
    
    /**
     * @brief Get the fixed user ID (always "admin").
     */
    static std::string_view user_id() { return ReservedIdentity::OWNER_ID; }
    
    /**
     * @brief Get presence info (always online).
     */
    Presence get_presence() const;
    
    /**
     * @brief Send a message to a channel (internal routing).
     */
    void send_to_channel(const std::string& channel_id, const std::string& message);
    
    /**
     * @brief Send a direct message to a user (internal routing).
     */
    void send_to_user(const std::string& user_id, const std::string& message);
    
    /**
     * @brief Execute an admin command.
     * @param command The command name (without /)
     * @param args Command arguments
     * @return Command result
     */
    CommandResult execute_command(const std::string& command, 
                                   const std::vector<std::string>& args);
    
    /**
     * @brief Execute a command from raw input string.
     */
    CommandResult execute_command_line(const std::string& command_line);
    
    /**
     * @brief Get the Ed25519 public key for verification.
     */
    std::vector<unsigned char> get_public_key() const;
    
    /**
     * @brief Check if the owner is initialized (keys loaded).
     */
    bool is_initialized() const { return keys_loaded_; }
    
    /**
     * @brief Generate a new key pair if none exists.
     */
    static bool generate_key_pair(const std::string& key_path);

    // Admin command handlers
    CommandResult cmd_announce(const std::vector<std::string>& args);
    CommandResult cmd_ban(const std::vector<std::string>& args);
    CommandResult cmd_kick(const std::vector<std::string>& args);
    CommandResult cmd_shutdown(const std::vector<std::string>& args);
    CommandResult cmd_restart(const std::vector<std::string>& args);
    CommandResult cmd_status(const std::vector<std::string>& args);
    CommandResult cmd_config(const std::vector<std::string>& args);
    CommandResult cmd_help(const std::vector<std::string>& args);

private:
    Server& server_;
    std::string key_path_;
    
    // Ed25519 keys
    std::vector<unsigned char> public_key_;
    std::vector<unsigned char> private_key_;
    bool keys_loaded_ = false;
    
    // Load or generate keys
    bool load_keys();
    
    // Sign a message with the owner's private key
    std::vector<unsigned char> sign_message(const std::string& message);
};

} // namespace ircord::admin
