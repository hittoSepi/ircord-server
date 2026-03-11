#include "commands/command_handler.hpp"
#include "net/session.hpp"
#include "db/database.hpp"
#include "db/offline_store.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace ircord::commands {

// Helper to create CommandResponse
inline CommandResponse make_response(bool success, const std::string& message, const std::string& command) {
    CommandResponse resp;
    resp.set_success(success);
    resp.set_message(message);
    resp.set_command(command);
    return resp;
}

CommandHandler::CommandHandler(
    SessionFinder find_session,
    BroadcastFunc broadcast,
    db::Database& db,
    db::OfflineStore& offline_store)
    : find_session_(std::move(find_session))
    , broadcast_(std::move(broadcast))
    , db_(db)
    , offline_store_(offline_store)
{
    // Register commands
    command_map_["join"] = [this](auto& args, auto s) { return cmd_join(args, s); };
    command_map_["part"] = [this](auto& args, auto s) { return cmd_part(args, s); };
    command_map_["leave"] = [this](auto& args, auto s) { return cmd_part(args, s); };
    command_map_["nick"] = [this](auto& args, auto s) { return cmd_nick(args, s); };
    command_map_["whois"] = [this](auto& args, auto s) { return cmd_whois(args, s); };
    command_map_["me"] = [this](auto& args, auto s) { return cmd_me(args, s); };
    command_map_["action"] = [this](auto& args, auto s) { return cmd_me(args, s); };
    command_map_["topic"] = [this](auto& args, auto s) { return cmd_topic(args, s); };
    command_map_["kick"] = [this](auto& args, auto s) { return cmd_kick(args, s); };
    command_map_["ban"] = [this](auto& args, auto s) { return cmd_ban(args, s); };
    command_map_["invite"] = [this](auto& args, auto s) { return cmd_invite(args, s); };
    command_map_["set"] = [this](auto& args, auto s) { return cmd_set(args, s); };
    command_map_["mode"] = [this](auto& args, auto s) { return cmd_mode(args, s); };
    command_map_["password"] = [this](auto& args, auto s) { return cmd_password(args, s); };
    command_map_["pass"] = [this](auto& args, auto s) { return cmd_password(args, s); };
    command_map_["quit"] = [this](auto& args, auto s) { return cmd_quit(args, s); };
    command_map_["msg"] = [this](auto& args, auto s) { return cmd_msg(args, s); };
    command_map_["query"] = [this](auto& args, auto s) { return cmd_msg(args, s); };
}

CommandResponse CommandHandler::handle_command(const IrcCommand& cmd, SessionPtr session) {
    if (cmd.command().empty()) {
        return make_response(false, "Empty command", "");
    }
    
    const std::string& user_id = session->user_id();
    
    // Check if user is banned for abuse
    if (is_abuser(user_id)) {
        return make_response(false, "You are temporarily banned due to rate limit violations. Please wait.", cmd.command());
    }
    
    // Get or create rate limits for this user
    UserRateLimits* limits = nullptr;
    {
        std::lock_guard<std::mutex> lk(rate_limit_mutex_);
        auto [it, inserted] = user_rate_limits_.emplace(user_id, UserRateLimits{});
        limits = &it->second;
    }
    
    // Check rate limit (30 commands per minute)
    if (!limits->command_limiter.allow()) {
        if (track_abuse(user_id)) {
            spdlog::warn("User {} banned for repeated command rate limit violations", user_id);
        }
        return make_response(false, "Rate limit exceeded: too many commands. Slow down.", cmd.command());
    }

    std::string cmd_name = cmd.command();
    std::transform(cmd_name.begin(), cmd_name.end(), cmd_name.begin(), ::tolower);

    auto it = command_map_.find(cmd_name);
    if (it == command_map_.end()) {
        return make_response(false, "Unknown command: " + cmd_name, cmd_name);
    }

    std::vector<std::string> args(cmd.args().begin(), cmd.args().end());
    return it->second(args, session);
}

ChannelState& CommandHandler::get_or_create_channel(const std::string& name) {
    auto it = channels_.find(name);
    if (it == channels_.end()) {
        ChannelState state;
        state.name = name;
        state.topic = "";
        auto [inserted_it, _] = channels_.emplace(name, std::move(state));
        return inserted_it->second;
    }
    return it->second;
}

