#include "session.hpp"
#include "config.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/endian/conversion.hpp>
#include <spdlog/spdlog.h>

#include <random>
#include <sstream>
#include <chrono>

namespace ircord::net {

namespace {

// Generate random 32-byte nonce
std::vector<uint8_t> generate_nonce() {
    std::vector<uint8_t> nonce(32);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (auto& b : nonce) {
        b = static_cast<uint8_t>(dist(gen));
    }
    return nonce;
}

// Get current timestamp in milliseconds
uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Convert endpoint to string
template<typename Socket>
std::string endpoint_to_string(const Socket& socket) {
    try {
        const auto& ep = socket.lowest_layer().remote_endpoint();
        std::ostringstream oss;
        oss << ep.address().to_string() << ":" << ep.port();
        return oss.str();
    } catch (...) {
        return "unknown";
    }
}

} // anonymous namespace

// ============================================================================
// Session Implementation
// ============================================================================

Session::Session(
    TcpSocket socket,
    boost::asio::ssl::context& ssl_ctx,
    ServerContext& server_ctx)
    : strand_(boost::asio::make_strand(socket.get_executor()))
    , socket_(std::move(socket), ssl_ctx)
    , server_ctx_(server_ctx)
    , ping_timer_(socket.get_executor())
{
    remote_endpoint_ = endpoint_to_string(socket_);
}

Session::~Session() {
    // Ensure socket is closed
    boost::system::error_code ec;
    socket_.shutdown(ec);
}

void Session::start() {
    auto self = shared_from_this();
    boost::asio::post(strand_, [this, self] {
        spdlog::info("[{}] Session started", remote_endpoint_);
        do_tls_handshake();
    });
}

void Session::do_tls_handshake() {
    auto self = shared_from_this();
    socket_.async_handshake(boost::asio::ssl::stream_base::server,
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& ec) {
                if (ec) {
                    spdlog::warn("[{}] TLS handshake failed: {}", remote_endpoint_, ec.message());
                    server_ctx_.on_session_disconnected(self, "TLS handshake failed");
                    return;
                }

                spdlog::debug("[{}] TLS handshake complete", remote_endpoint_);
                set_state(SessionState::Hello);
                do_read_frame_header();
            }));
}

void Session::do_read_frame_header() {
    if (state_ == SessionState::Dead) {
        return;
    }

    auto self = shared_from_this();
    boost::asio::async_read(socket_,
        boost::asio::buffer(header_buf_),
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& ec, size_t) {
                if (ec) {
                    if (ec != boost::asio::error::eof) {
                        spdlog::debug("[{}] Read error: {}", remote_endpoint_, ec.message());
                    }
                    server_ctx_.on_session_disconnected(self, ec.message());
                    return;
                }

                // Parse header (big-endian uint32)
                uint32_t payload_size = boost::endian::big_to_native(
                    *reinterpret_cast<uint32_t*>(header_buf_.data()));

                // Validate size
                if (payload_size > kMaxMessageSize) {
                    spdlog::warn("[{}] Message too large: {} bytes", remote_endpoint_, payload_size);
                    send_error(4001, "Message too large");
                    server_ctx_.on_session_disconnected(self, "Message too large");
                    return;
                }

                if (payload_size == 0) {
                    spdlog::warn("[{}] Zero-length message", remote_endpoint_);
                    server_ctx_.on_session_disconnected(self, "Zero-length message");
                    return;
                }

                do_read_frame_payload(payload_size);
            }));
}

void Session::do_read_frame_payload(uint32_t payload_size) {
    payload_buf_.resize(payload_size);

    auto self = shared_from_this();
    boost::asio::async_read(socket_,
        boost::asio::buffer(payload_buf_),
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& ec, size_t) {
                if (ec) {
                    spdlog::debug("[{}] Payload read error: {}", remote_endpoint_, ec.message());
                    server_ctx_.on_session_disconnected(self, ec.message());
                    return;
                }

                // Process frame
                on_frame_received(std::move(payload_buf_));

                // Read next frame
                if (state_ != SessionState::Dead) {
                    do_read_frame_header();
                }
            }));
}

