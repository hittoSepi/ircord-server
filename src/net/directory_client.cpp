#include "directory_client.hpp"
#include <spdlog/spdlog.h>

namespace ircord {

using boost::asio::ip::tcp;

// Simple JSON string escaping
std::string escape_json(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default: o << c;
        }
    }
    return o.str();
}

DirectoryClient::DirectoryClient(boost::asio::io_context& io_context, const ServerConfig& config)
    : io_context_(io_context)
    , config_(config) {
    
    // Parse directory URL
    std::string url = config.directory_url;
    if (url.empty()) {
        url = "https://directory.ircord.dev";
    }
    
    // Simple URL parsing
    if (url.substr(0, 8) == "https://") {
        directory_use_ssl_ = true;
        directory_port_ = 443;
        url = url.substr(8);
    } else if (url.substr(0, 7) == "http://") {
        directory_use_ssl_ = false;
        directory_port_ = 80;
        url = url.substr(7);
    } else {
        directory_use_ssl_ = true;
        directory_port_ = 443;
    }
    
    // Extract host and optional port/path
    size_t port_pos = url.find(':');
    size_t path_pos = url.find('/');
    
    if (port_pos != std::string::npos && (path_pos == std::string::npos || port_pos < path_pos)) {
        directory_host_ = url.substr(0, port_pos);
        size_t port_end = (path_pos != std::string::npos) ? path_pos : url.length();
        try {
            directory_port_ = static_cast<uint16_t>(std::stoi(url.substr(port_pos + 1, port_end - port_pos - 1)));
        } catch (...) {
            directory_port_ = directory_use_ssl_ ? 443 : 80;
        }
    } else {
        directory_host_ = (path_pos != std::string::npos) ? url.substr(0, path_pos) : url;
    }
    
    if (path_pos != std::string::npos) {
        directory_path_ = url.substr(path_pos);
    } else {
        directory_path_ = "";
    }
    
    // Enable only if public and directory is enabled
    enabled_ = config.is_public && config.directory_enabled;
    
    if (enabled_) {
        spdlog::info("Directory client enabled for {}:{}", directory_host_, directory_port_);
    }
}

DirectoryClient::~DirectoryClient() {
    stop();
}

void DirectoryClient::start() {
    if (!enabled_ || running_) {
        return;
    }
    
    running_ = true;
    spdlog::info("Starting directory client for public server listing");
    
    // Register with directory
    register_server();
}

void DirectoryClient::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    if (timer_) {
        timer_->cancel();
    }
    
    spdlog::info("Directory client stopped");
}

void DirectoryClient::register_server() {
    if (!running_) return;
    
    std::string name = config_.server_name.empty() ? "IRCord Server" : config_.server_name;
    std::string desc = config_.server_description.empty() ? 
        "An IRCord encrypted chat server" : config_.server_description;
    
    std::ostringstream json;
    json << "{";
    json << "\"host\":\"" << escape_json(config_.host) << "\",";
    json << "\"port\":" << config_.port << ",";
    json << "\"name\":\"" << escape_json(name) << "\",";
    json << "\"description\":\"" << escape_json(desc) << "\"";
    json << "}";
    
    post_request("/api/servers/register", json.str(), 
        [this, self = shared_from_this()](bool success, const std::string& response) {
            if (!running_) return;
            
            if (success) {
                // Parse server_id from JSON response (simple parsing)
                size_t id_pos = response.find("\"server_id\"");
                if (id_pos != std::string::npos) {
                    size_t colon_pos = response.find(':', id_pos);
                    size_t quote_start = response.find('"', colon_pos);
                    if (quote_start != std::string::npos) {
                        size_t quote_end = response.find('"', quote_start + 1);
                        if (quote_end != std::string::npos) {
                            server_id_ = response.substr(quote_start + 1, quote_end - quote_start - 1);
                            registered_ = true;
                            spdlog::info("Registered with directory service, server_id: {}", server_id_);
                        }
                    }
                }
                
                if (!registered_) {
                    last_error_ = "Registration response missing server_id";
                    spdlog::warn("Directory registration failed: {}", last_error_);
                }
                
                // Start periodic pings
                schedule_next_ping();
            } else {
                last_error_ = response;
                spdlog::warn("Directory registration failed: {}", last_error_);
                // Retry after delay
                schedule_next_ping();
            }
        });
}

