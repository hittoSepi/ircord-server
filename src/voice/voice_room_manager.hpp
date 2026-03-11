#pragma once

#include "ircord.pb.h"
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ircord::net { class Session; }

namespace ircord::voice {

class VoiceRoomManager {
public:
    using SessionPtr = std::shared_ptr<net::Session>;
    using FindSessionFn = std::function<SessionPtr(const std::string&)>;

    explicit VoiceRoomManager(FindSessionFn find_session);

    // Returns error string on failure, empty on success.
    std::string join(const std::string& channel_id, const std::string& user_id);
    void leave(const std::string& channel_id, const std::string& user_id);
    void on_disconnect(const std::string& user_id);

    std::vector<std::string> participants(const std::string& channel_id) const;
    bool is_in_voice(const std::string& user_id) const;

    static constexpr size_t kMaxPerRoom = 8;

private:
    void send_to(const std::string& user_id, MessageType type,
                 const google::protobuf::Message& msg);

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::set<std::string>> rooms_;
    std::unordered_map<std::string, std::string> user_room_;

    FindSessionFn find_session_;
};

} // namespace ircord::voice