void Session::on_frame_received(std::vector<uint8_t> data) {
    try {
        Envelope env;
        if (!env.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
            spdlog::warn("[{}] Failed to parse envelope", remote_endpoint_);
            send_error(4002, "Invalid protobuf");
            return;
        }

        spdlog::trace("[{}] Received envelope type={}", remote_endpoint_,
            static_cast<int>(env.type()));

        handle_envelope(env);

    } catch (const std::exception& e) {
        spdlog::error("[{}] Exception processing frame: {}", remote_endpoint_, e.what());
    }
}

void Session::handle_envelope(const Envelope& env) {
    switch (env.type()) {
    case MT_HELLO:
        if (state_ == SessionState::Hello) {
            Hello hello;
            if (env.payload().empty() || !hello.ParseFromArray(env.payload().data(),
                    static_cast<int>(env.payload().size()))) {
                send_error(4003, "Invalid HELLO message");
                disconnect("Invalid HELLO");
                return;
            }
            handle_hello(hello);
        } else {
            spdlog::warn("[{}] Unexpected HELLO in state {}", remote_endpoint_,
                static_cast<int>(state_));
        }
        break;

    case MT_AUTH_RESPONSE:
        if (state_ == SessionState::AuthPending) {
            AuthResponse auth;
            if (env.payload().empty() || !auth.ParseFromArray(env.payload().data(),
                    static_cast<int>(env.payload().size()))) {
                send_error(4004, "Invalid AUTH_RESPONSE");
                disconnect("Invalid AUTH_RESPONSE");
                return;
            }
            handle_auth_response(auth);
        } else {
            spdlog::warn("[{}] Unexpected AUTH_RESPONSE in state {}", remote_endpoint_,
                static_cast<int>(state_));
        }
        break;

    case MT_PING:
        if (state_ == SessionState::Established) {
            handle_ping(env);
        }
        break;

    case MT_PONG:
        // Reset ping timer on pong
        reset_ping_timer();
        ping_sent_ = false;
        break;

    default:
        spdlog::debug("[{}] Unhandled message type: {}", remote_endpoint_,
            static_cast<int>(env.type()));
        break;
    }
}

void Session::handle_hello(const Hello& hello) {
    spdlog::info("[{}] HELLO: protocol_version={}, client_version={}",
        remote_endpoint_, hello.protocol_version(), hello.client_version());

    // Check protocol version
    if (hello.protocol_version() != kProtocolVersion) {
        spdlog::warn("[{}] Protocol version mismatch: expected {}, got {}",
            remote_endpoint_, kProtocolVersion, hello.protocol_version());
        send_error(4005, "Protocol version mismatch");
        disconnect("Version mismatch");
        return;
    }

    // Generate and send auth challenge
    auto nonce = generate_nonce();
    AuthChallenge challenge;
    challenge.set_nonce(nonce.data(), nonce.size());

    // Store nonce for auth verification (in a real implementation, store securely)
    // For now, we'll accept any non-empty signature

    send_envelope(MT_AUTH_CHALLENGE, challenge);
    set_state(SessionState::AuthPending);

    spdlog::debug("[{}] Sent AUTH_CHALLENGE", remote_endpoint_);
}

void Session::handle_auth_response(const AuthResponse& auth) {
    spdlog::info("[{}] AUTH_RESPONSE: user_id={}", remote_endpoint_, auth.user_id());

    // In Phase 1, we don't verify the signature yet (Phase 2)
    // Just accept the auth and move to Established state

    user_id_ = auth.user_id();

    send_envelope(MT_AUTH_OK, Empty());
    set_state(SessionState::Established);

    spdlog::info("[{}] Session established for user: {}", remote_endpoint_, user_id_);

    // Notify server context
    server_ctx_.on_session_authenticated(shared_from_this());

    // Start ping timer
    start_ping_timer();
}

