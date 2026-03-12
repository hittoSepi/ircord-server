#include "session.hpp"
#include "config.hpp"
#include "db/user_store.hpp"
#include "db/offline_store.hpp"
#include "db/file_store.hpp"
#include "commands/command_handler.hpp"
#include "security/virus_scanner.hpp"
#include "voice/voice_room_manager.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/endian/conversion.hpp>
#include <spdlog/spdlog.h>
#include <sodium.h>

#include <cctype>
#include <cstring>
#include <optional>
#include <random>
#include <sstream>
#include <chrono>

namespace ircord::net {

namespace {

// Generate cryptographically random 32-byte nonce via libsodium
std::vector<uint8_t> generate_nonce() {
    std::vector<uint8_t> nonce(32);
    randombytes_buf(nonce.data(), nonce.size());
    return nonce;
}

// Validate user_id: 1-64 printable non-space ASCII/UTF-8 chars, no null bytes
bool is_valid_user_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char c : id) {
        if (c == 0 || c == ' ') return false;
        if (c < 0x20) return false;  // reject control characters
    }
    return true;
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

    case MT_CHAT_ENVELOPE:
        if (state_ == SessionState::Established) {
            if (msg_rate_limiter_ && !msg_rate_limiter_->allow()) {
                send_error(4290, "Rate limit exceeded");
                spdlog::warn("[{}] Rate limit exceeded for user {}", remote_endpoint_, user_id_);
                return;
            }
            ChatEnvelope chat;
            if (!env.payload().empty() && chat.ParseFromArray(
                    env.payload().data(), static_cast<int>(env.payload().size()))) {
                handle_chat(chat, env);
            } else {
                send_error(4030, "Invalid CHAT_ENVELOPE");
            }
        } else {
            send_error(4031, "Not authenticated");
        }
        break;

    case MT_VOICE_SIGNAL:
        if (state_ == SessionState::Established) {
            if (msg_rate_limiter_ && !msg_rate_limiter_->allow()) {
                send_error(4290, "Rate limit exceeded");
                return;
            }
            VoiceSignal vs;
            if (!env.payload().empty() && vs.ParseFromArray(
                    env.payload().data(), static_cast<int>(env.payload().size()))) {
                handle_voice_signal(vs, env);
            } else {
                send_error(4070, "Invalid VOICE_SIGNAL");
            }
        } else {
            send_error(4071, "Not authenticated");
        }
        break;


    case MT_VOICE_ROOM_JOIN:
        if (state_ == SessionState::Established) {
            VoiceRoomJoin join;
            if (!env.payload().empty() && join.ParseFromArray(
                    env.payload().data(), static_cast<int>(env.payload().size()))) {
                handle_voice_room_join(join);
            } else {
                send_error(4075, "Invalid VOICE_ROOM_JOIN");
            }
        }
        break;

    case MT_VOICE_ROOM_LEAVE:
        if (state_ == SessionState::Established) {
            VoiceRoomLeave leave;
            if (!env.payload().empty() && leave.ParseFromArray(
                    env.payload().data(), static_cast<int>(env.payload().size()))) {
                handle_voice_room_leave(leave);
            } else {
                send_error(4076, "Invalid VOICE_ROOM_LEAVE");
            }
        }
        break;

    case MT_KEY_UPLOAD:
        if (state_ == SessionState::Established) {
            KeyUpload ku;
            if (!env.payload().empty() && ku.ParseFromArray(
                    env.payload().data(), static_cast<int>(env.payload().size()))) {
                handle_key_upload(ku);
            } else {
                send_error(4040, "Invalid KEY_UPLOAD");
            }
        } else {
            send_error(4041, "Not authenticated");
        }
        break;

    case MT_KEY_REQUEST:
        if (state_ == SessionState::Established) {
            KeyRequest kr;
            if (!env.payload().empty() && kr.ParseFromArray(
                    env.payload().data(), static_cast<int>(env.payload().size()))) {
                handle_key_request(kr);
            } else {
                send_error(4050, "Invalid KEY_REQUEST");
            }
        } else {
            send_error(4051, "Not authenticated");
        }
        break;

    case MT_COMMAND:
        if (state_ == SessionState::Established) {
            IrcCommand cmd;
            if (!env.payload().empty() && cmd.ParseFromArray(
                    env.payload().data(), static_cast<int>(env.payload().size()))) {
                handle_command(cmd);
            } else {
                send_error(4060, "Invalid COMMAND");
            }
        } else {
            send_error(4061, "Not authenticated");
        }
        break;

