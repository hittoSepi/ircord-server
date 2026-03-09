#include "tls_context.hpp"

#include <boost/asio/ssl/context.hpp>
#include <fstream>
#include <stdexcept>
#include <openssl/ssl.h>

namespace ircord::net {

namespace {

// Check if a file exists and is readable
bool file_readable(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

} // anonymous namespace

void TlsContextFactory::verify_cert_files(
    const std::string& cert_file,
    const std::string& key_file)
{
    if (!file_readable(cert_file)) {
        throw std::runtime_error("Certificate file not found or unreadable: " + cert_file);
    }

    if (!file_readable(key_file)) {
        throw std::runtime_error("Private key file not found or unreadable: " + key_file);
    }
}

boost::asio::ssl::context TlsContextFactory::create_server_context(
    const std::string& cert_file,
    const std::string& key_file)
{
    // Verify files exist first
    verify_cert_files(cert_file, key_file);

    // Create TLS server context
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);

    // Set TLS 1.3 only (disable older protocols)
    ctx.set_options(
        boost::asio::ssl::context::default_workarounds |
        boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::no_sslv3 |
        boost::asio::ssl::context::no_tlsv1 |
        boost::asio::ssl::context::no_tlsv1_1 |
        boost::asio::ssl::context::no_tlsv1_2
    );

    // Try to set minimum protocol version to TLS 1.3 (OpenSSL 1.1.1+)
    SSL_CTX* ssl_ctx = ctx.native_handle();
    if (ssl_ctx) {
        SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
    }

    // Load certificate chain
    try {
        ctx.use_certificate_chain_file(cert_file);
    } catch (const boost::system::system_error& e) {
        throw std::runtime_error("Failed to load certificate file: " + cert_file + " - " + e.what());
    }

    // Load private key
    try {
        ctx.use_private_key_file(key_file, boost::asio::ssl::context::pem);
    } catch (const boost::system::system_error& e) {
        throw std::runtime_error("Failed to load private key file: " + key_file + " - " + e.what());
    }

    // Restrict cipher suites for TLS 1.3
    // Note: TLS 1.3 has its own cipher suite configuration
    if (ssl_ctx) {
        // TLS 1.3 ciphers (server cannot select in TLS 1.3, but we set for completeness)
        SSL_CTX_set_ciphersuites(ssl_ctx,
            "TLS_AES_256_GCM_SHA384:"
            "TLS_CHACHA20_POLY1305_SHA256:"
            "TLS_AES_128_GCM_SHA256");
    }

    return ctx;
}

} // namespace ircord::net
