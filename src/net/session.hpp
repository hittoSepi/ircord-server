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

#include "ircord.pb.h"

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

    // Get current config values
    virtual int ping_interval_sec() const = 0;
    virtual int ping_timeout_sec() const = 0;
};

} // namespace ircord::net