void Session::handle_ping(const Envelope& env) {
    // Respond with PONG
    Envelope pong;
    pong.set_seq(next_seq_++);
    pong.set_timestamp_ms(now_ms());
    pong.set_type(MT_PONG);
    pong.set_payload(env.payload());  // Echo back ping payload

    send(pong);
}

void Session::send(const Envelope& env) {
    std::vector<uint8_t> data(env.ByteSizeLong());
    env.SerializeToArray(data.data(), data.size());

    // Frame format: 4-byte big-endian length + protobuf payload
    std::vector<uint8_t> frame(4 + data.size());
    uint32_t size_be = boost::endian::native_to_big(static_cast<uint32_t>(data.size()));
    std::memcpy(frame.data(), &size_be, 4);
    std::memcpy(frame.data() + 4, data.data(), data.size());

    auto self = shared_from_this();
    boost::asio::async_write(socket_,
        boost::asio::buffer(frame),
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& ec, size_t) {
                if (ec) {
                    spdlog::warn("[{}] Send error: {}", remote_endpoint_, ec.message());
                    server_ctx_.on_session_disconnected(self, "Send error");
                }
            }));
}

void Session::send_envelope(MessageType type, const google::protobuf::Message& msg) {
    Envelope env;
    env.set_seq(next_seq_++);
    env.set_timestamp_ms(now_ms());
    env.set_type(type);

    std::vector<uint8_t> payload(msg.ByteSizeLong());
    msg.SerializeToArray(payload.data(), payload.size());
    env.set_payload(payload.data(), payload.size());

    send(env);
}

void Session::send_error(uint32_t code, const std::string& message) {
    Error err;
    err.set_code(code);
    err.set_message(message);

    send_envelope(MT_ERROR, err);

    spdlog::warn("[{}] Sent error: {} - {}", remote_endpoint_, code, message);
}

void Session::start_ping_timer() {
    ping_sent_ = false;
    reset_ping_timer();
}

void Session::reset_ping_timer() {
    ping_timer_.expires_after(std::chrono::seconds(server_ctx_.ping_interval_sec()));
    ping_timer_.async_wait(
        boost::asio::bind_executor(strand_,
            [this](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted) {
                    return;  // Timer cancelled
                }

                if (state_ != SessionState::Established) {
                    return;
                }

                if (ping_sent_) {
                    // Ping timeout - no pong received
                    spdlog::warn("[{}] Ping timeout", remote_endpoint_);
                    server_ctx_.on_session_disconnected(shared_from_this(), "Ping timeout");
                    return;
                }

                // Send ping
                Envelope ping;
                ping.set_seq(next_seq_++);
                ping.set_timestamp_ms(now_ms());
                ping.set_type(MT_PING);
                send(ping);
                ping_sent_ = true;

                // Set timeout timer
                ping_timer_.expires_after(std::chrono::seconds(server_ctx_.ping_timeout_sec()));
                ping_timer_.async_wait(
                    boost::asio::bind_executor(strand_,
                        [this](const boost::system::error_code& ec2) {
                            if (ec2 == boost::asio::error::operation_aborted) {
                                return;  // Reset by pong
                            }

                            if (state_ == SessionState::Established) {
                                spdlog::warn("[{}] Ping timeout (no pong)", remote_endpoint_);
                                server_ctx_.on_session_disconnected(shared_from_this(), "Ping timeout");
                            }
                        }));
            }));
}

void Session::disconnect(const std::string& reason) {
    spdlog::info("[{}] Disconnecting: {}", remote_endpoint_, reason);

    auto self = shared_from_this();
    boost::asio::post(strand_, [this, self, reason] {
        set_state(SessionState::Dead);
        ping_timer_.cancel();

        boost::system::error_code ec;
        socket_.shutdown(ec);
        socket_.lowest_layer().close(ec);
    });
}

void Session::set_state(SessionState new_state) {
    SessionState old_state = state_;
    state_ = new_state;
    spdlog::trace("[{}] State: {} -> {}", remote_endpoint_,
        static_cast<int>(old_state), static_cast<int>(new_state));
}

} // namespace ircord::net
