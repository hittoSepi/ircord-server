#pragma once

#include <boost/asio/ssl/context.hpp>
#include <string>

namespace ircord::net {

// Creates a TLS 1.3 server context configured for IRCord
class TlsContextFactory {
public:
    // Create a TLS server context
    // Throws std::runtime_error on certificate load failure
    static boost::asio::ssl::context create_server_context(
        const std::string& cert_file,
        const std::string& key_file);

    // Verify certificate and key files exist and are readable
    // Throws std::runtime_error if files are missing or unreadable
    static void verify_cert_files(
        const std::string& cert_file,
        const std::string& key_file);
};

} // namespace ircord::net
