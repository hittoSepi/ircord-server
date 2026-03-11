#include "server.hpp"
#include "net/tls_context.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <sodium.h>

#include <iostream>
#include <csignal>

namespace ircord {

namespace {

// Global server pointer for signal handler
Server* g_server = nullptr;

void signal_handler(int signal) {
    if (g_server) {
        spdlog::info("Received signal {}", signal);
        g_server->shutdown();
    }
}

} // anonymous namespace

Server::Server(const ServerConfig& config)
    : config_(config)
    , ssl_ctx_(boost::asio::ssl::context::tls_server)
{
    spdlog::info("Initializing IRCord Server...");

    // Setup logging
    setup_logging();

    // Initialize libsodium
    if (sodium_init() < 0) {
        throw std::runtime_error("Failed to initialize libsodium");
    }
    spdlog::info("libsodium initialized");

    // Open database
    try {
        db_            = std::make_unique<db::Database>(config_.db_path);
        user_store_    = std::make_unique<db::UserStore>(*db_);
        offline_store_ = std::make_unique<db::OfflineStore>(*db_);
        spdlog::info("Database layer initialized: {}", config_.db_path);
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize database: {}", e.what());
        throw;
    }

    // Create TLS context
    try {
        ssl_ctx_ = net::TlsContextFactory::create_server_context(
            config_.tls_cert_file,
            config_.tls_key_file);
        spdlog::info("TLS context initialized");
    } catch (const std::exception& e) {
        spdlog::error("Failed to create TLS context: {}", e.what());
        throw;
    }

    // Create listener
    listener_ = std::make_unique<net::Listener>(
        ioc_, ssl_ctx_, config_.host, config_.port,
        *user_store_, *offline_store_);

    // Set database for command handler
    listener_->set_database(*db_);

    // Set ping intervals
    listener_->set_ping_intervals(
        config_.ping_interval_sec,
        config_.ping_timeout_sec);

    // Set rate limits and connection cap
    listener_->set_rate_limits(
        config_.msg_rate_per_sec,
        config_.conn_rate_per_min);
    listener_->set_max_connections(config_.max_connections);

    // Set global pointer for signal handler
    g_server = this;
}

Server::~Server() {
    shutdown();
    g_server = nullptr;
}

void Server::setup_logging() {
    try {
        std::vector<spdlog::sink_ptr> sinks;

        // Console sink (colored)
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::trace);
        sinks.push_back(console_sink);

        // File sink (rotating)
        try {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                "ircord-server.log", 1024 * 1024 * 5, 3);  // 5MB, 3 files
            file_sink->set_level(spdlog::level::debug);
            sinks.push_back(file_sink);
        } catch (...) {
            // File logging may fail, continue with console only
        }

        // Create logger
        auto logger = std::make_shared<spdlog::logger>(
            "ircord", sinks.begin(), sinks.end());

        // Set log level from config
        std::string level = config_.log_level;
        if (level == "debug") {
            logger->set_level(spdlog::level::debug);
        } else if (level == "info") {
            logger->set_level(spdlog::level::info);
        } else if (level == "warn" || level == "warning") {
            logger->set_level(spdlog::level::warn);
        } else if (level == "error") {
            logger->set_level(spdlog::level::err);
        } else {
            logger->set_level(spdlog::level::info);
        }

        // Set format
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");

        spdlog::register_logger(logger);
        spdlog::set_default_logger(logger);

        spdlog::info("Logging initialized: level={}", config_.log_level);

    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log init failed: " << ex.what() << std::endl;
    }
}

void Server::create_thread_pool() {
    // Determine thread count (hardware concurrency or 4 minimum)
    unsigned int thread_count = std::max(2u, std::thread::hardware_concurrency());
    spdlog::info("Creating thread pool with {} threads", thread_count);

    thread_pool_.reserve(thread_count);
    for (unsigned int i = 0; i < thread_count; ++i) {
        thread_pool_.emplace_back([this] {
            ioc_.run();
        });
    }
}

void Server::stop_thread_pool() {
    for (auto& thread : thread_pool_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    thread_pool_.clear();
}

void Server::run() {
    if (running_.exchange(true)) {
        spdlog::warn("Server already running");
        return;
    }

    spdlog::info("Starting IRCord Server v0.1.0");

    // Schedule first offline-message cleanup
    cleanup_timer_.emplace(ioc_);
    schedule_cleanup();

    // Install signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

#ifdef SIGBREAK
    std::signal(SIGBREAK, signal_handler);
#endif

    // Keep io_context alive
    work_.emplace(boost::asio::make_work_guard(ioc_));

    // Start listener
    listener_->run();

    // Start thread pool
    create_thread_pool();

    spdlog::info("Server started. Press Ctrl+C to stop.");

    // Wait for shutdown
    // (io_context runs in thread pool, main thread waits for running_ flag)
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    spdlog::info("Server stopped");
}

void Server::shutdown() {
    if (!running_.exchange(false)) {
        return;  // Already shutting down
    }

    spdlog::info("Shutting down...");

    // Stop accepting new connections
    if (listener_) {
        listener_->shutdown();
    }

    // Remove work guard to allow io_context to exit
    work_.reset();

    // Stop io_context
    ioc_.stop();

    // Wait for threads to finish
    stop_thread_pool();

    spdlog::info("Shutdown complete");
}

void Server::schedule_cleanup() {
    cleanup_timer_->expires_after(std::chrono::hours(kCleanupIntervalHours));
    cleanup_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted || !running_) {
            return;
        }
        if (offline_store_) {
            offline_store_->cleanup_expired();
        }
        schedule_cleanup();
    });
}

} // namespace ircord
