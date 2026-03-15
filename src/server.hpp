#pragma once

#include "config.hpp"
#include "net/listener.hpp"
#include "net/directory_client.hpp"
#include "db/database.hpp"
#include "db/user_store.hpp"
#include "db/offline_store.hpp"
#include "db/file_store.hpp"
#include "security/virus_scanner.hpp"
#include "admin/reserved_identity.hpp"
#include "admin/server_owner.hpp"

// HTTP API Server (optional)
#ifdef IRCORD_HTTP_API_ENABLED
#include <ircord/api/server.hpp>
#endif

// TUI (optional)
#ifdef IRCORD_HAS_TUI
#include <ircord/tui/admin_socket_listener.hpp>
#include <ircord/tui/admin_tui.hpp>
#include <ircord/tui/tui_log_sink.hpp>
#endif

#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace ircord {

	// Main server class that owns io_context and coordinates startup/shutdown
	class Server {
	public:
		explicit Server( const ServerConfig &config );
		~Server();

		// Start the server (blocking call)
		void run();

		// Initiate graceful shutdown
		void shutdown();

		// Check if server is running
		bool is_running() const {
			return running_;
		}
		
		// Get server owner (admin) identity
		admin::ServerOwner* server_owner() { return server_owner_.get(); }
		const admin::ServerOwner* server_owner() const { return server_owner_.get(); }
		
		// Get server config
		const ServerConfig& config() const { return config_; }

	private:
		void setup_logging();
		void create_thread_pool();
		void stop_thread_pool();

		const ServerConfig &config_;

		// Boost.Asio I/O context
		boost::asio::io_context ioc_;

		// Thread pool
		std::vector<std::thread> thread_pool_;

		// TLS context
		boost::asio::ssl::context ssl_ctx_;

		// Database layer
		std::unique_ptr<db::Database>     db_;
		std::unique_ptr<db::UserStore>    user_store_;
		std::unique_ptr<db::OfflineStore> offline_store_;
		std::unique_ptr<db::FileStore>    file_store_;

		// Listener
		std::unique_ptr<net::Listener> listener_;

		// Directory client (for public server listing)
		std::shared_ptr<DirectoryClient> directory_client_;

		// HTTP API server (optional)
#ifdef IRCORD_HTTP_API_ENABLED
		std::unique_ptr<ircord::api::Server> http_api_server_;
		void setup_http_api_routes();
#endif

		// Server owner/admin identity (always online)
		std::unique_ptr<admin::ServerOwner> server_owner_;

		// Running state
		std::atomic<bool> running_ { false };

		// Work guard to keep io_context running
		std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;

		// Periodic cleanup timer (offline message expiry)
		std::optional<boost::asio::steady_timer> cleanup_timer_;
		void schedule_cleanup();
		static constexpr int kCleanupIntervalHours = 1;

#ifdef IRCORD_HAS_TUI
		std::shared_ptr<tui::AdminSocketListener> admin_listener_;
		std::unique_ptr<std::thread> tui_thread_;
		std::optional<boost::asio::steady_timer> state_timer_;
		std::chrono::steady_clock::time_point start_time_;

		void handle_admin_command(const nlohmann::json& cmd);
		void send_periodic_state();
#endif
	};

} // namespace ircord
