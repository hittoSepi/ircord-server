#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <array>
#include <vector>
#include <string>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <mutex>

#include "ircord.pb.h"
#include "net/rate_limiter.hpp"

// Forward declarations for db types (avoid pulling heavy headers into every TU)
namespace ircord::db { class UserStore; class OfflineStore; class Database; class FileStore; }
namespace ircord::commands { class CommandHandler; }
namespace ircord::voice { class VoiceRoomManager; }

namespace ircord::net {

// Forward declarations
class ServerContext;

// Session states
enum class SessionState {
    Handshake,      // TLS handshake in progress
    Hello,          // Waiting for HELLO message
    AuthPending,    // Sent AUTH_CHALLENGE, waiting for AUTH_RESPONSE
    Established,    // Authenticated, normal operation
    Dead            // Session closed
};

// Per-connection session state machine
class Session : public std::enable_shared_from_this<Session> {
public:
    using TcpSocket = boost::asio::ip::tcp::socket;
    using SslSocket = boost::asio::ssl::stream<TcpSocket>;
    using Strand = boost::asio::strand<boost::asio::any_io_executor>;
    using Timer = boost::asio::steady_timer;

    Session(
        TcpSocket socket,
        boost::asio::ssl::context& ssl_ctx,
        ServerContext& server_ctx);

    ~Session();

    // Start the session: begin TLS handshake and async read loop
    void start();

    // Send a protobuf envelope to the client
    void send(const Envelope& env);

    // Disconnect the session with optional reason
    void disconnect(const std::string& reason = "");

    // Getters
    const std::string& user_id() const { return user_id_; }
    SessionState state() const { return state_; }
    const std::string& remote_endpoint() const { return remote_endpoint_; }

private:
    // TLS handshake
    void do_tls_handshake();

    // Frame read loop
    void do_read_frame_header();
    void do_read_frame_payload(uint32_t payload_size);

    // Frame processing
    void on_frame_received(std::vector<uint8_t> data);
    void handle_envelope(const Envelope& env);

    // Message handlers
    void handle_hello(const Hello& hello);
    void handle_auth_response(const AuthResponse& auth);
    void handle_ping(const Envelope& env);
    void handle_chat(const ChatEnvelope& chat, const Envelope& raw);
    void handle_key_upload(const KeyUpload& ku);
    void handle_key_request(const KeyRequest& kr);
    void handle_voice_signal(const VoiceSignal& vs, const Envelope& raw);
    void handle_voice_room_join(const VoiceRoomJoin& join);
    void handle_voice_room_leave(const VoiceRoomLeave& leave);
    void handle_command(const IrcCommand& cmd);
    void handle_file_request(const FileUploadRequest& req, const Envelope& raw);
    void handle_file_upload(const FileUploadChunk& chunk, const Envelope& raw);
    void handle_file_download(const FileDownloadRequest& req);

    // Send helpers
    void send_envelope(MessageType type, const google::protobuf::Message& msg);
    void send_error(uint32_t code, const std::string& message);

    // Timer for ping/pong
    void start_ping_timer();
    void on_ping_timer();
    void reset_ping_timer();

    // State transitions
    void set_state(SessionState new_state);

    // Member variables
    Strand strand_;
    SslSocket socket_;
    ServerContext& server_ctx_;

    std::string remote_endpoint_;
    SessionState state_ = SessionState::Handshake;
    std::string user_id_;

    // Frame buffers
    std::array<uint8_t, 4> header_buf_;
    std::vector<uint8_t> payload_buf_;

    // Maximum message size (from config)
    static constexpr size_t kMaxMessageSize = 65536;  // 64 KB

    // Sequencing
    uint64_t next_seq_ = 1;

    // Ping/pong timer
    Timer ping_timer_;
    std::atomic<bool> ping_sent_{false};

    // Nonce sent in AUTH_CHALLENGE (stored for verification in AUTH_RESPONSE)
    std::vector<uint8_t> auth_nonce_;

    // Per-session message rate limiter (initialised after auth)
    std::optional<RateLimiter> msg_rate_limiter_;

    // File upload state
    struct FileUploadState {
        std::string file_id;
        uint64_t file_size = 0;
        uint32_t chunk_size = 65536;
        uint32_t total_chunks = 0;
        std::vector<bool> received_chunks;
        uint64_t bytes_received = 0;
    };
    std::unordered_map<std::string, FileUploadState> active_uploads_;
    std::mutex uploads_mutex_;

    // Protocol version
    static constexpr uint32_t kProtocolVersion = 1;
};

// Server context shared by all sessions
class ServerContext {
public:
    virtual ~ServerContext() = default;

    // Called when a session completes authentication
    virtual void on_session_authenticated(std::shared_ptr<Session> session) = 0;

    // Called when a session disconnects
    virtual void on_session_disconnected(std::shared_ptr<Session> session, const std::string& reason) = 0;

    // Broadcast message to all authenticated sessions
    virtual void broadcast(const Envelope& env, std::shared_ptr<Session> exclude = nullptr) = 0;

    // Broadcast a presence update to all authenticated sessions
    virtual void broadcast_presence(const PresenceUpdate& update,
                                    std::shared_ptr<Session> exclude = nullptr) = 0;

    // Find an online authenticated session by user_id (returns nullptr if offline)
    virtual std::shared_ptr<Session> find_session(const std::string& user_id) = 0;

    // Check if a nickname is already in use by an online user (case-insensitive)
    // Returns the existing session if found, nullptr if available
    virtual std::shared_ptr<Session> find_session_by_nickname(const std::string& nickname) = 0;

    // Check if a nickname is available (not in use by another online user)
    // This is the primary method for nickname availability checks
    virtual bool is_nickname_available(const std::string& nickname, 
                                        const std::string& exclude_user_id = "") = 0;

    // DB stores for auth and offline delivery
    virtual db::UserStore& user_store() = 0;
    virtual db::OfflineStore& offline_store() = 0;
    virtual db::Database& database() = 0;
    virtual db::FileStore& file_store() = 0;

    // Command handler for IRC commands
    virtual commands::CommandHandler* command_handler() = 0;

    // Voice room manager
    virtual voice::VoiceRoomManager& voice_room_manager() = 0;

    // Get current config values
    virtual int ping_interval_sec() const = 0;
    virtual int ping_timeout_sec() const = 0;
    virtual int msg_rate_per_sec() const = 0;
    virtual const std::string& motd() const = 0;
};

} // namespace ircord::net
