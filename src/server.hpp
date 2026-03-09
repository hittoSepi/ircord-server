#pragma once

#include "config.hpp"
#include "net/listener.hpp"

#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
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

		// Listener
		std::unique_ptr<net::Listener> listener_;

		// Running state
		std::atomic<bool> running_ { false };

		// Work guard to keep io_context running
		std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;
	};

} // namespace ircord