    case MT_FILE_UPLOAD:
        if (state_ == SessionState::Established) {
            if (env.payload().empty()) {
                send_error(4080, "Empty FILE_UPLOAD");
                break;
            }
            // Try to parse as chunk first (has data field)
            FileUploadChunk chunk;
            if (chunk.ParseFromArray(env.payload().data(),
                    static_cast<int>(env.payload().size())) && chunk.data().size() > 0) {
                handle_file_upload(chunk, env);
            } else {
                // Try as request
                FileUploadRequest req;
                if (req.ParseFromArray(env.payload().data(),
                        static_cast<int>(env.payload().size()))) {
                    handle_file_request(req, env);
                } else {
                    send_error(4082, "Invalid FILE_UPLOAD");
                }
            }
        } else {
            send_error(4081, "Not authenticated");
        }
        break;

    case MT_FILE_DOWNLOAD:
        if (state_ == SessionState::Established) {
            FileDownloadRequest req;
            if (!env.payload().empty() && req.ParseFromArray(
                    env.payload().data(), static_cast<int>(env.payload().size()))) {
                handle_file_download(req);
            } else {
                send_error(4086, "Invalid FILE_DOWNLOAD");
            }
        } else {
            send_error(4087, "Not authenticated");
        }
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

    // Generate and send auth challenge; store nonce for later verification
    auth_nonce_ = generate_nonce();
    AuthChallenge challenge;
    challenge.set_nonce(auth_nonce_.data(), auth_nonce_.size());

    send_envelope(MT_AUTH_CHALLENGE, challenge);
    set_state(SessionState::AuthPending);

    spdlog::debug("[{}] Sent AUTH_CHALLENGE", remote_endpoint_);
}