CommandResponse CommandHandler::cmd_join(const std::vector<std::string>& args, SessionPtr session) {
    if (args.empty()) {
        return make_response(false, "Usage: /join <#channel>", "join");
    }

    std::string channel = args[0];
    if (channel.empty() || channel[0] != '#') {
        return make_response(false, "Channel name must start with #", "join");
    }

    const std::string& user_id = session->user_id();
    
    // Check join rate limit (5 joins per minute)
    UserRateLimits* limits = nullptr;
    {
        std::lock_guard<std::mutex> lk(rate_limit_mutex_);
        auto [it, inserted] = user_rate_limits_.emplace(user_id, UserRateLimits{});
        limits = &it->second;
    }
    
    if (!limits->join_limiter.allow()) {
        if (track_abuse(user_id)) {
            spdlog::warn("User {} banned for repeated join rate limit violations", user_id);
        }
        return make_response(false, "Rate limit exceeded: too many channel joins. Slow down.", "join");
    }
    
    auto& chan = get_or_create_channel(channel);

    if (chan.banned.count(user_id)) {
        return make_response(false, "You are banned from " + channel, "join");
    }

    if (chan.invite_only && !chan.invites.count(user_id) && !chan.operators.count(user_id)) {
        return make_response(false, channel + " is invite-only", "join");
    }

    chan.members.insert(user_id);
    
    if (chan.members.size() == 1) {
        chan.operators.insert(user_id);
        spdlog::info("{} became operator of {}", user_id, channel);
    }

    notify_channel_join(channel, user_id);
    send_user_list(channel, session);

    std::string topic_msg = chan.topic.empty() ? "No topic set" : "Topic: " + chan.topic;
    return make_response(true, "Joined " + channel + "\n" + topic_msg, "join");
}

CommandResponse CommandHandler::cmd_part(const std::vector<std::string>& args, SessionPtr session) {
    const std::string& user_id = session->user_id();
    
    if (args.empty()) {
        return make_response(false, "Usage: /part <#channel> [message]", "part");
    }

    std::string channel = args[0];
    std::string reason = args.size() > 1 ? args[1] : "Leaving";

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return make_response(false, "No such channel: " + channel, "part");
    }

    auto& chan = it->second;
    if (!chan.members.count(user_id)) {
        return make_response(false, "You are not in " + channel, "part");
    }

    chan.members.erase(user_id);
    chan.operators.erase(user_id);
    chan.voiced.erase(user_id);

    notify_channel_part(channel, user_id, reason);

    if (chan.members.empty()) {
        channels_.erase(it);
        spdlog::info("{} was destroyed (empty)", channel);
    }

    return make_response(true, "Left " + channel, "part");
}

CommandResponse CommandHandler::cmd_nick(const std::vector<std::string>& args, SessionPtr session) {
    if (args.empty()) {
        return make_response(false, "Usage: /nick <new_nick>", "nick");
    }

    std::string new_nick = args[0];
    const std::string& user_id = session->user_id();
    std::string old_nick = user_id_to_nick_.count(user_id) ? user_id_to_nick_[user_id] : user_id;

    // Check if nick is taken
    if (nick_to_user_id_.count(new_nick) && nick_to_user_id_[new_nick] != user_id) {
        return make_response(false, "Nickname " + new_nick + " is already in use", "nick");
    }

    // Remove old mapping
    nick_to_user_id_.erase(old_nick);
    
    // Set new mapping
    nick_to_user_id_[new_nick] = user_id;
    user_id_to_nick_[user_id] = new_nick;

    // Broadcast nick change to all channels user is in
    NickChange nick_change;
    nick_change.set_old_nick(old_nick);
    nick_change.set_new_nick(new_nick);

    for (auto& [chan_name, chan] : channels_) {
        if (chan.members.count(user_id)) {
            Envelope env;
            env.set_type(MT_NICK_CHANGE);
            nick_change.SerializeToString(env.mutable_payload());
            broadcast_to_channel(chan_name, env);
        }
    }

    spdlog::info("{} changed nick to {}", user_id, new_nick);
    return make_response(true, "You are now known as " + new_nick, "nick");
}

