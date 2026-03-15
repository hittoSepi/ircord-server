#include "server.hpp"
#include "net/tls_context.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <sodium.h>

#include <iostream>
#include <csignal>

#ifdef IRCORD_HAS_TUI
#include "version.hpp"
#endif

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
        file_store_    = std::make_unique<db::FileStore>(*db_);
        spdlog::info("Database layer initialized: {}", config_.db_path);
        
        // Initialize file encryption if master key is configured
        if (!config_.file_encryption_key.empty()) {
            file_store_->init_encryption(config_.file_encryption_key);
        } else {
            spdlog::warn("File encryption key not configured - files will be stored unencrypted!");
        }
        
        // Initialize virus scanner (optional)
        if (!config_.clamav_socket.empty()) {
            security::VirusScannerManager::instance().initialize(config_.clamav_socket);
        } else if (config_.clamav_port > 0) {
            security::VirusScannerManager::instance().initialize(
                config_.clamav_host, config_.clamav_port);
        } else {
            spdlog::info("VirusScanner: Not configured (set clamav_socket or clamav_host/port)");
        }
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

    // Set database and file store for listener
    listener_->set_database(*db_);
    listener_->set_file_store(*file_store_);

    // Set ping intervals
    listener_->set_ping_intervals(
        config_.ping_interval_sec,
        config_.ping_timeout_sec);

    // Set rate limits and connection cap
    listener_->set_rate_limits(
        config_.msg_rate_per_sec,
        config_.conn_rate_per_min);
    listener_->set_max_connections(config_.max_connections);
    listener_->set_motd(config_.motd);

    // Set global pointer for signal handler
    g_server = this;

    // Create directory client if public listing is enabled
    if (config_.is_public && config_.directory_enabled) {
        directory_client_ = std::make_shared<DirectoryClient>(ioc_, config_);
        spdlog::info("Directory client created for public server listing");
    } else if (config_.is_public && !config_.directory_enabled) {
        spdlog::warn("Server is marked public but directory registration is disabled");
    }
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

    // Start directory client if enabled
    if (directory_client_) {
        directory_client_->start();
    }

#ifdef IRCORD_HTTP_API_ENABLED
    // Start HTTP API server if enabled
    if (config_.http_api_enabled) {
        ircord::api::ServerConfig api_config;
        api_config.port = config_.http_api_port;
        api_config.bind_address = config_.http_api_bind;
        api_config.api_keys = config_.http_api_keys;
        api_config.cors_enabled = config_.http_api_cors;
        api_config.rate_limit_enabled = config_.http_api_rate_limit;
        api_config.rate_limit_requests = config_.http_api_rate_limit_requests;
        
        http_api_server_ = std::make_unique<ircord::api::Server>(ioc_, api_config);
        setup_http_api_routes();
        http_api_server_->start();
        spdlog::info("HTTP API server started on {}:{}", config_.http_api_bind, config_.http_api_port);
    }
#endif

    // Start thread pool
    create_thread_pool();

#ifdef IRCORD_HAS_TUI
    // Record start time for uptime calculation
    start_time_ = std::chrono::steady_clock::now();

    // Start admin socket listener
    admin_listener_ = std::make_shared<tui::AdminSocketListener>(ioc_);
    admin_listener_->set_command_callback([this](const nlohmann::json& cmd) {
        handle_admin_command(cmd);
    });
    admin_listener_->start();

    // Add spdlog sink for TUI
    auto tui_sink = std::make_shared<tui::TuiLogSinkMt>(admin_listener_);
    tui_sink->set_level(spdlog::level::debug);
    spdlog::default_logger()->sinks().push_back(tui_sink);

    // Start periodic state updates to TUI
    state_timer_.emplace(ioc_);
    send_periodic_state();

    // Start TUI in integrated mode (unless headless)
    if (!config_.headless) {
        tui_thread_ = std::make_unique<std::thread>([this] {
            tui::AdminTui tui;  // connects to default admin socket path
            tui.run();
            // TUI exited (Ctrl+D) - server keeps running
            spdlog::info("TUI detached. Server continues running.");
        });
    }
#endif

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

#ifdef IRCORD_HTTP_API_ENABLED
    // Stop HTTP API server
    if (http_api_server_) {
        http_api_server_->stop();
    }
#endif

    // Stop directory client
    if (directory_client_) {
        directory_client_->stop();
    }

#ifdef IRCORD_HAS_TUI
    // Stop TUI admin socket
    if (state_timer_) {
        state_timer_->cancel();
    }
    if (admin_listener_) {
        admin_listener_->stop();
    }
    if (tui_thread_ && tui_thread_->joinable()) {
        tui_thread_->join();
    }
#endif

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

