#include "commands/command_handler.hpp"
#include "net/session.hpp"
#include "db/database.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace ircord::commands {

CommandHandler::CommandHandler(
    SessionFinder find_session,
    BroadcastFunc broadcast,
    db::Database& db)
    : find_session_(std::move(find_session))
    , broadcast_(std::move(broadcast))
    , db_(db)
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
}

CommandResponse CommandHandler::handle_command(const IrcCommand& cmd, SessionPtr session) {
    if (cmd.command().empty()) {
        return CommandResponse{false, "Empty command", ""};
    }

    std::string cmd_name = cmd.command();
    std::transform(cmd_name.begin(), cmd_name.end(), cmd_name.begin(), ::tolower);

    auto it = command_map_.find(cmd_name);
    if (it == command_map_.end()) {
        return CommandResponse{false, "Unknown command: " + cmd_name, cmd_name};
    }

    std::vector<std::string> args(cmd.args().begin(), cmd.args().end());
    return it->second(args, session);
}

CommandHandler::ChannelState& CommandHandler::get_or_create_channel(const std::string& name) {
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
        return CommandResponse{false, "Usage: /join <#channel>", "join"};
    }

    std::string channel = args[0];
    if (channel.empty() || channel[0] != '#') {
        return CommandResponse{false, "Channel name must start with #", "join"};
    }

    const std::string& user_id = session->user_id();
    auto& chan = get_or_create_channel(channel);

    if (chan.banned.count(user_id)) {
        return CommandResponse{false, "You are banned from " + channel, "join"};
    }

    if (chan.invite_only && !chan.invites.count(user_id) && !chan.operators.count(user_id)) {
        return CommandResponse{false, channel + " is invite-only", "join"};
    }

    chan.members.insert(user_id);
    
    if (chan.members.size() == 1) {
        chan.operators.insert(user_id);
        spdlog::info("{} became operator of {}", user_id, channel);
    }

    notify_channel_join(channel, user_id);
    send_user_list(channel, session);

    std::string topic_msg = chan.topic.empty() ? "No topic set" : "Topic: " + chan.topic;
    return CommandResponse{true, "Joined " + channel + "\n" + topic_msg, "join"};
}

CommandResponse CommandHandler::cmd_part(const std::vector<std::string>& args, SessionPtr session) {
    const std::string& user_id = session->user_id();
    
    if (args.empty()) {
        return CommandResponse{false, "Usage: /part <#channel> [message]", "part"};
    }

    std::string channel = args[0];
    std::string reason = args.size() > 1 ? args[1] : "Leaving";

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return CommandResponse{false, "No such channel: " + channel, "part"};
    }

    auto& chan = it->second;
    if (!chan.members.count(user_id)) {
        return CommandResponse{false, "You are not in " + channel, "part"};
    }

    chan.members.erase(user_id);
    chan.operators.erase(user_id);
    chan.voiced.erase(user_id);

    notify_channel_part(channel, user_id, reason);

    if (chan.members.empty()) {
        channels_.erase(it);
        spdlog::info("{} was destroyed (empty)", channel);
    }

    return CommandResponse{true, "Left " + channel, "part"};
}

