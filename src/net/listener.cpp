#include "listener.hpp"
#include "tls_context.hpp"

#include <boost/asio/bind_executor.hpp>
#include <spdlog/spdlog.h>

#include <chrono>

namespace ircord::net {

Listener::Listener(
    boost::asio::io_context& ioc,
    boost::asio::ssl::context& ssl_ctx,
    const std::string& host,
    uint16_t port,
    db::UserStore& user_store,
    db::OfflineStore& offline_store)
    : strand_(boost::asio::make_strand(ioc))
    , acceptor_(boost::asio::make_strand(ioc))
    , socket_(boost::asio::make_strand(ioc))
    , ssl_ctx_(ssl_ctx)
    , signals_(boost::asio::make_strand(ioc), SIGINT, SIGTERM)
    , user_store_(user_store)
    , offline_store_(offline_store)
{
    // Open acceptor
    boost::system::error_code ec;
    acceptor_.open(boost::asio::ip::tcp::v4(), ec);
    if (ec) {
        throw std::runtime_error("Failed to open acceptor: " + ec.message());
    }

    // Allow address reuse
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) {
        throw std::runtime_error("Failed to set reuse_address: " + ec.message());
    }

    // Bind to endpoint
    boost::asio::ip::tcp::endpoint endpoint(
        boost::asio::ip::make_address(host), port);
    acceptor_.bind(endpoint, ec);
    if (ec) {
        throw std::runtime_error("Failed to bind to " + host + ":" + std::to_string(port) + ": " + ec.message());
    }

    // Listen
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw std::runtime_error("Failed to listen: " + ec.message());
    }

    spdlog::info("Listening on {}:{}", host, port);
}

Listener::~Listener() {
    shutdown();
}

void Listener::run() {
    // Initialize command handler if database is set
    if (db_) {
        auto find_session = [this](const std::string& user_id) -> std::shared_ptr<Session> {
            return this->find_session(user_id);
        };
        auto broadcast = [this](const Envelope& env, std::shared_ptr<Session> exclude) {
            this->broadcast(env, exclude);
        };
        command_handler_ = std::make_unique<commands::CommandHandler>(
            find_session, broadcast, *db_, user_store_, offline_store_);
        spdlog::info("Command handler initialized");

        voice_room_mgr_ = std::make_unique<voice::VoiceRoomManager>(
            [this](const std::string& uid) { return this->find_session(uid); });
        spdlog::info("Voice room manager initialized");
    }

    // Start accepting connections
    do_accept();

    // Wait for signals
    signals_.async_wait(
        boost::asio::bind_executor(strand_,
            [this](boost::system::error_code ec, int signal_number) {
                on_signal(ec, signal_number);
            }));
}

void Listener::do_accept() {
    acceptor_.async_accept(socket_,
        boost::asio::bind_executor(strand_,
            [this](boost::system::error_code ec) {
                if (ec) {
                    if (shutting_down_) {
                        return;  // Expected during shutdown
                    }
                    spdlog::error("Accept error: {}", ec.message());
                    if (!shutting_down_) {
                        // Continue accepting
                        socket_ = boost::asio::ip::tcp::socket(
                            boost::asio::make_strand(acceptor_.get_executor()));
                        do_accept();
                    }
                    return;
                }

                // Enforce max_connections
                if (active_connections_.load() >= max_connections_) {
                    spdlog::warn("Max connections ({}) reached, rejecting new connection",
                        max_connections_);
                    boost::system::error_code close_ec;
                    socket_.close(close_ec);
                    socket_ = boost::asio::ip::tcp::socket(
                        boost::asio::make_strand(acceptor_.get_executor()));
                    if (!shutting_down_) do_accept();
                    return;
                }

                // Per-IP connection rate limiting
                std::string remote_ip;
                try {
                    remote_ip = socket_.remote_endpoint().address().to_string();
                } catch (...) {
                    remote_ip = "unknown";
                }
                {
                    std::lock_guard<std::mutex> lk(ip_rate_mutex_);
                    auto [it, inserted] = ip_rate_map_.emplace(
                        std::piecewise_construct,
                        std::forward_as_tuple(remote_ip),
                        std::forward_as_tuple(conn_rate_per_min_,
                                              std::chrono::minutes(1)));
                    if (!it->second.allow()) {
                        spdlog::warn("Connection rate limit exceeded for IP {}", remote_ip);
                        boost::system::error_code close_ec;
                        socket_.close(close_ec);
                        socket_ = boost::asio::ip::tcp::socket(
                            boost::asio::make_strand(acceptor_.get_executor()));
                        if (!shutting_down_) do_accept();
                        return;
                    }
                }

                ++active_connections_;

                // Create new session
                auto session = std::make_shared<Session>(
                    std::move(socket_), ssl_ctx_, *this);
                session->start();

                // Prepare next accept
                socket_ = boost::asio::ip::tcp::socket(
                    boost::asio::make_strand(acceptor_.get_executor()));

                if (!shutting_down_) {
                    do_accept();
                }
            }));
}

void Listener::on_signal(boost::system::error_code ec, int signal_number) {
    if (ec) {
        spdlog::error("Signal error: {}", ec.message());
        return;
    }

    spdlog::info("Received signal {}", signal_number);
    shutdown();
}

