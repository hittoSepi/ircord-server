#include "channel_manager.hpp"
#include "net/session.hpp"

#include <spdlog/spdlog.h>

namespace ircord::channel {

ChannelManager::ChannelManager(SessionFinder find_session, db::OfflineStore& offline_store)
    : find_session_(std::move(find_session))
    , offline_store_(offline_store)
{}

void ChannelManager::route(const ChatEnvelope& chat, const Envelope& raw_env) {
    const std::string& recipient = chat.recipient_id();

    auto session = find_session_(recipient);
    if (session) {
        session->send(raw_env);
        spdlog::debug("ChannelManager: routed message from {} to {} (online)",
            chat.sender_id(), recipient);
        return;
    }

    // Recipient offline: serialise and store
    std::vector<uint8_t> payload(raw_env.ByteSizeLong());
    raw_env.SerializeToArray(payload.data(), static_cast<int>(payload.size()));
    offline_store_.save(recipient, payload);
    spdlog::debug("ChannelManager: stored offline message for {}", recipient);
}

} // namespace ircord::channel