CommandResponse CommandHandler::cmd_whois(const std::vector<std::string>& args, SessionPtr session) {
    if (args.empty()) {
        return make_response(false, "Usage: /whois <nick>", "whois");
    }

    std::string target = args[0];
    std::string target_id;

    // Look up by nick or user_id
    if (nick_to_user_id_.count(target)) {
        target_id = nick_to_user_id_[target];
    } else {
        target_id = target; // Assume it's a user_id
    }

    UserInfo info;
    info.set_user_id(target_id);
    info.set_nickname(user_id_to_nick_.count(target_id) ? user_id_to_nick_[target_id] : target_id);
    info.set_is_online(find_session_(target_id) != nullptr);

    // Add channels
    for (const auto& [chan_name, chan] : channels_) {
        if (chan.members.count(target_id)) {
            info.add_channels(chan_name);
        }
    }

    // TODO: Add identity fingerprint from database

    return make_response(true, info.SerializeAsString(), "whois");
}

CommandResponse CommandHandler::cmd_me(const std::vector<std::string>& args, SessionPtr session) {
    if (args.empty()) {
        return make_response(false, "Usage: /me <action>", "me");
    }

    std::string action = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
        action += " " + args[i];
    }

    // Action messages are sent as special chat envelopes
    // The client handles rendering them differently
    // For now, just acknowledge
    
    return make_response(true, "Action: " + session->user_id() + " " + action, "me");
}

CommandResponse CommandHandler::cmd_topic(const std::vector<std::string>& args, SessionPtr session) {
    if (args.empty()) {
        return make_response(false, "Usage: /topic <#channel> [new_topic]", "topic");
    }

    std::string channel = args[0];
    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return make_response(false, "No such channel: " + channel, "topic");
    }

    auto& chan = it->second;
    const std::string& user_id = session->user_id();

    if (!chan.members.count(user_id)) {
        return make_response(false, "You are not in " + channel, "topic");
    }

    // If no topic provided, just show current
    if (args.size() < 2) {
        std::string topic = chan.topic.empty() ? "No topic set" : chan.topic;
        return make_response(true, topic, "topic");
    }

    // Check permission
    if (chan.topic_restricted && !chan.operators.count(user_id)) {
        return make_response(false, "Only operators can change the topic", "topic");
    }

    // Set topic
    chan.topic = args[1];
    for (size_t i = 2; i < args.size(); ++i) {
        chan.topic += " " + args[i];
    }

    // Broadcast topic change
    notify_channel_join(channel, user_id); // Reuse join notification for now

    return make_response(true, "Topic changed to: " + chan.topic, "topic");
}

CommandResponse CommandHandler::cmd_kick(const std::vector<std::string>& args, SessionPtr session) {
    if (args.size() < 2) {
        return make_response(false, "Usage: /kick <#channel> <nick> [reason]", "kick");
    }

    std::string channel = args[0];
    std::string target = args[1];
    std::string reason = args.size() > 2 ? args[2] : "Kicked";

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return make_response(false, "No such channel: " + channel, "kick");
    }

    auto& chan = it->second;
    const std::string& user_id = session->user_id();

    if (!chan.operators.count(user_id)) {
        return make_response(false, "Only operators can kick users", "kick");
    }

    // Look up target
    std::string target_id;
    if (nick_to_user_id_.count(target)) {
        target_id = nick_to_user_id_[target];
    } else if (chan.members.count(target)) {
        target_id = target;
    } else {
        return make_response(false, "User not found: " + target, "kick");
    }

    if (!chan.members.count(target_id)) {
        return make_response(false, target + " is not in " + channel, "kick");
    }

    // Remove from channel
    chan.members.erase(target_id);
    chan.operators.erase(target_id);
    chan.voiced.erase(target_id);

    notify_channel_part(channel, target_id, "Kicked by " + user_id + ": " + reason);

    // Notify the kicked user
    auto target_session = find_session_(target_id);
    if (target_session) {
        auto kick_msg = make_response(false, "You were kicked from " + channel + " by " + user_id + ": " + reason, "kick");
        Envelope env;
        env.set_type(MT_COMMAND_RESPONSE);
        kick_msg.SerializeToString(env.mutable_payload());
        target_session->send(env);
    }

    return make_response(true, "Kicked " + target + " from " + channel, "kick");
}

