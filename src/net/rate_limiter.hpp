#pragma once

#include <chrono>

namespace ircord::net {

// Simple fixed-window rate limiter.
// Not thread-safe: each Session owns one on its strand; Listener uses a mutex.
class RateLimiter {
public:
    RateLimiter(int max_tokens, std::chrono::steady_clock::duration window)
        : max_tokens_(max_tokens)
        , window_(window)
        , window_start_(std::chrono::steady_clock::now())
    {}

    // Returns true if the request is within the allowed rate.
    bool allow() {
        const auto now = std::chrono::steady_clock::now();
        if (now - window_start_ >= window_) {
            window_start_ = now;
            count_ = 0;
        }
        if (count_ >= max_tokens_) {
            return false;
        }
        ++count_;
        return true;
    }

    void reset() {
        count_ = 0;
        window_start_ = std::chrono::steady_clock::now();
    }

private:
    int max_tokens_;
    std::chrono::steady_clock::duration window_;
    std::chrono::steady_clock::time_point window_start_;
    int count_ = 0;
};

} // namespace ircord::net