#ifdef IRCORD_HTTP_API_ENABLED
void Server::setup_http_api_routes() {
    if (!http_api_server_) return;
    
    // Health check endpoint (no auth required)
    http_api_server_->get("/health", [](const auto& req) {
        return ircord::api::Response::ok({
            {"status", "ok"},
            {"service", "ircord-server"}
        });
    });
    
    // Server info endpoint
    http_api_server_->get("/api/v1/info", [this](const auto& req) {
        return ircord::api::Response::ok({
            {"name", config_.server_name},
            {"version", "0.1.0"},
            {"public", config_.is_public}
        });
    });
    
    // Bug report submission (public endpoint with rate limiting)
    http_api_server_->post("/api/v1/bug-reports", [this](const auto& req) {
        if (!req.has_json_body()) {
            return ircord::api::Response::bad_request("JSON body required");
        }
        
        auto body = req.json_body();
        
        if (!body.contains("description")) {
            return ircord::api::Response::bad_request("Missing 'description' field");
        }
        
        std::string description = body["description"];
        std::string user_id = body.value("user_id", "anonymous");
        
        if (description.length() < 10) {
            return ircord::api::Response::bad_request("Description too short (min 10 chars)");
        }
        
        if (description.length() > 2000) {
            return ircord::api::Response::bad_request("Description too long (max 2000 chars)");
        }
        
        // TODO: Store bug report in database
        // For now, just log it
        spdlog::info("Bug report from {}: {}", user_id, description.substr(0, 50));
        
        return ircord::api::Response::created({
            {"id", 1},
            {"message", "Bug report submitted successfully"}
        });
    });
    
    // User count endpoint
    http_api_server_->get("/api/v1/stats", [this](const auto& req) {
        // TODO: Add actual stats when available
        return ircord::api::Response::ok({
            {"online_users", 0},
            {"channels", 0}
        });
    });
    
    spdlog::info("HTTP API routes registered");
}
#endif

void Server::schedule_cleanup() {
    cleanup_timer_->expires_after(std::chrono::hours(kCleanupIntervalHours));
    cleanup_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted || !running_) {
            return;
        }
        if (offline_store_) {
            offline_store_->cleanup_expired();
        }
        if (file_store_) {
            int cleaned = file_store_->cleanupExpired();
            if (cleaned > 0) {
                spdlog::info("Cleaned up {} expired files", cleaned);
            }
        }
        schedule_cleanup();
    });
}

#ifdef IRCORD_HAS_TUI
void Server::handle_admin_command(const nlohmann::json& cmd) {
    if (!cmd.contains("cmd")) return;

    std::string command = cmd["cmd"];
    auto params = cmd.value("params", nlohmann::json::object());

    if (command == "kick") {
        std::string user_id = params.value("user_id", "");
        std::string reason = params.value("reason", "Kicked by admin");
        if (!user_id.empty() && listener_) {
            auto session = listener_->find_session(user_id);
            if (session) {
                session->disconnect(reason);
                spdlog::info("Admin kicked user: {}", user_id);
            } else {
                spdlog::warn("Admin kick: user {} not found", user_id);
            }
        }
    } else if (command == "ban") {
        std::string user_id = params.value("user_id", "");
        std::string reason = params.value("reason", "Banned by admin");
        if (!user_id.empty() && listener_) {
            auto session = listener_->find_session(user_id);
            if (session) {
                session->disconnect(reason);
            }
            spdlog::info("Admin banned user: {}", user_id);
        }
    } else if (command == "set_config") {
        std::string key = params.value("key", "");
        std::string value = params.value("value", "");
        if (listener_) {
            if (key == "max_connections") {
                int val = std::stoi(value);
                listener_->set_max_connections(val);
                spdlog::info("Admin set max_connections = {}", val);
            } else if (key == "msg_rate_per_sec") {
                int val = std::stoi(value);
                listener_->set_rate_limits(val, -1);
                spdlog::info("Admin set msg_rate_per_sec = {}", val);
            } else if (key == "motd") {
                listener_->set_motd(value);
                spdlog::info("Admin set MOTD = {}", value);
            } else {
                spdlog::warn("Admin set_config: unknown key '{}'", key);
            }
        }
    } else if (command == "subscribe") {
        // Acknowledge — events are sent to all connected TUI clients
        spdlog::debug("Admin TUI subscribed to events");
    } else {
        spdlog::warn("Unknown admin command: {}", command);
    }
}

void Server::send_periodic_state() {
    state_timer_->expires_after(std::chrono::seconds(2));
    state_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec || !running_) return;
        if (admin_listener_ && admin_listener_->has_client() && listener_) {
            // Send users
            auto online_users = listener_->get_online_users();
            nlohmann::json users_json = nlohmann::json::array();
            for (const auto& [uid, endpoint] : online_users) {
                users_json.push_back({
                    {"id", uid},
                    {"ip", endpoint},
                    {"nickname", uid},
                    {"connected", ""}
                });
            }
            admin_listener_->send_event({
                {"type", "users"},
                {"data", users_json}
            });

            // Send stats
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time_).count();
            admin_listener_->send_event({
                {"type", "stats"},
                {"data", {
                    {"uptime", uptime},
                    {"connections", listener_->active_connection_count()},
                    {"version", std::string(ircord::kProjectVersion)}
                }}
            });
        }
        send_periodic_state();
    });
}
#endif

} // namespace ircord