void Listener::shutdown() {
    if (shutting_down_.exchange(true)) {
        return;  // Already shutting down
    }

    spdlog::info("Shutting down...");

    // Cancel signal handling
    signals_.cancel();

    // Close acceptor
    boost::system::error_code ec;
    acceptor_.close(ec);

    // Disconnect all sessions
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [session, user_id] : users_by_session_) {
        session->disconnect("Server shutting down");
    }
    sessions_by_user_.clear();
    users_by_session_.clear();

    spdlog::info("Shutdown complete");
}

void Listener::on_session_authenticated(std::shared_ptr<Session> session) {
    const std::string& user_id = session->user_id();

    std::lock_guard<std::mutex> lock(sessions_mutex_);

    // Check if user already has a session
    auto existing_it = sessions_by_user_.find(user_id);
    if (existing_it != sessions_by_user_.end()) {
        // Disconnect old session
        spdlog::info("User {} already connected, replacing old session", user_id);
        existing_it->second->disconnect("New login from another location");
        users_by_session_.erase(existing_it->second);
        sessions_by_user_.erase(existing_it);
    }

    // Add new session
    sessions_by_user_[user_id] = session;
    users_by_session_[session] = user_id;

    spdlog::info("User {} authenticated. Total sessions: {}",
        user_id, sessions_by_user_.size());
}

void Listener::on_session_disconnected(std::shared_ptr<Session> session, const std::string& reason) {
    --active_connections_;

    std::string disconnected_user_id;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);

        auto it = users_by_session_.find(session);
        if (it != users_by_session_.end()) {
            disconnected_user_id = it->second;
            spdlog::info("User {} disconnected: {}", disconnected_user_id, reason);

            sessions_by_user_.erase(disconnected_user_id);
            users_by_session_.erase(it);
        }
    }

    // Broadcast OFFLINE presence after releasing the lock
    if (!disconnected_user_id.empty()) {
        PresenceUpdate presence;
        presence.set_user_id(disconnected_user_id);
        presence.set_status(PresenceUpdate::OFFLINE);
        broadcast_presence(presence, nullptr);

        // Remove from voice rooms
        if (voice_room_mgr_) {
            voice_room_mgr_->on_disconnect(disconnected_user_id);
        }
    }
}

void Listener::broadcast(const Envelope& env, std::shared_ptr<Session> exclude) {
    std::vector<std::shared_ptr<Session>> sessions;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        spdlog::debug("Broadcast: {} authenticated sessions in map", sessions_by_user_.size());
        sessions.reserve(sessions_by_user_.size());
        for (auto& [user_id, session] : sessions_by_user_) {
            if (session != exclude) {
                sessions.push_back(session);
                spdlog::debug("Broadcast: including user {}", user_id);
            } else {
                spdlog::debug("Broadcast: excluding sender {}", user_id);
            }
        }
    }

    spdlog::info("Broadcasting message type {} to {} sessions", 
        static_cast<int>(env.type()), sessions.size());

    for (auto& session : sessions) {
        session->send(env);
    }
}

std::shared_ptr<Session> Listener::find_session(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_by_user_.find(user_id);
    if (it != sessions_by_user_.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<Session> Listener::find_session_by_nickname(const std::string& nickname) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    std::string normalized_target = utils::normalize_nickname(nickname);
    
    for (const auto& [user_id, session] : sessions_by_user_) {
        if (utils::normalize_nickname(user_id) == normalized_target) {
            return session;
        }
    }
    return nullptr;
}

bool Listener::is_nickname_available(const std::string& nickname, 
                                     const std::string& exclude_user_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    std::string normalized_target = utils::normalize_nickname(nickname);
    std::string normalized_exclude = utils::normalize_nickname(exclude_user_id);
    
    for (const auto& [user_id, session] : sessions_by_user_) {
        // Skip the excluded user (for nickname changes by same user)
        if (!exclude_user_id.empty() && 
            utils::normalize_nickname(user_id) == normalized_exclude) {
            continue;
        }
        
        // Check if any online user matches the nickname (case-insensitive)
        if (utils::normalize_nickname(user_id) == normalized_target) {
            return false;  // Nickname is taken
        }
    }
    return true;  // Nickname is available
}

void Listener::broadcast_presence(const PresenceUpdate& update,
                                   std::shared_ptr<Session> exclude) {
    Envelope env;
    env.set_seq(0);
    env.set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    env.set_type(MT_PRESENCE);
    std::vector<uint8_t> payload(update.ByteSizeLong());
    update.SerializeToArray(payload.data(), static_cast<int>(payload.size()));
    env.set_payload(payload.data(), payload.size());

    broadcast(env, exclude);
}

void Listener::set_ping_intervals(int interval_sec, int timeout_sec) {
    ping_interval_sec_ = interval_sec;
    ping_timeout_sec_ = timeout_sec;
}

void Listener::set_rate_limits(int msg_rate_per_sec, int conn_rate_per_min) {
    msg_rate_per_sec_ = msg_rate_per_sec;
    conn_rate_per_min_ = conn_rate_per_min;
}

void Listener::set_max_connections(int max_connections) {
    max_connections_ = max_connections;
}

db::Database& Listener::database() {
    if (!db_) {
        throw std::runtime_error("Database not set");
    }
    return *db_;
}

void Listener::cleanup_dead_sessions() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    // Remove dead sessions
    for (auto it = users_by_session_.begin(); it != users_by_session_.end(); ) {
        if (it->first->state() == SessionState::Dead) {
            const std::string& user_id = it->second;
            sessions_by_user_.erase(user_id);
            it = users_by_session_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace ircord::net