CommandResponse CommandHandler::cmd_nick(const std::vector<std::string>& args, SessionPtr session) {
    if (args.empty()) {
        return CommandResponse{false, "Usage: /nick <new_nick>", "nick"};
    }

    std::string new_nick = args[0];
    const std::string& user_id = session->user_id();
    std::string old_nick = user_id_to_nick_.count(user_id) ? user_id_to_nick_[user_id] : user_id;

    // Check if nick is taken
    if (nick_to_user_id_.count(new_nick) && nick_to_user_id_[new_nick] != user_id) {
        return CommandResponse{false, "Nickname " + new_nick + " is already in use", "nick"};
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
    return CommandResponse{true, "You are now known as " + new_nick, "nick"};
}

CommandResponse CommandHandler::cmd_whois(const std::vector<std::string>& args, SessionPtr session) {
    if (args.empty()) {
        return CommandResponse{false, "Usage: /whois <nick>", "whois"};
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

    return CommandResponse{true, info.SerializeAsString(), "whois"};
}

CommandResponse CommandHandler::cmd_me(const std::vector<std::string>& args, SessionPtr session) {
    if (args.empty()) {
        return CommandResponse{false, "Usage: /me <action>", "me"};
    }

    std::string action = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
        action += " " + args[i];
    }

    // Action messages are sent as special chat envelopes
    // The client handles rendering them differently
    // For now, just acknowledge
    
    return CommandResponse{true, "Action: " + session->user_id() + " " + action, "me"};
}

CommandResponse CommandHandler::cmd_topic(const std::vector<std::string>& args, SessionPtr session) {
    if (args.empty()) {
        return CommandResponse{false, "Usage: /topic <#channel> [new_topic]", "topic"};
    }

    std::string channel = args[0];
    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return CommandResponse{false, "No such channel: " + channel, "topic"};
    }

    auto& chan = it->second;
    const std::string& user_id = session->user_id();

    if (!chan.members.count(user_id)) {
        return CommandResponse{false, "You are not in " + channel, "topic"};
    }

    // If no topic provided, just show current
    if (args.size() < 2) {
        std::string topic = chan.topic.empty() ? "No topic set" : chan.topic;
        return CommandResponse{true, topic, "topic"};
    }

    // Check permission
    if (chan.topic_restricted && !chan.operators.count(user_id)) {
        return CommandResponse{false, "Only operators can change the topic", "topic"};
    }

    // Set topic
    chan.topic = args[1];
    for (size_t i = 2; i < args.size(); ++i) {
        chan.topic += " " + args[i];
    }

    // Broadcast topic change
    notify_channel_join(channel, user_id); // Reuse join notification for now

    return CommandResponse{true, "Topic changed to: " + chan.topic, "topic"};
}

CommandResponse CommandHandler::cmd_kick(const std::vector<std::string>& args, SessionPtr session) {
    if (args.size() < 2) {
        return CommandResponse{false, "Usage: /kick <#channel> <nick> [reason]", "kick"};
    }

    std::string channel = args[0];
    std::string target = args[1];
    std::string reason = args.size() > 2 ? args[2] : "Kicked";

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return CommandResponse{false, "No such channel: " + channel, "kick"};
    }

    auto& chan = it->second;
    const std::string& user_id = session->user_id();

    if (!chan.operators.count(user_id)) {
        return CommandResponse{false, "Only operators can kick users", "kick"};
    }

    // Look up target
    std::string target_id;
    if (nick_to_user_id_.count(target)) {
        target_id = nick_to_user_id_[target];
    } else if (chan.members.count(target)) {
        target_id = target;
    } else {
        return CommandResponse{false, "User not found: " + target, "kick"};
    }

    if (!chan.members.count(target_id)) {
        return CommandResponse{false, target + " is not in " + channel, "kick"};
    }

    // Remove from channel
    chan.members.erase(target_id);
    chan.operators.erase(target_id);
    chan.voiced.erase(target_id);

    notify_channel_part(channel, target_id, "Kicked by " + user_id + ": " + reason);

    // Notify the kicked user
    auto target_session = find_session_(target_id);
    if (target_session) {
        CommandResponse kick_msg{false, "You were kicked from " + channel + " by " + user_id + ": " + reason, "kick"};
        Envelope env;
        env.set_type(MT_COMMAND_RESPONSE);
        kick_msg.SerializeToString(env.mutable_payload());
        target_session->send(env);
    }

    return CommandResponse{true, "Kicked " + target + " from " + channel, "kick"};
}

CommandResponse CommandHandler::cmd_ban(const std::vector<std::string>& args, SessionPtr session) {
    if (args.size() < 2) {
        return CommandResponse{false, "Usage: /ban <#channel> <nick>", "ban"};
    }

    std::string channel = args[0];
    std::string target = args[1];

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return CommandResponse{false, "No such channel: " + channel, "ban"};
    }

    auto& chan = it->second;
    const std::string& user_id = session->user_id();

    if (!chan.operators.count(user_id)) {
        return CommandResponse{false, "Only operators can ban users", "ban"};
    }

    std::string target_id = nick_to_user_id_.count(target) ? nick_to_user_id_[target] : target;
    chan.banned.insert(target_id);

    // Kick if currently in channel
    if (chan.members.count(target_id)) {
        chan.members.erase(target_id);
        notify_channel_part(channel, target_id, "Banned by " + user_id);
    }

    return CommandResponse{true, "Banned " + target + " from " + channel, "ban"};
}