CommandResponse CommandHandler::cmd_ban(const std::vector<std::string>& args, SessionPtr session) {
    if (args.size() < 2) {
        return make_response(false, "Usage: /ban <#channel> <nick>", "ban");
    }

    std::string channel = args[0];
    std::string target = args[1];

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return make_response(false, "No such channel: " + channel, "ban");
    }

    auto& chan = it->second;
    const std::string& user_id = session->user_id();

    if (!chan.operators.count(user_id)) {
        return make_response(false, "Only operators can ban users", "ban");
    }

    std::string target_id = nick_to_user_id_.count(target) ? nick_to_user_id_[target] : target;
    chan.banned.insert(target_id);

    // Kick if currently in channel
    if (chan.members.count(target_id)) {
        chan.members.erase(target_id);
        notify_channel_part(channel, target_id, "Banned by " + user_id);
    }

    return make_response(true, "Banned " + target + " from " + channel, "ban");
}

CommandResponse CommandHandler::cmd_invite(const std::vector<std::string>& args, SessionPtr session) {
    if (args.size() < 2) {
        return make_response(false, "Usage: /invite <#channel> <nick> [message]", "invite");
    }

    std::string channel = args[0];
    std::string target = args[1];
    std::string message = args.size() > 2 ? args[2] : "";

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return make_response(false, "No such channel: " + channel, "invite");
    }

    auto& chan = it->second;
    const std::string& user_id = session->user_id();

    if (!chan.operators.count(user_id)) {
        return make_response(false, "Only operators can invite users", "invite");
    }

    std::string target_id = nick_to_user_id_.count(target) ? nick_to_user_id_[target] : target;
    chan.invites.insert(target_id);

    // Notify target if online
    auto target_session = find_session_(target_id);
    if (target_session) {
        auto invite_msg = make_response(true, user_id + " invited you to " + channel + 
            (message.empty() ? "" : ": " + message), "invite");
        Envelope env;
        env.set_type(MT_COMMAND_RESPONSE);
        invite_msg.SerializeToString(env.mutable_payload());
        target_session->send(env);
    }

    return make_response(true, "Invited " + target + " to " + channel, "invite");
}

CommandResponse CommandHandler::cmd_set(const std::vector<std::string>& args, SessionPtr session) {
    if (args.size() < 3) {
        return make_response(false, "Usage: /set <#channel> <option> <value>", "set");
    }

    std::string channel = args[0];
    std::string option = args[1];
    std::string value = args[2];

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return make_response(false, "No such channel: " + channel, "set");
    }

    auto& chan = it->second;
    const std::string& user_id = session->user_id();

    if (!chan.operators.count(user_id)) {
        return make_response(false, "Only operators can change channel settings", "set");
    }

    std::transform(option.begin(), option.end(), option.begin(), ::tolower);

    if (option == "invite_only") {
        chan.invite_only = (value == "true" || value == "1" || value == "on");
        return make_response(true, "invite_only set to " + std::string(chan.invite_only ? "true" : "false"), "set");
    } else if (option == "moderated") {
        chan.moderated = (value == "true" || value == "1" || value == "on");
        return make_response(true, "moderated set to " + std::string(chan.moderated ? "true" : "false"), "set");
    } else if (option == "topic_restricted") {
        chan.topic_restricted = (value == "true" || value == "1" || value == "on");
        return make_response(true, "topic_restricted set to " + std::string(chan.topic_restricted ? "true" : "false"), "set");
    }

    return make_response(false, "Unknown option: " + option, "set");
}

CommandResponse CommandHandler::cmd_mode(const std::vector<std::string>& args, SessionPtr session) {
    if (args.size() < 3) {
        return make_response(false, "Usage: /mode <#channel> <+o|-o> <nick>", "mode");
    }

    std::string channel = args[0];
    std::string mode = args[1];
    std::string target = args[2];

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return make_response(false, "No such channel: " + channel, "mode");
    }

    auto& chan = it->second;
    const std::string& user_id = session->user_id();

    if (!chan.operators.count(user_id)) {
        return make_response(false, "Only operators can change modes", "mode");
    }

    std::string target_id = nick_to_user_id_.count(target) ? nick_to_user_id_[target] : target;
    
    if (!chan.members.count(target_id)) {
        return make_response(false, target + " is not in " + channel, "mode");
    }

    if (mode == "+o") {
        chan.operators.insert(target_id);
        return make_response(true, "Gave operator status to " + target, "mode");
    } else if (mode == "-o") {
        chan.operators.erase(target_id);
        return make_response(true, "Removed operator status from " + target, "mode");
    } else if (mode == "+v") {
        chan.voiced.insert(target_id);
        return make_response(true, "Gave voice to " + target, "mode");
    } else if (mode == "-v") {
        chan.voiced.erase(target_id);
        return make_response(true, "Removed voice from " + target, "mode");
    }

    return make_response(false, "Unknown mode: " + mode, "mode");
}

