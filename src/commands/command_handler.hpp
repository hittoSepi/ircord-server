#pragma once

#include "ircord.pb.h"
#include "net/rate_limiter.hpp"
#include "db/user_store.hpp"
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <mutex>

// Forward declarations
namespace ircord::net { class Session; }
namespace ircord::db { class Database; class OfflineStore; }

namespace ircord::commands {

using SessionPtr = std::shared_ptr<net::Session>;
using SessionFinder = std::function<SessionPtr(const std::string&)>;
using BroadcastFunc = std::function<void(const Envelope&, SessionPtr)>;

// Channel state with operators and settings
struct ChannelState {
    std::string name;
    std::string topic;
    std::unordered_set<std::string> members;      // user_ids
    std::unordered_set<std::string> operators;    // user_ids with @
    std::unordered_set<std::string> voiced;       // user_ids with +
    std::unordered_set<std::string> banned;       // banned user_ids
    bool invite_only = false;
    bool moderated = false;
    bool topic_restricted = true;  // Only ops can change topic by default
    std::unordered_set<std::string> invites;      // Invited users (for invite-only)
};

// IRC Command handler
class CommandHandler {
public:
    CommandHandler(
        SessionFinder find_session,
        BroadcastFunc broadcast,
        db::Database& db,
        db::UserStore& user_store,
        db::OfflineStore& offline_store);

    // Handle an incoming IRC command from a session
    CommandResponse handle_command(const IrcCommand& cmd, SessionPtr session);

    // Channel management
    void join_channel(const std::string& channel, const std::string& user_id);
    void part_channel(const std::string& channel, const std::string& user_id);
    void broadcast_to_channel(const std::string& channel, const Envelope& env, SessionPtr exclude = nullptr);
    
    // Check if user is in channel
    bool is_in_channel(const std::string& channel, const std::string& user_id);
    bool is_operator(const std::string& channel, const std::string& user_id);
    bool is_voiced(const std::string& channel, const std::string& user_id);

    // Getters
    std::vector<std::string> get_channel_members(const std::string& channel);
    std::string get_channel_topic(const std::string& channel);

private:
    // Command implementations
    CommandResponse cmd_join(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_part(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_nick(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_whois(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_me(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_topic(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_kick(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_ban(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_invite(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_set(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_mode(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_password(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_quit(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_msg(const std::vector<std::string>& args, SessionPtr session);
    CommandResponse cmd_resetdb(const std::vector<std::string>& args, SessionPtr session);

    // Helper functions
    ChannelState& get_or_create_channel(const std::string& name);
    void notify_channel_join(const std::string& channel, const std::string& user_id);
    void notify_channel_part(const std::string& channel, const std::string& user_id, const std::string& reason);
    void send_user_list(const std::string& channel, SessionPtr session);
    void broadcast_user_list(const std::string& channel);

    SessionFinder find_session_;
    BroadcastFunc broadcast_;
    db::Database& db_;
    db::UserStore& user_store_;
    db::OfflineStore& offline_store_;

    // Channel state
    std::unordered_map<std::string, ChannelState> channels_;  // name -> state
    std::unordered_map<std::string, std::string> nick_to_user_id_;  // nickname -> user_id
    std::unordered_map<std::string, std::string> user_id_to_nick_;  // user_id -> nickname

    // Command dispatch table
    using CommandFunc = std::function<CommandResponse(const std::vector<std::string>&, SessionPtr)>;
    std::unordered_map<std::string, CommandFunc> command_map_;

    // Rate limiting per user
    struct UserRateLimits {
        net::RateLimiter command_limiter;
        net::RateLimiter join_limiter;
        
        UserRateLimits()
            : command_limiter(30, std::chrono::seconds(60))  // 30 commands/min
            , join_limiter(5, std::chrono::seconds(60))      // 5 joins/min
        {}
    };
    std::unordered_map<std::string, UserRateLimits> user_rate_limits_;
    std::mutex rate_limit_mutex_;
    
    // Abuse tracking
    struct AbuseRecord {
        int violations = 0;
        std::chrono::steady_clock::time_point first_violation;
        std::chrono::steady_clock::time_point last_violation;
        bool banned = false;
    };
    std::unordered_map<std::string, AbuseRecord> abuse_records_;
    std::mutex abuse_mutex_;
    
    static constexpr int kMaxViolations = 5;        // Ban after 5 violations
    static constexpr auto kViolationWindow = std::chrono::minutes(10);  // Within 10 min
    static constexpr auto kBanDuration = std::chrono::minutes(30);      // 30 min ban
    
    bool check_rate_limit(const std::string& user_id, UserRateLimits& limits);
    bool track_abuse(const std::string& user_id);
    bool is_abuser(const std::string& user_id);
    void cleanup_old_abuse_records();
};

} // namespace ircord::commands