CommandResponse CommandHandler::cmd_invite(const std::vector<std::string>& args, SessionPtr session) {
    if (args.size() < 2) {
        return CommandResponse{false, "Usage: /invite <#channel> <nick> [message]", "invite"};
    }

    std::string channel = args[0];
    std::string target = args[1];
    std::string message = args.size() > 2 ? args[2] : "";

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return CommandResponse{false, "No such channel: " + channel, "invite"};
    }

    auto& chan = it->second;
    const std::string& user_id = session->user_id();

    if (!chan.operators.count(user_id)) {
        return CommandResponse{false, "Only operators can invite users", "invite"};
    }

    std::string target_id = nick_to_user_id_.count(target) ? nick_to_user_id_[target] : target;
    chan.invites.insert(target_id);

    // Notify target if online
    auto target_session = find_session_(target_id);
    if (target_session) {
        CommandResponse invite_msg{true, user_id + " invited you to " + channel + 
            (message.empty() ? "" : ": " + message), "invite"};
        Envelope env;
        env.set_type(MT_COMMAND_RESPONSE);
        invite_msg.SerializeToString(env.mutable_payload());
        target_session->send(env);
    }

    return CommandResponse{true, "Invited " + target + " to " + channel, "invite"};
}

CommandResponse CommandHandler::cmd_set(const std::vector<std::string>& args, SessionPtr session) {
    if (args.size() < 3) {
        return CommandResponse{false, "Usage: /set <#channel> <option> <value>", "set"};
    }

    std::string channel = args[0];
    std::string option = args[1];
    std::string value = args[2];

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return CommandResponse{false, "No such channel: " + channel, "set"};
    }

    auto& chan = it->second;
    const std::string& user_id = session->user_id();

    if (!chan.operators.count(user_id)) {
        return CommandResponse{false, "Only operators can change channel settings", "set"};
    }

    std::transform(option.begin(), option.end(), option.begin(), ::tolower);

    if (option == "invite_only") {
        chan.invite_only = (value == "true" || value == "1" || value == "on");
        return CommandResponse{true, "invite_only set to " + std::string(chan.invite_only ? "true" : "false"), "set"};
    } else if (option == "moderated") {
        chan.moderated = (value == "true" || value == "1" || value == "on");
        return CommandResponse{true, "moderated set to " + std::string(chan.moderated ? "true" : "false"), "set"};
    } else if (option == "topic_restricted") {
        chan.topic_restricted = (value == "true" || value == "1" || value == "on");
        return CommandResponse{true, "topic_restricted set to " + std::string(chan.topic_restricted ? "true" : "false"), "set"};
    }

    return CommandResponse{false, "Unknown option: " + option, "set"};
}

CommandResponse CommandHandler::cmd_mode(const std::vector<std::string>& args, SessionPtr session) {
    if (args.size() < 3) {
        return CommandResponse{false, "Usage: /mode <#channel> <+o|-o> <nick>", "mode"};
    }

    std::string channel = args[0];
    std::string mode = args[1];
    std::string target = args[2];

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return CommandResponse{false, "No such channel: " + channel, "mode"};
    }

    auto& chan = it->second;
    const std::string& user_id = session->user_id();

    if (!chan.operators.count(user_id)) {
        return CommandResponse{false, "Only operators can change modes", "mode"};
    }

    std::string target_id = nick_to_user_id_.count(target) ? nick_to_user_id_[target] : target;
    
    if (!chan.members.count(target_id)) {
        return CommandResponse{false, target + " is not in " + channel, "mode"};
    }

    if (mode == "+o") {
        chan.operators.insert(target_id);
        return CommandResponse{true, "Gave operator status to " + target, "mode"};
    } else if (mode == "-o") {
        chan.operators.erase(target_id);
        return CommandResponse{true, "Removed operator status from " + target, "mode"};
    } else if (mode == "+v") {
        chan.voiced.insert(target_id);
        return CommandResponse{true, "Gave voice to " + target, "mode"};
    } else if (mode == "-v") {
        chan.voiced.erase(target_id);
        return CommandResponse{true, "Removed voice from " + target, "mode"};
    }

    return CommandResponse{false, "Unknown mode: " + mode, "mode"};
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

    CommandResponse response{true, user_list, "names"};
    Envelope env;
    env.set_type(MT_COMMAND_RESPONSE);
    response.SerializeToString(env.mutable_payload());
    session->send(env);
}

void CommandHandler::broadcast_to_channel(const std::string& channel, const Envelope& env, SessionPtr exclude) {
    auto it = channels_.find(channel);
    if (it == channels_.end()) return;

    for (const auto& user_id : it->second.members) {
        auto session = find_session_(user_id);
        if (session && session != exclude) {
            session->send(env);
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

} // namespace ircord::commands