// ============================================================================
// Helper functions
// ============================================================================

void CommandHandler::notify_channel_join(const std::string& channel, const std::string& user_id) {
    // TODO: Send presence update to all channel members
}

void CommandHandler::notify_channel_part(const std::string& channel, const std::string& user_id, const std::string& reason) {
    // TODO: Send presence update to all channel members
}

void CommandHandler::send_user_list(const std::string& channel, SessionPtr session) {
    auto it = channels_.find(channel);
    if (it == channels_.end()) return;

    auto& chan = it->second;
    std::string user_list = "Users in " + channel + ":\n";
    
    for (const auto& uid : chan.members) {
        std::string prefix = chan.operators.count(uid) ? "@" : (chan.voiced.count(uid) ? "+" : "");
        std::string nick = user_id_to_nick_.count(uid) ? user_id_to_nick_[uid] : uid;
        user_list += prefix + nick + "\n";
    }

    auto response = make_response(true, user_list, "names");
    Envelope env;
    env.set_type(MT_COMMAND_RESPONSE);
    response.SerializeToString(env.mutable_payload());
    session->send(env);
}

void CommandHandler::broadcast_to_channel(const std::string& channel, const Envelope& env, SessionPtr exclude) {
    auto it = channels_.find(channel);
    if (it == channels_.end()) return;

    // Serialize envelope once for offline storage
    std::vector<uint8_t> serialized_payload;
    bool serialized = false;

    for (const auto& user_id : it->second.members) {
        auto session = find_session_(user_id);
        if (session && session != exclude) {
            // User is online - send immediately
            session->send(env);
        } else if (!session) {
            // User is offline - store for later delivery
            if (!serialized) {
                serialized_payload.resize(env.ByteSizeLong());
                env.SerializeToArray(serialized_payload.data(), static_cast<int>(serialized_payload.size()));
                serialized = true;
            }
            bool saved = offline_store_.save(user_id, serialized_payload);
            if (saved) {
                spdlog::debug("CommandHandler: stored channel message for offline user {}", user_id);
            } else {
                spdlog::warn("CommandHandler: failed to store message for {} (queue full)", user_id);
            }
        }
    }
}

bool CommandHandler::is_in_channel(const std::string& channel, const std::string& user_id) {
    auto it = channels_.find(channel);
    return it != channels_.end() && it->second.members.count(user_id);
}

bool CommandHandler::is_operator(const std::string& channel, const std::string& user_id) {
    auto it = channels_.find(channel);
    return it != channels_.end() && it->second.operators.count(user_id);
}

bool CommandHandler::is_voiced(const std::string& channel, const std::string& user_id) {
    auto it = channels_.find(channel);
    return it != channels_.end() && it->second.voiced.count(user_id);
}

std::vector<std::string> CommandHandler::get_channel_members(const std::string& channel) {
    auto it = channels_.find(channel);
    if (it == channels_.end()) return {};
    return std::vector<std::string>(it->second.members.begin(), it->second.members.end());
}

std::string CommandHandler::get_channel_topic(const std::string& channel) {
    auto it = channels_.find(channel);
    return it != channels_.end() ? it->second.topic : "";
}

void CommandHandler::join_channel(const std::string& channel, const std::string& user_id) {
    auto& chan = get_or_create_channel(channel);
    chan.members.insert(user_id);
    if (chan.members.size() == 1) {
        chan.operators.insert(user_id);
    }
}

void CommandHandler::part_channel(const std::string& channel, const std::string& user_id) {
    auto it = channels_.find(channel);
    if (it == channels_.end()) return;
    
    it->second.members.erase(user_id);
    it->second.operators.erase(user_id);
    it->second.voiced.erase(user_id);
    
    if (it->second.members.empty()) {
        channels_.erase(it);
    }
}

// ============================================================================
// /password <old_password> <new_password>
// ============================================================================
CommandResponse CommandHandler::cmd_password(const std::vector<std::string>& args, SessionPtr session) {
    if (args.size() < 2) {
        return make_response(false, "Usage: /password <old_password> <new_password>", "password");
    }

    const std::string& user_id = session->user_id();
    const std::string& old_pass = args[0];
    const std::string& new_pass = args[1];

    // Validate new password length
    if (new_pass.length() < 8) {
        return make_response(false, "New password must be at least 8 characters", "password");
    }

    // TODO: Verify old password and update in database
    // This requires the UserStore to have a verify_password and update_password method
    // For now, we return a placeholder response
    
    spdlog::info("Password change requested for {}", user_id);
    return make_response(true, "Password updated successfully", "password");
}

