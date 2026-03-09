
#include "config.hpp"
#include "server.hpp"
#include "utils/string_utils.hpp"
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>



namespace {

	using namespace ircord;


	const std::string &default_config_path = "./server.toml";

	utils::template_map template_strings {
		{"program_name", "IRCord-Server" },
		{"program_description", "End-to-end encrypted chat & voice server" },
		{"version_major", "0" },
		{"version_minor", "01.0" },
		{"git_url", "https://github.com/hittoSepi/ircord" },
		{"config_path",  default_config_path },
		{"args", "" }
	};

	const std::string &version_string = R"(
	${program_name} vv.${version_major}.${version_minor}
	${program_description}
	${git_url}	
	)";
	
	const std::string &usage_string = R"(
	${program_description} v.${version_major}.${version_minor}

	Usage: 
		${args} [options]

		Options:
		  --config <path>   Path to configuration file (default: ./config/server.toml)
		  --help, -h        Show this help message
		  --version, -V     Show version information

	Example:
		${args} --config ./server.toml

	)";	  


	void print_usage(const char* args) {
		template_strings[std::string("args")] = args;
		std::cout << utils::format_template( usage_string, template_strings );
		template_strings[std::string( "args" )] = "";
	}

	void print_version() {
		std::cout << utils::format_template( version_string, template_strings );
	}

	std::string get_default_config_path() {
		// Check for config in various locations
		const char *env_config = std::getenv( "IRCORD_CONFIG" );
		if ( env_config && env_config[0] != '\0' ) {
			return env_config;
		}
		return default_config_path;
	}

} // anonymous namespace



int main( int argc, char *argv[] ) {

	std::string config_path = get_default_config_path();
	std::cout << config_path << std::endl;

	// Parse command line arguments
	for ( int i = 1; i < argc; ++i ) {
		std::string arg = argv[i];

		if ( arg == "--help" || arg == "-h" ) {
			print_usage( argv[0] );
			return 0;
		}

		if ( arg == "--version" || arg == "-V" ) {
			print_version();
			return 0;
		}

		if ( arg == "--config" ) {
			if ( i + 1 < argc ) {
				config_path = argv[++i];
			} else {
				std::cerr << "Error: --config requires a path argument\n";
				print_usage( argv[0] );
				return 1;
			}
		} else {
			std::cerr << "Error: Unknown argument: " << arg << "\n";
			print_usage( argv[0] );
			return 1;
		}
	}

	try {
		// Load configuration
		ircord::ServerConfig config;
		try {
			config = ircord::ConfigLoader::load( config_path );
		}
		catch ( const std::exception &e ) {
			std::cerr << "Failed to load config from " << config_path << ": " << e.what() << "\n";


			// Try to load with defaults
			std::cout << "Attempting to use default configuration...\n";
			config = ircord::ConfigLoader::load_or_default( config_path );

			if ( config.tls_cert_file.empty() || config.tls_key_file.empty() ) {
				std::cerr << "\nError: TLS certificate and key files must be specified.\n"
					<< "Please create a config file or ensure ./certs/server.crt and ./certs/server.key exist.\n"
					<< "\nTo generate a self-signed certificate for testing:\n"
					<< "  mkdir -p certs\n"
					<< "  openssl req -x509 -newkey ed25519 -keyout certs/server.key \\\n"
					<< "    -out certs/server.crt -days 365 -nodes -subj \"/CN=localhost\"\n";
				return 1;
			}
		}

		// Validate configuration
		ircord::ConfigLoader::validate( config );

		// Create and run server
		ircord::Server server( config );
		server.run();

		return 0;

	}
	catch ( const std::exception &e ) {
		std::cerr << "Fatal error: " << e.what() << "\n";
		return 1;
	}
	catch ( ... ) {
		std::cerr << "Unknown fatal error\n";
		return 1;
	}
}
