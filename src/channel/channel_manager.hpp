#pragma once

#include "db/offline_store.hpp"
#include "ircord.pb.h"

#include <functional>
#include <memory>
#include <string>

namespace ircord::net { class Session; }

namespace ircord::channel {

using SessionFinder = std::function<std::shared_ptr<net::Session>(const std::string&)>;

// Routes chat envelopes to online recipients or stores them offline.
// The server is blind to the content — only recipient_id is inspected.
class ChannelManager {
public:
    ChannelManager(SessionFinder find_session, db::OfflineStore& offline_store);

    void route(const ChatEnvelope& chat, const Envelope& raw_env);

private:
    SessionFinder find_session_;
    db::OfflineStore& offline_store_;
};

} // namespace ircord::channel