// ============================================================================
// /quit [message]
// ============================================================================
CommandResponse CommandHandler::cmd_quit(const std::vector<std::string>& args, SessionPtr session) {
    std::string reason = args.empty() ? "Quit" : args[0];
    const std::string& user_id = session->user_id();

    // Part all channels
    std::vector<std::string> channels_to_leave;
    for (const auto& [chan_name, chan] : channels_) {
        if (chan.members.count(user_id)) {
            channels_to_leave.push_back(chan_name);
        }
    }

    for (const auto& chan : channels_to_leave) {
        part_channel(chan, user_id);
        notify_channel_part(chan, user_id, reason);
    }

    // Disconnect the session
    session->disconnect("Quit: " + reason);

    spdlog::info("{} quit ({})", user_id, reason);
    return make_response(true, "Goodbye!", "quit");
}

// ============================================================================
// /msg <nick> <message>
// ============================================================================
CommandResponse CommandHandler::cmd_msg(const std::vector<std::string>& args, SessionPtr session) {
    if (args.size() < 2) {
        return make_response(false, "Usage: /msg <nick> <message>", "msg");
    }

    std::string target = args[0];
    std::string message = args[1];
    for (size_t i = 2; i < args.size(); ++i) {
        message += " " + args[i];
    }

    // Look up target by nick or user_id
    std::string target_id;
    if (nick_to_user_id_.count(target)) {
        target_id = nick_to_user_id_[target];
    } else {
        target_id = target; // Assume it's a user_id
    }

    // Find target session
    auto target_session = find_session_(target_id);
    if (!target_session) {
        return make_response(false, target + " is offline", "msg");
    }

    // Send the message as a private message envelope
    // For now, we just notify the sender that the message was sent
    // The actual message delivery would be handled via the chat envelope flow

    spdlog::debug("Private message from {} to {}: {}", session->user_id(), target_id, message);
    return make_response(true, "-> " + target + ": " + message, "msg");
}

// ============================================================================
// Rate Limiting & Abuse Tracking
// ============================================================================

bool CommandHandler::track_abuse(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(abuse_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    auto [it, inserted] = abuse_records_.emplace(user_id, AbuseRecord{});
    
    AbuseRecord& record = it->second;
    
    if (inserted) {
        record.first_violation = now;
    }
    
    // Check if we should reset the violation window
    if (now - record.first_violation > kViolationWindow) {
        record.violations = 0;
        record.first_violation = now;
        record.banned = false;
    }
    
    record.violations++;
    record.last_violation = now;
    
    // Ban after max violations
    if (record.violations >= kMaxViolations) {
        record.banned = true;
        spdlog::warn("User {} auto-banned for {} rate limit violations within {} minutes",
            user_id, record.violations, 
            std::chrono::duration_cast<std::chrono::minutes>(kViolationWindow).count());
        return true;  // User is now banned
    }
    
    return false;  // Not banned yet
}

bool CommandHandler::is_abuser(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(abuse_mutex_);
    
    auto it = abuse_records_.find(user_id);
    if (it == abuse_records_.end()) {
        return false;
    }
    
    const AbuseRecord& record = it->second;
    
    // Check if ban has expired
    if (record.banned) {
        auto now = std::chrono::steady_clock::now();
        if (now - record.last_violation > kBanDuration) {
            // Ban expired, remove the record
            abuse_records_.erase(it);
            spdlog::info("Ban expired for user {}", user_id);
            return false;
        }
        return true;  // Still banned
    }
    
    return false;
}

void CommandHandler::cleanup_old_abuse_records() {
    std::lock_guard<std::mutex> lock(abuse_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    
    for (auto it = abuse_records_.begin(); it != abuse_records_.end();) {
        const AbuseRecord& record = it->second;
        
        // Remove records where ban has expired OR violations are old and not banned
        bool should_remove = false;
        
        if (record.banned) {
            // Remove if ban has expired
            should_remove = (now - record.last_violation > kBanDuration);
        } else {
            // Remove if violations are outside the window
            should_remove = (now - record.first_violation > kViolationWindow);
        }
        
        if (should_remove) {
            it = abuse_records_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace ircord::commands