void Session::handle_auth_response(const AuthResponse& auth) {
    spdlog::info("[{}] AUTH_RESPONSE: user_id={}", remote_endpoint_, auth.user_id());

    // --- Basic field validation ---
    if (!is_valid_user_id(auth.user_id())) {
        send_error(4006, "Invalid user_id (1-64 printable chars, no spaces)");
        disconnect("Invalid user_id");
        return;
    }
    if (auth.identity_pub().size() != crypto_sign_PUBLICKEYBYTES) {
        send_error(4007, "Invalid identity_pub length");
        disconnect("Bad identity_pub");
        return;
    }
    if (auth.signature().size() != crypto_sign_BYTES) {
        send_error(4008, "Invalid signature length");
        disconnect("Bad signature");
        return;
    }

    // --- Ed25519 signature verification ---
    // Message = nonce (32 bytes) || user_id (UTF-8)
    std::vector<uint8_t> message;
    message.insert(message.end(), auth_nonce_.begin(), auth_nonce_.end());
    message.insert(message.end(), auth.user_id().begin(), auth.user_id().end());

    const auto* sig = reinterpret_cast<const unsigned char*>(auth.signature().data());
    const auto* pk  = reinterpret_cast<const unsigned char*>(auth.identity_pub().data());

    if (crypto_sign_verify_detached(sig, message.data(), message.size(), pk) != 0) {
        Error err;
        err.set_code(4009);
        err.set_message("Signature verification failed");
        send_envelope(MT_AUTH_FAIL, err);
        disconnect("Auth failed: bad signature");
        return;
    }

    // --- DB: register new user or verify known user ---
    auto& us = server_ctx_.user_store();
    auto existing = us.find_by_id(auth.user_id());

    const std::vector<uint8_t> presented_key(
        auth.identity_pub().begin(), auth.identity_pub().end());

    if (!existing) {
        // New user — register
        db::User user;
        user.user_id     = auth.user_id();
        user.identity_pub = presented_key;
        user.created_at  = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        try {
            us.insert(user);
        } catch (const std::exception& e) {
            spdlog::error("[{}] DB insert failed: {}", remote_endpoint_, e.what());
            send_error(5001, "Internal error");
            disconnect("DB error");
            return;
        }

        // Store initial signed pre-key if provided
        if (!auth.signed_prekey().empty() && !auth.spk_sig().empty()) {
            us.upsert_signed_prekey(
                auth.user_id(),
                std::vector<uint8_t>(auth.signed_prekey().begin(), auth.signed_prekey().end()),
                std::vector<uint8_t>(auth.spk_sig().begin(), auth.spk_sig().end()));
        }

        spdlog::info("[{}] Registered new user: {}", remote_endpoint_, auth.user_id());
    } else {
        // Known user — verify identity key matches stored key
        if (existing->identity_pub != presented_key) {
            Error err;
            err.set_code(4010);
            err.set_message("Identity key mismatch");
            send_envelope(MT_AUTH_FAIL, err);
            disconnect("Auth failed: key mismatch");
            return;
        }
    }

    // --- Auth success ---
    user_id_ = auth.user_id();

    send_envelope(MT_AUTH_OK, Empty());
    set_state(SessionState::Established);

    spdlog::info("[{}] Session established for user: {}", remote_endpoint_, user_id_);

    server_ctx_.on_session_authenticated(shared_from_this());
    start_ping_timer();

    // Initialise per-session message rate limiter
    msg_rate_limiter_.emplace(
        server_ctx_.msg_rate_per_sec(),
        std::chrono::seconds(1));

    // --- Deliver offline messages ---
    auto offline_msgs = server_ctx_.offline_store().fetch_and_delete(user_id_);
    for (const auto& payload : offline_msgs) {
        Envelope env;
        if (env.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
            send(env);
        }
    }
    if (!offline_msgs.empty()) {
        spdlog::info("[{}] Delivered {} offline messages to {}",
            remote_endpoint_, offline_msgs.size(), user_id_);
    }

    // --- Broadcast ONLINE presence ---
    PresenceUpdate presence;
    presence.set_user_id(user_id_);
    presence.set_status(PresenceUpdate::ONLINE);
    server_ctx_.broadcast_presence(presence, shared_from_this());
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

void Session::handle_chat(const ChatEnvelope& chat, const Envelope& raw) {
    spdlog::info("[{}] handle_chat: sender={}, claimed={}, user_id_={}",
        remote_endpoint_, chat.sender_id(), chat.sender_id(), user_id_);
    
    // Reject sender spoofing
    if (chat.sender_id() != user_id_) {
        spdlog::warn("[{}] sender_id mismatch: claimed={}, actual={}",
            remote_endpoint_, chat.sender_id(), user_id_);
        send_error(4032, "Sender ID mismatch");
        disconnect("Sender ID spoofing attempt");
        return;
    }

    if (chat.recipient_id().empty()) {
        send_error(4033, "Empty recipient_id");
        return;
    }

    const std::string& recipient = chat.recipient_id();
    spdlog::info("[{}] handle_chat: recipient={}, is_channel={}",
        remote_endpoint_, recipient, (!recipient.empty() && recipient[0] == '#'));

    // Channel fanout: broadcast to all authenticated sessions except sender
    if (!recipient.empty() && recipient[0] == '#') {
        spdlog::info("[{}] Broadcasting CHAT from {} to channel {}",
            remote_endpoint_, user_id_, recipient);
        server_ctx_.broadcast(raw, shared_from_this());
        return;
    }

    // 1:1 DM: route to recipient or store offline
    auto recipient_session = server_ctx_.find_session(recipient);
    if (recipient_session) {
        recipient_session->send(raw);
        spdlog::debug("[{}] Routed CHAT {} -> {} (online)",
            remote_endpoint_, user_id_, recipient);
    } else {
        std::vector<uint8_t> payload(raw.ByteSizeLong());
        raw.SerializeToArray(payload.data(), static_cast<int>(payload.size()));
        bool saved = server_ctx_.offline_store().save(recipient, payload);
        if (saved) {
            spdlog::debug("[{}] Stored CHAT {} -> {} (offline)",
                remote_endpoint_, user_id_, recipient);
        } else {
            spdlog::warn("[{}] Failed to store CHAT {} -> {} (queue full)",
                remote_endpoint_, user_id_, recipient);
            send_error(4034, "Recipient offline queue full");
        }
    }
}

void Session::handle_voice_signal(const VoiceSignal& vs, const Envelope& raw) {
    // Anti-spoofing: from_user must match authenticated user
    if (vs.from_user() != user_id_) {
        spdlog::warn("[{}] VoiceSignal from_user mismatch: claimed={}, actual={}",
            remote_endpoint_, vs.from_user(), user_id_);
        send_error(4072, "from_user mismatch");
        disconnect("Voice signal spoofing attempt");
        return;
    }

    if (vs.to_user().empty()) {
        send_error(4073, "Empty to_user");
        return;
    }

    auto recipient = server_ctx_.find_session(vs.to_user());
    if (recipient) {
        recipient->send(raw);
        spdlog::debug("[{}] Relayed VoiceSignal type={} {} -> {}",
            remote_endpoint_,
            static_cast<int>(vs.signal_type()),
            user_id_, vs.to_user());
    } else {
        // Voice signals are ephemeral — no offline storage.
        // For CALL_INVITE, notify the caller so they know the call can't be placed.
        if (vs.signal_type() == VoiceSignal::CALL_INVITE) {
            Error err;
            err.set_code(4074);
            err.set_message("Recipient is offline");
            send_envelope(MT_ERROR, err);
        }
        spdlog::debug("[{}] VoiceSignal dropped — {} is offline", remote_endpoint_, vs.to_user());
    }
}


void Session::handle_voice_room_join(const VoiceRoomJoin& join) {
    std::string error = server_ctx_.voice_room_manager().join(
        join.channel_id(), user_id_);
    if (!error.empty()) {
        send_error(4077, error);
    }
}

void Session::handle_voice_room_leave(const VoiceRoomLeave& leave) {
    server_ctx_.voice_room_manager().leave(leave.channel_id(), user_id_);
}

void Session::handle_key_upload(const KeyUpload& ku) {
    auto& us = server_ctx_.user_store();

    // Update signed pre-key
    if (!ku.signed_prekey().empty() && !ku.spk_signature().empty()) {
        us.upsert_signed_prekey(
            user_id_,
            std::vector<uint8_t>(ku.signed_prekey().begin(), ku.signed_prekey().end()),
            std::vector<uint8_t>(ku.spk_signature().begin(), ku.spk_signature().end()));
    }

    // Store one-time pre-keys
    for (const auto& opk_bytes : ku.one_time_prekeys()) {
        us.store_opk(user_id_,
            std::vector<uint8_t>(opk_bytes.begin(), opk_bytes.end()));
    }

    spdlog::info("[{}] KEY_UPLOAD: {} OPKs uploaded by {}",
        remote_endpoint_, ku.one_time_prekeys_size(), user_id_);
}

void Session::handle_key_request(const KeyRequest& kr) {
    auto& us = server_ctx_.user_store();

    auto target = us.find_by_id(kr.user_id());
    if (!target) {
        send_error(4060, "User not found");
        return;
    }

    auto spk = us.get_signed_prekey(kr.user_id());
    auto opk = us.consume_opk(kr.user_id());

    KeyBundle bundle;
    bundle.set_identity_pub(target->identity_pub.data(),
                             target->identity_pub.size());

    if (spk) {
        bundle.set_signed_prekey(spk->spk_pub.data(), spk->spk_pub.size());
        bundle.set_spk_signature(spk->spk_sig.data(), spk->spk_sig.size());
    }

    if (!opk.empty()) {
        bundle.set_one_time_prekey(opk.data(), opk.size());
    }

    bundle.set_recipient_for(kr.user_id());

    spdlog::debug("[{}] KEY_REQUEST: sending bundle for {} (opk={})",
        remote_endpoint_, kr.user_id(), !opk.empty());

    send_envelope(MT_KEY_BUNDLE, bundle);
}

void Session::handle_file_request(const FileUploadRequest& req, const Envelope& raw) {
    auto& file_store = server_ctx_.file_store();
    
    // Validate request
    if (req.file_size() == 0) {
        FileError err;
        err.set_file_id(req.file_id());
        err.set_error_code(4080);
        err.set_error_message("File size must be greater than 0");
        send_envelope(MT_FILE_ERROR, err);
        return;
    }
    
    // Check max file size (100 MB)
    constexpr uint64_t max_file_size = 100 * 1024 * 1024;
    if (req.file_size() > max_file_size) {
        FileError err;
        err.set_file_id(req.file_id());
        err.set_error_code(4085);
        err.set_error_message("File too large (max 100 MB)");
        send_envelope(MT_FILE_ERROR, err);
        return;
    }
    
    // Create file entry
    db::FileMetadata metadata;
    metadata.file_id = req.file_id();
    metadata.filename = req.filename();
    metadata.file_size = req.file_size();
    metadata.mime_type = req.mime_type();
    metadata.sender_id = user_id_;
    metadata.recipient_id = req.recipient_id();
    metadata.channel_id = req.channel_id();
    
    if (!file_store.createFile(metadata)) {
        FileError err;
        err.set_file_id(req.file_id());
        err.set_error_code(4081);
        err.set_error_message("Failed to create file entry");
        send_envelope(MT_FILE_ERROR, err);
        return;
    }
    
    // Initialize upload state
    {
        std::lock_guard<std::mutex> lock(uploads_mutex_);
        auto& state = active_uploads_[req.file_id()];
        state.file_id = req.file_id();
        state.file_size = req.file_size();
        state.chunk_size = req.chunk_size() > 0 ? req.chunk_size() : 65536;
        state.total_chunks = static_cast<uint32_t>((req.file_size() + state.chunk_size - 1) / state.chunk_size);
        state.received_chunks.resize(state.total_chunks, false);
        state.bytes_received = 0;
    }
    
    // Send progress update as acknowledgment
    FileProgress progress;
    progress.set_file_id(req.file_id());
    progress.set_bytes_transferred(0);
    progress.set_total_bytes(req.file_size());
    progress.set_percent_complete(0.0f);
    progress.set_status("uploading");
    send_envelope(MT_FILE_PROGRESS, progress);
    
    spdlog::info("[{}] File upload started: {} ({} bytes, {})",
        remote_endpoint_, req.file_id(), req.file_size(), req.filename());
}

void Session::handle_file_upload(const FileUploadChunk& chunk, const Envelope& raw) {
    auto& file_store = server_ctx_.file_store();
    
    // Check rate limit
    if (msg_rate_limiter_ && !msg_rate_limiter_->allow()) {
        FileError err;
        err.set_file_id(chunk.file_id());
        err.set_error_code(4290);
        err.set_error_message("Rate limit exceeded");
        send_envelope(MT_FILE_ERROR, err);
        return;
    }
    
    std::lock_guard<std::mutex> lock(uploads_mutex_);
    
    // Find upload state
    auto it = active_uploads_.find(chunk.file_id());
    if (it == active_uploads_.end()) {
        FileError err;
        err.set_file_id(chunk.file_id());
        err.set_error_code(4082);
        err.set_error_message("Upload not started - send FileUploadRequest first");
        send_envelope(MT_FILE_ERROR, err);
        return;
    }
    
    auto& upload = it->second;
    
    // Validate chunk index
    if (chunk.chunk_index() >= upload.total_chunks) {
        FileError err;
        err.set_file_id(chunk.file_id());
        err.set_error_code(4083);
        err.set_error_message("Invalid chunk index");
        send_envelope(MT_FILE_ERROR, err);
        return;
    }
    
    // Store the chunk
    std::vector<uint8_t> data(chunk.data().begin(), chunk.data().end());
    if (!file_store.storeChunk(chunk.file_id(), chunk.chunk_index(), data)) {
        FileError err;
        err.set_file_id(chunk.file_id());
        err.set_error_code(4084);
        err.set_error_message("Failed to store chunk");
        send_envelope(MT_FILE_ERROR, err);
        return;
    }
    
    // Update state
    upload.received_chunks[chunk.chunk_index()] = true;
    upload.bytes_received += data.size();
    
    // Send progress update
    FileProgress progress;
    progress.set_file_id(chunk.file_id());
    progress.set_bytes_transferred(upload.bytes_received);
    progress.set_total_bytes(upload.file_size);
    progress.set_percent_complete(
        static_cast<float>(upload.bytes_received) / upload.file_size * 100.0f);
    progress.set_status("uploading");
    send_envelope(MT_FILE_PROGRESS, progress);
    
    // If last chunk, mark complete
    if (chunk.is_last()) {
        // Assemble all chunks for virus scanning
        std::vector<uint8_t> assembled_file;
        assembled_file.reserve(upload.file_size);
        
        for (uint32_t i = 0; i < upload.total_chunks; ++i) {
            auto chunk_data = file_store.getChunk(chunk.file_id(), i);
            if (chunk_data) {
                assembled_file.insert(assembled_file.end(), 
                    chunk_data->begin(), chunk_data->end());
            }
        }
        
        // Virus scan
        auto& scanner_mgr = security::VirusScannerManager::instance();
        if (scanner_mgr.is_enabled()) {
            spdlog::debug("[{}] Scanning file {} for viruses ({} bytes)",
                remote_endpoint_, chunk.file_id(), assembled_file.size());
            
            auto scan_result = scanner_mgr.scan(assembled_file);
            
            if (scan_result.error) {
                spdlog::warn("[{}] Virus scan failed for {}: {}",
                    remote_endpoint_, chunk.file_id(), scan_result.error_message);
                // Continue anyway - fail open for availability
            } else if (!scan_result.clean) {
                spdlog::error("[{}] THREAT DETECTED in file {}: {}",
                    remote_endpoint_, chunk.file_id(), scan_result.virus_name);
                
                // Delete infected file
                file_store.deleteFile(chunk.file_id());
                
                // Notify client
                FileError err;
                err.set_file_id(chunk.file_id());
                err.set_error_code(4090);
                err.set_error_message("Threat detected: " + scan_result.virus_name);
                send_envelope(MT_FILE_ERROR, err);
                
                active_uploads_.erase(it);
                return;
            } else {
                spdlog::debug("[{}] File {} is clean (scan time: {} ms)",
                    remote_endpoint_, chunk.file_id(), scan_result.scan_time.count());
            }
        }
        
        // TODO: Calculate full file checksum
        std::vector<uint8_t> checksum;
        file_store.markComplete(chunk.file_id(), checksum);
        
        // Notify completion
        FileComplete complete;
        complete.set_file_id(chunk.file_id());
        complete.set_total_bytes(upload.bytes_received);
        send_envelope(MT_FILE_COMPLETE, complete);
        
        spdlog::info("[{}] File upload complete: {} ({} bytes)",
            remote_endpoint_, chunk.file_id().c_str(), upload.bytes_received);
        
        // Clean up upload state
        active_uploads_.erase(it);
    }
}

void Session::handle_file_download(const FileDownloadRequest& req) {
    auto& file_store = server_ctx_.file_store();
    
    // Get file metadata
    auto metadata = file_store.getFile(req.file_id());
    if (!metadata) {
        FileError err;
        err.set_file_id(req.file_id());
        err.set_error_code(4083);
        err.set_error_message("File not found");
        send_envelope(MT_FILE_ERROR, err);
        return;
    }
    
    // Calculate chunk size and total chunks
    constexpr size_t chunk_size = 64 * 1024;  // 64 KB chunks
    size_t total_chunks = (metadata->file_size + chunk_size - 1) / chunk_size;
    
    // Send chunks starting from requested index
    for (size_t i = req.chunk_index(); i < total_chunks; ++i) {
        auto chunk_data = file_store.getChunk(req.file_id(), static_cast<uint32_t>(i));
        if (!chunk_data) {
            FileError err;
            err.set_file_id(req.file_id());
            err.set_error_code(4084);
            err.set_error_message("Chunk not found: " + std::to_string(i));
            send_envelope(MT_FILE_ERROR, err);
            return;
        }
        
        FileChunk chunk;
        chunk.set_file_id(req.file_id());
        chunk.set_chunk_index(static_cast<uint32_t>(i));
        chunk.set_data(chunk_data->data(), chunk_data->size());
        chunk.set_is_last(i == total_chunks - 1);
        
        send_envelope(MT_FILE_CHUNK, chunk);
        
        // Send progress update
        FileProgress progress;
        progress.set_file_id(req.file_id());
        progress.set_bytes_transferred((i + 1) * chunk_size);
        progress.set_total_bytes(metadata->file_size);
        progress.set_percent_complete(
            static_cast<float>((i + 1) * chunk_size) / metadata->file_size * 100.0f);
        progress.set_status("downloading");
        send_envelope(MT_FILE_PROGRESS, progress);
    }
    
    spdlog::info("[{}] File download started: {} ({} chunks)",
        remote_endpoint_, req.file_id(), total_chunks);
}

void Session::send(const Envelope& env) {
    const int byte_size = static_cast<int>(env.ByteSizeLong());
    std::vector<uint8_t> data(static_cast<size_t>(byte_size));
    env.SerializeToArray(data.data(), byte_size);

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

    const int msg_size = static_cast<int>(msg.ByteSizeLong());
    std::vector<uint8_t> payload(static_cast<size_t>(msg_size));
    msg.SerializeToArray(payload.data(), msg_size);
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

void Session::handle_command(const IrcCommand& cmd) {
    spdlog::debug("[{}] COMMAND: {} from {}", remote_endpoint_, cmd.command(), user_id_);

    // Get command handler from server context
    auto* cmd_handler = server_ctx_.command_handler();
    if (!cmd_handler) {
        send_error(4062, "Command handler not available");
        return;
    }

    CommandResponse response = cmd_handler->handle_command(cmd, shared_from_this());

    // Send response back to client
    Envelope env;
    env.set_type(MT_COMMAND_RESPONSE);
    response.SerializeToString(env.mutable_payload());
    send(env);
}

} // namespace ircord::net
