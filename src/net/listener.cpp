#include "listener.hpp"
#include "tls_context.hpp"

#include <boost/asio/bind_executor.hpp>
#include <spdlog/spdlog.h>

namespace ircord::net {

Listener::Listener(
    boost::asio::io_context& ioc,
    boost::asio::ssl::context& ssl_ctx,
    const std::string& host,
    uint16_t port)
    : strand_(boost::asio::make_strand(ioc))
    , acceptor_(boost::asio::make_strand(ioc))
    , socket_(boost::asio::make_strand(ioc))
    , ssl_ctx_(ssl_ctx)
    , signals_(boost::asio::make_strand(ioc), SIGINT, SIGTERM)
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
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = users_by_session_.find(session);
    if (it != users_by_session_.end()) {
        const std::string& user_id = it->second;
        spdlog::info("User {} disconnected: {}", user_id, reason);

        sessions_by_user_.erase(user_id);
        users_by_session_.erase(it);
    }
}

void Listener::broadcast(const Envelope& env, std::shared_ptr<Session> exclude) {
    std::vector<std::shared_ptr<Session>> sessions;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions.reserve(sessions_by_user_.size());
        for (auto& [user_id, session] : sessions_by_user_) {
            if (session != exclude) {
                sessions.push_back(session);
            }
        }
    }

    for (auto& session : sessions) {
        session->send(env);
    }

    spdlog::debug("Broadcast to {} sessions", sessions.size());
}

void Listener::set_ping_intervals(int interval_sec, int timeout_sec) {
    ping_interval_sec_ = interval_sec;
    ping_timeout_sec_ = timeout_sec;
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
