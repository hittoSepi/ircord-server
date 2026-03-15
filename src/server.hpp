#pragma once

#include "config.hpp"
#include "net/listener.hpp"
#include "net/directory_client.hpp"
#include "db/database.hpp"
#include "db/user_store.hpp"
#include "db/offline_store.hpp"
#include "db/file_store.hpp"
#include "security/virus_scanner.hpp"

// HTTP API Server (optional)
#ifdef IRCORD_HTTP_API_ENABLED
#include <ircord/api/server.hpp>
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

		// Running state
		std::atomic<bool> running_ { false };

		// Work guard to keep io_context running
		std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;

		// Periodic cleanup timer (offline message expiry)
		std::optional<boost::asio::steady_timer> cleanup_timer_;
		void schedule_cleanup();
		static constexpr int kCleanupIntervalHours = 1;
	};

} // namespace ircord
