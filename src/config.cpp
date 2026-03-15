#include "config.hpp"


namespace ircord {

	namespace {

		// Helper to get required string field
		template<typename T>
		T get_required( const toml::value &data, const std::string &key ) {
			try {
				// toml11 v4.x API: use toml::find() instead of toml::get()
				return toml::find<T>( data, key );
			}
			catch ( const std::out_of_range & ) {
				std::ostringstream oss;
				oss << "Missing required config field: " << key;
				throw std::runtime_error( oss.str() );
			}
			catch ( const toml::type_error &e ) {
				std::ostringstream oss;
				oss << "Invalid type for config field " << key << ": " << e.what();
				throw std::runtime_error( oss.str() );
			}
		}

		// Helper to get optional field with default
		template<typename T>
		T get_optional( const toml::value &data, const std::string &key, const T &default_val ) {
			try {
				// toml11 v4.x API: use toml::find() instead of toml::get()
				return toml::find<T>( data, key );
			}
			catch ( const std::out_of_range & ) {
				return default_val;
			}
			catch ( const toml::type_error & ) {
				return default_val;
			}
		}

	} // anonymous namespace




	ServerConfig ConfigLoader::load( const std::string &config_path ) {

		std::filesystem::path cp = std::filesystem::path( config_path );
		std::string _path = cp.string();

		if ( !ircord::file_exists( _path ) ) {
			std::cout << "Creating new config file.." << std::endl;
			ircord::create_config_file( _path );
		}


		const auto data = toml::parse( _path );

		ServerConfig config;

		// [server] section - required
		if ( data.contains( "server" ) ) {
			const auto &server = data.at( "server" );
			config.host = get_required<std::string>( server, "host" );
			config.port = static_cast<uint16_t>( get_required<int64_t>( server, "port" ) );
			config.log_level = get_optional<std::string>( server, "log_level", "info" );
			config.max_connections = get_optional<int>( server, "max_connections", 100 );
			config.is_public = get_optional<bool>( server, "public", false );
		} else {
			throw std::runtime_error( "Missing required [server] section in config" );
		}

		// [tls] section - required
		if ( data.contains( "tls" ) ) {
			const auto &tls = data.at( "tls" );
			config.tls_cert_file = get_required<std::string>( tls, "cert_file" );
			config.tls_key_file = get_required<std::string>( tls, "key_file" );
		} else {
			throw std::runtime_error( "Missing required [tls] section in config" );
		}

		// [database] section - optional
		if ( data.contains( "database" ) ) {
			const auto &database = data.at( "database" );
			config.db_path = get_optional<std::string>( database, "path", "./ircord.db" );
		}

		// [limits] section - optional
		if ( data.contains( "limits" ) ) {
			const auto &limits = data.at( "limits" );
			config.max_message_bytes = static_cast<size_t>(
				get_optional<int64_t>( limits, "max_message_bytes", 65536 ) );
			config.ping_interval_sec  = get_optional<int>( limits, "ping_interval_sec", 30 );
			config.ping_timeout_sec   = get_optional<int>( limits, "ping_timeout_sec", 60 );
			config.msg_rate_per_sec   = get_optional<int>( limits, "msg_rate_per_sec", 20 );
			config.conn_rate_per_min  = get_optional<int>( limits, "conn_rate_per_min", 10 );
			config.commands_per_min   = get_optional<int>( limits, "commands_per_min", 30 );
			config.joins_per_min      = get_optional<int>( limits, "joins_per_min", 5 );
			config.abuse_threshold    = get_optional<int>( limits, "abuse_threshold", 5 );
			config.abuse_window_min   = get_optional<int>( limits, "abuse_window_min", 10 );
			config.ban_duration_min   = get_optional<int>( limits, "ban_duration_min", 30 );
		}

		// [security] section - optional
		if ( data.contains( "security" ) ) {
			const auto &security = data.at( "security" );
			config.file_encryption_key = get_optional<std::string>( security, "file_encryption_key", "" );
		}

		// [antivirus] section - optional
		if ( data.contains( "antivirus" ) ) {
			const auto &av = data.at( "antivirus" );
			config.clamav_socket = get_optional<std::string>( av, "clamav_socket", "" );
			config.clamav_host = get_optional<std::string>( av, "clamav_host", "127.0.0.1" );
			config.clamav_port = static_cast<uint16_t>(
				get_optional<int64_t>( av, "clamav_port", 0 ) );
		}

		// [motd] section - optional
		if ( data.contains( "motd" ) ) {
			const auto &motd_val = data.at( "motd" );
			if ( motd_val.is_array() ) {
				// Array of strings for multiline MOTD
				const auto &lines = motd_val.as_array();
				std::ostringstream oss;
				for ( size_t i = 0; i < lines.size(); ++i ) {
					if ( i > 0 ) oss << "\n";
					oss << lines[i].as_string();
				}
				config.motd = oss.str();
			} else if ( motd_val.is_string() ) {
				// Single string
				config.motd = motd_val.as_string();
			}
		}

		// [directory] section - optional
		if ( data.contains( "directory" ) ) {
			const auto &dir = data.at( "directory" );
			config.directory_enabled = get_optional<bool>( dir, "enabled", false );
			config.directory_url = get_optional<std::string>( dir, "url", "https://directory.ircord.dev" );
			config.directory_ping_interval_sec = get_optional<int>( dir, "ping_interval_sec", 300 );
			config.server_name = get_optional<std::string>( dir, "server_name", "" );
			config.server_description = get_optional<std::string>( dir, "description", "" );
		}

		// [http_api] section - optional
		if ( data.contains( "http_api" ) ) {
			const auto &http = data.at( "http_api" );
			config.http_api_enabled = get_optional<bool>( http, "enabled", false );
			config.http_api_port = static_cast<uint16_t>( get_optional<int64_t>( http, "port", 8080 ) );
			config.http_api_bind = get_optional<std::string>( http, "bind_address", "127.0.0.1" );
			config.http_api_cors = get_optional<bool>( http, "cors_enabled", true );
			config.http_api_rate_limit = get_optional<bool>( http, "rate_limit_enabled", true );
			config.http_api_rate_limit_requests = get_optional<int>( http, "rate_limit_requests", 60 );
			
			// Parse api_keys array
			if ( http.contains( "api_keys" ) ) {
				const auto &keys = http.at( "api_keys" );
				if ( keys.is_array() ) {
					for ( const auto &key : keys.as_array() ) {
						if ( key.is_string() ) {
							config.http_api_keys.push_back( key.as_string() );
						}
					}
				}
			}
		}

		return config;
	}

