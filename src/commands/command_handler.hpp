#pragma once

#include "ircord.pb.h"
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward declarations
namespace ircord::net { class Session; }
namespace ircord::db { class Database; }

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
        db::Database& db);

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

    // Helper functions
    ChannelState& get_or_create_channel(const std::string& name);
    void notify_channel_join(const std::string& channel, const std::string& user_id);
    void notify_channel_part(const std::string& channel, const std::string& user_id, const std::string& reason);
    void send_user_list(const std::string& channel, SessionPtr session);

    SessionFinder find_session_;
    BroadcastFunc broadcast_;
    db::Database& db_;

    // Channel state
    std::unordered_map<std::string, ChannelState> channels_;  // name -> state
    std::unordered_map<std::string, std::string> nick_to_user_id_;  // nickname -> user_id
    std::unordered_map<std::string, std::string> user_id_to_nick_;  // user_id -> nickname

    // Command dispatch table
    using CommandFunc = std::function<CommandResponse(const std::vector<std::string>&, SessionPtr)>;
    std::unordered_map<std::string, CommandFunc> command_map_;
};

} // namespace ircord::commands
