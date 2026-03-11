#include "voice/voice_room_manager.hpp"
#include "net/session.hpp"
#include <spdlog/spdlog.h>

namespace ircord::voice {

VoiceRoomManager::VoiceRoomManager(FindSessionFn find_session)
    : find_session_(std::move(find_session)) {}

std::string VoiceRoomManager::join(const std::string& channel_id,
                                    const std::string& user_id) {
    std::lock_guard lk(mu_);

    // Already in a room? Auto-leave.
    if (auto it = user_room_.find(user_id); it != user_room_.end()) {
        auto& old_room = rooms_[it->second];
        old_room.erase(user_id);

        VoiceRoomLeave leave_msg;
        leave_msg.set_channel_id(it->second);
        leave_msg.set_user_id(user_id);
        for (const auto& peer : old_room) {
            send_to(peer, MT_VOICE_ROOM_LEAVE, leave_msg);
        }

        if (old_room.empty()) rooms_.erase(it->second);
        user_room_.erase(it);
    }

    auto& room = rooms_[channel_id];

    if (room.size() >= kMaxPerRoom) {
        return "Voice room is full (max " + std::to_string(kMaxPerRoom) + ")";
    }

    // Notify existing participants that a new user joined
    VoiceRoomJoin join_msg;
    join_msg.set_channel_id(channel_id);
    join_msg.set_user_id(user_id);
    for (const auto& peer : room) {
        send_to(peer, MT_VOICE_ROOM_JOIN, join_msg);
    }

    // Add user to room
    room.insert(user_id);
    user_room_[user_id] = channel_id;

    // Send full participant list to the joiner
    VoiceRoomState state;
    state.set_channel_id(channel_id);
    for (const auto& p : room) {
        state.add_participants(p);
    }
    send_to(user_id, MT_VOICE_ROOM_STATE, state);

    spdlog::info("Voice: {} joined room {} ({} participants)",
                 user_id, channel_id, room.size());
    return {};
}

void VoiceRoomManager::leave(const std::string& channel_id,
                              const std::string& user_id) {
    std::lock_guard lk(mu_);

    auto room_it = rooms_.find(channel_id);
    if (room_it == rooms_.end()) return;

    auto& room = room_it->second;
    if (!room.erase(user_id)) return;

    user_room_.erase(user_id);

    VoiceRoomLeave leave_msg;
    leave_msg.set_channel_id(channel_id);
    leave_msg.set_user_id(user_id);
    for (const auto& peer : room) {
        send_to(peer, MT_VOICE_ROOM_LEAVE, leave_msg);
    }

    if (room.empty()) rooms_.erase(room_it);

    spdlog::info("Voice: {} left room {}", user_id, channel_id);
}

void VoiceRoomManager::on_disconnect(const std::string& user_id) {
    std::lock_guard lk(mu_);

    auto it = user_room_.find(user_id);
    if (it == user_room_.end()) return;

    std::string channel_id = it->second;
    auto room_it = rooms_.find(channel_id);
    if (room_it != rooms_.end()) {
        room_it->second.erase(user_id);

        VoiceRoomLeave leave_msg;
        leave_msg.set_channel_id(channel_id);
        leave_msg.set_user_id(user_id);
        for (const auto& peer : room_it->second) {
            send_to(peer, MT_VOICE_ROOM_LEAVE, leave_msg);
        }

        if (room_it->second.empty()) rooms_.erase(room_it);
    }

    user_room_.erase(it);
    spdlog::info("Voice: {} disconnected, removed from {}", user_id, channel_id);
}

std::vector<std::string> VoiceRoomManager::participants(
    const std::string& channel_id) const {
    std::lock_guard lk(mu_);
    auto it = rooms_.find(channel_id);
    if (it == rooms_.end()) return {};
    return {it->second.begin(), it->second.end()};
}

bool VoiceRoomManager::is_in_voice(const std::string& user_id) const {
    std::lock_guard lk(mu_);
    return user_room_.count(user_id) > 0;
}

void VoiceRoomManager::send_to(const std::string& user_id, MessageType type,
                                const google::protobuf::Message& msg) {
    auto session = find_session_(user_id);
    if (!session) return;
    Envelope env;
    env.set_type(type);
    msg.SerializeToString(env.mutable_payload());
    session->send(env);
}

} // namespace ircord::voice