	ServerConfig ConfigLoader::load_or_default( const std::string &config_path ) {
		try {
			return load( config_path );
		}
		catch ( const std::runtime_error &e ) {
			// File doesn't exist or parse error - return defaults
			// Note: TLS files still need to be set before use
			ServerConfig config;
			config.tls_cert_file = "./certs/server.crt";
			config.tls_key_file = "./certs/server.key";
			return config;
		}
	}

	void ConfigLoader::validate( const ServerConfig &config ) {
		if ( config.host.empty() ) {
			throw std::runtime_error( "Invalid config: host cannot be empty" );
		}

		if ( config.port == 0 ) {
			throw std::runtime_error( "Invalid config: port cannot be zero" );
		}

		if ( config.tls_cert_file.empty() ) {
			throw std::runtime_error( "Invalid config: tls.cert_file cannot be empty" );
		}

		if ( config.tls_key_file.empty() ) {
			throw std::runtime_error( "Invalid config: tls.key_file cannot be empty" );
		}

		if ( config.max_message_bytes == 0 || config.max_message_bytes > 1024 * 1024 ) {
			throw std::runtime_error( "Invalid config: max_message_bytes must be between 1 and 1MB" );
		}

		if ( config.ping_interval_sec <= 0 || config.ping_interval_sec > 300 ) {
			throw std::runtime_error( "Invalid config: ping_interval_sec must be between 1 and 300" );
		}

		if ( config.ping_timeout_sec <= 0 || config.ping_timeout_sec > 600 ) {
			throw std::runtime_error( "Invalid config: ping_timeout_sec must be between 1 and 600" );
		}
	}

} // namespace ircord