void DirectoryClient::send_ping() {
    if (!running_ || !registered_) {
        schedule_next_ping();
        return;
    }
    
    std::ostringstream json;
    json << "{\"server_id\":\"" << escape_json(server_id_) << "\"}";
    
    post_request("/api/servers/ping", json.str(),
        [this, self = shared_from_this()](bool success, const std::string& response) {
            if (!running_) return;
            
            if (success) {
                spdlog::debug("Directory ping successful");
            } else {
                spdlog::warn("Directory ping failed: {}", response);
                // Check if we need to re-register
                if (response.find("not found") != std::string::npos || 
                    response.find("Invalid server") != std::string::npos) {
                    spdlog::info("Server not found in directory, re-registering...");
                    registered_ = false;
                    server_id_.clear();
                }
            }
            schedule_next_ping();
        });
}

void DirectoryClient::schedule_next_ping() {
    if (!running_) return;
    
    if (!timer_) {
        timer_ = std::make_unique<boost::asio::steady_timer>(io_context_);
    }
    
    timer_->expires_after(std::chrono::seconds(config_.directory_ping_interval_sec));
    timer_->async_wait([this, self = shared_from_this()](boost::system::error_code ec) {
        if (ec || !running_) return;
        
        if (registered_) {
            send_ping();
        } else {
            register_server();
        }
    });
}

void DirectoryClient::post_request(const std::string& path, const std::string& json_body,
                                   std::function<void(bool, const std::string&)> callback) {
    auto resolver = std::make_shared<tcp::resolver>(io_context_);
    
    resolver->async_resolve(directory_host_, std::to_string(directory_port_),
        [this, self = shared_from_this(), path, json_body, callback, resolver](
            boost::system::error_code ec, tcp::resolver::results_type results) {
            
            if (ec) {
                callback(false, "DNS resolution failed: " + ec.message());
                return;
            }
            
            auto socket = std::make_shared<tcp::socket>(io_context_);
            boost::asio::async_connect(*socket, results,
                [this, self, path, json_body, callback, socket](boost::system::error_code ec, const tcp::endpoint&) {
                    
                    if (ec) {
                        callback(false, "Connection failed: " + ec.message());
                        return;
                    }
                    
                    // Build HTTP request
                    std::string full_path = directory_path_ + path;
                    std::ostringstream request;
                    request << "POST " << full_path << " HTTP/1.1\r\n";
                    request << "Host: " << directory_host_ << "\r\n";
                    request << "Content-Type: application/json\r\n";
                    request << "Content-Length: " << json_body.length() << "\r\n";
                    request << "Connection: close\r\n";
                    request << "\r\n";
                    request << json_body;
                    
                    auto request_str = std::make_shared<std::string>(request.str());
                    boost::asio::async_write(*socket, boost::asio::buffer(*request_str),
                        [this, self, callback, socket, request_str](boost::system::error_code ec, size_t) {
                            
                            if (ec) {
                                callback(false, "Request write failed: " + ec.message());
                                return;
                            }
                            
                            auto response = std::make_shared<std::string>();
                            auto buffer = std::make_shared<std::vector<char>>(4096);
                            
                            std::function<void()> do_read;
                            do_read = [this, self, callback, socket, response, buffer, &do_read]() {
                                socket->async_read_some(boost::asio::buffer(*buffer),
                                    [this, self, callback, socket, response, buffer, &do_read](
                                        boost::system::error_code ec, size_t bytes_transferred) {
                                        
                                        if (!ec) {
                                            response->append(buffer->data(), bytes_transferred);
                                            do_read();
                                        } else if (ec == boost::asio::error::eof) {
                                            // Parse response
                                            size_t body_start = response->find("\r\n\r\n");
                                            if (body_start != std::string::npos) {
                                                std::string body = response->substr(body_start + 4);
                                                size_t status_pos = response->find(" ");
                                                if (status_pos != std::string::npos) {
                                                    try {
                                                        int status_code = std::stoi(response->substr(status_pos + 1, 3));
                                                        callback(status_code >= 200 && status_code < 300, body);
                                                    } catch (...) {
                                                        callback(false, "Invalid HTTP status code");
                                                    }
                                                } else {
                                                    callback(false, "Invalid HTTP response");
                                                }
                                            } else {
                                                callback(false, "Invalid HTTP response");
                                            }
                                        } else {
                                            callback(false, "Read failed: " + ec.message());
                                        }
                                    });
                            };
                            do_read();
                        });
                });
        });
}

} // namespace ircord
