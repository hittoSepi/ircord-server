# Admin Reserved Nickname System — PLAN

**Status:** Design Phase  
**Target:** ircord-server v0.2.0  
**Related:** ircord-server-tui (admin interface)

---

## 1. Overview

The server owner needs a reserved identity that:
- Cannot be taken by regular users (any variation of "admin")
- Is always online (runs inside server process)
- Has elevated privileges
- Is controlled via ircord-server-tui interface

This is **not** a regular user account - it's a server-internal system identity.

---

## 2. Reserved Identifiers

### 2.1 Exact Matches (Case-insensitive)
```
admin
administrator
root
sysop
operator
ircord
server
system
```

### 2.2 Pattern Matches
```regex
^admin.*$           # admin, admin123, admin_
^.*admin$           # myadmin, theadmin
^.*admin.*$         # myadmin123, admin_test
^adm.*$             # adm, adm1n
^root.*$            # root, root123
^sys.*$             # sys, system
^oper.*$            # oper, operator99
```

### 2.3 Leet Speak Variations
```
4dmin, 4dm1n        # a→4, i→1
@dmin, @dm1n        # a→@
adm1n, @dm1n        # i→1
root, r00t, r00T    # o→0
```

### 2.4 Unicode/Homoglyph Prevention
```
аdmin (Cyrillic 'а' instead of Latin 'a')
admіn (Cyrillic 'і')
𝖆𝖉𝖒𝖎𝖓 (Mathematical bold)
```

---

## 3. System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        IRCord Server                             │
│                                                                  │
│  ┌──────────────────┐      ┌─────────────────────────────────┐ │
│  │ ircord-server    │      │ ServerOwner (reserved identity) │ │
│  │     (TUI)        │◄────►│                                 │ │
│  │  Admin Interface │      │ • User ID: "admin" (fixed)      │ │
│  └──────────────────┘      │ • Always online                 │ │
│           │                │ • Internal message routing      │ │
│           │                │ • No network connection         │ │
│           │                │ • Direct DB access              │ │
│           ▼                └─────────────────────────────────┘ │
│  ┌──────────────────┐                     │                    │
│  │   Commands:      │                     │                    │
│  │   /ban, /kick    │                     │                    │ │
│  │   /announce      │◄────────────────────┘                    │
│  │   /shutdown      │                                          │
│  └──────────────────┘                                          │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐│
│  │              Reserved Nickname Filter                       ││
│  │                                                             ││
│  │  Register Request ──► [Check against patterns] ──► Reject  ││
│  │                                    │                        ││
│  │                                    ▼                        ││
│  │                              Allow if:                      ││
│  │                              • Not in reserved list         ││
│  │                              • Not pattern match            ││
│  │                              • Not leet variation           ││
│  │                              • Not unicode homoglyph        ││
│  └────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
```

---

## 4. Implementation Details

### 4.1 ReservedIdentity Class

```cpp
namespace ircord {

class ReservedIdentity {
public:
    static constexpr const char* OWNER_ID = "admin";
    
    // Check if nickname is reserved
    static bool is_reserved(std::string_view nickname);
    
    // Normalize nickname for comparison
    static std::string normalize(std::string_view nickname);
    
    // Check if user is the server owner
    static bool is_owner(std::string_view user_id);
};

// ServerOwner - the internal admin entity
class ServerOwner {
public:
    ServerOwner(Server& server);
    
    // Send message to channel (internal routing)
    void send_to_channel(const std::string& channel, const std::string& message);
    
    // Send direct message to user (internal routing)
    void send_to_user(const std::string& user_id, const std::string& message);
    
    // Execute admin command
    bool execute_command(const std::string& command, const std::vector<std::string>& args);
    
    // Get presence info
    UserPresence get_presence() const;
    
    // User ID is always "admin"
    std::string_view user_id() const { return ReservedIdentity::OWNER_ID; }
    
private:
    Server& server_;
    std::unique_ptr<CryptoIdentity> crypto_;  // Ed25519 keys for signing
};

} // namespace ircord
```

### 4.2 Registration Filter

```cpp
// In UserStore::register_user()

bool UserStore::register_user(const std::string& user_id, /* ... */) {
    // Check reserved names
    if (ReservedIdentity::is_reserved(user_id)) {
        spdlog::warn("Registration rejected: '{}' is a reserved identifier", user_id);
        return false;  // Or specific error: ERR_RESERVED_NICK
    }
    
    // Continue with normal registration...
}
```

### 4.3 Pattern Matching Algorithm

```cpp
bool ReservedIdentity::is_reserved(std::string_view nickname) {
    // 1. Normalize (lowercase, remove special chars)
    auto normalized = normalize(nickname);
    
    // 2. Check exact matches
    static const std::unordered_set<std::string> exact_reserved = {
        "admin", "administrator", "root", "sysop", 
        "operator", "ircord", "server", "system"
    };
    if (exact_reserved.contains(normalized)) {
        return true;
    }
    
    // 3. Check pattern matches
    static const std::vector<std::regex> patterns = {
        std::regex("^adm.*"),      // starts with adm
        std::regex(".*admin.*"),   // contains admin
        std::regex("^root.*"),     // starts with root
        std::regex("^sys.*"),      // starts with sys
        std::regex("^oper.*"),     // starts with oper
    };
    
    for (const auto& pattern : patterns) {
        if (std::regex_match(normalized, pattern)) {
            return true;
        }
    }
    
    // 4. Check leet speak variations
    auto leet_normalized = normalize_leet(normalized);
    if (exact_reserved.contains(leet_normalized)) {
        return true;
    }
    
    // 5. Check unicode homoglyphs
    if (contains_unicode_homoglyphs(nickname)) {
        return true;
    }
    
    return false;
}

std::string ReservedIdentity::normalize_leet(std::string_view input) {
    std::string result(input);
    // Replace leet characters
    std::replace(result.begin(), result.end(), '4', 'a');
    std::replace(result.begin(), result.end(), '@', 'a');
    std::replace(result.begin(), result.end(), '1', 'i');
    std::replace(result.begin(), result.end(), '3', 'e');
    std::replace(result.begin(), result.end(), '0', 'o');
    std::replace(result.begin(), result.end(), '5', 's');
    std::replace(result.begin(), result.end(), '7', 't');
    return result;
}
```

---

## 5. ServerOwner Behavior

### 5.1 Always Online
```cpp
// ServerOwner is never "offline"
UserPresence ServerOwner::get_presence() const {
    return {
        .user_id = std::string(ReservedIdentity::OWNER_ID),
        .status = PresenceStatus::Online,
        .last_seen = std::chrono::system_clock::now()
    };
}
```

### 5.2 Internal Message Routing

```cpp
// Messages from ServerOwner don't go through network
void ServerOwner::send_to_channel(const std::string& channel, const std::string& message) {
    // Create envelope signed with owner's keys
    ChatEnvelope envelope;
    envelope.channel_id = channel;
    envelope.sender_id = std::string(user_id());
    envelope.content = message;
    envelope.timestamp = get_timestamp();
    envelope.signature = crypto_->sign(envelope.serialize());
    
    // Route directly to channel manager
    server_.channel_manager().broadcast(envelope);
    
    // Store in database for history
    server_.database().store_message(envelope);
}
```

### 5.3 Available Commands

| Command | Description | Example |
|---------|-------------|---------|
| `/announce <message>` | Send to all channels | `/announce Server restart in 5 min` |
| `/ban <user> [reason]` | Ban user | `/ban spammer Advertising` |
| `/kick <user> [reason]` | Kick from channel | `/kick troublemaker` |
| `/shutdown` | Graceful shutdown | `/shutdown` |
| `/restart` | Restart server | `/restart` |
| `/config <key> <value>` | Change config | `/config max_connections 200` |
| `/status` | Show server stats | `/status` |

---

## 6. ircord-server-tui Integration

### 6.1 TUI as Admin Interface

```
┌─────────────────────────────────────────────────────────────┐
│  IRCord Server Admin (TUI)                                   │
├─────────────────────────────────────────────────────────────┤
│  [#general] [Logs] [Users] [Config] [Status]                │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  [09:23:45] <admin> Server started                          │
│  [09:24:12] <admin> 3 users online                          │
│  [09:25:01] <Sepi> Hello everyone!                          │
│                                                              │
│  > /announce Welcome to the server!                         │
│                                                              │
├─────────────────────────────────────────────────────────────┤
│  [Type command or message...]                               │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 TUI ↔ ServerOwner Communication

```cpp
// In ircord-server-tui
class AdminSession {
public:
    // Connect to local server (unix socket or internal)
    void connect_to_server();
    
    // Send command as admin
    void send_command(const std::string& command);
    
    // Receive messages (for display in TUI)
    void on_server_message(const std::string& channel, const std::string& message);
    
private:
    // We ARE the ServerOwner, not a separate connection
    ServerOwner* owner_;
};
```

---

## 7. Security Considerations

### 7.1 Reserved List Updates
- Reserved patterns are hardcoded in source
- Updates require server restart (or hot-reload)
- Configurable additional reserved words in server.toml

### 7.2 ServerOwner Keys
- Generated on first server start if not exists
- Stored in `keys/admin.ed25519` (chmod 600)
- Backup required for server migration

### 7.3 TUI Access Control
- TUI runs on same machine (local only)
- Unix socket with file permissions
- Optional: password for TUI startup

---

## 8. Configuration

```toml
[admin]
# Server owner settings
enabled = true
user_id = "admin"           # Fixed, cannot change
auto_join_channels = ["#general", "#announcements"]

# Additional reserved patterns
additional_reserved = ["owner", "founder", "boss"]

# Allow these exact matches (exceptions)
# whitelist = ["administrative"]

[admin.security]
# TUI access
tui_password_hash = ""      # Empty = no password (file perms only)
tui_unix_socket = "/run/ircord/admin.sock"

# Key file location
key_file = "./keys/admin.ed25519"
```

---

## 9. Error Messages

| Error Code | Message |
|------------|---------|
| `ERR_RESERVED_NICK` | "This nickname is reserved for system use" |
| `ERR_RESERVED_PATTERN` | "This nickname matches a reserved pattern" |
| `ERR_RESERVED_LEET` | "Leet speak variations of reserved names are not allowed" |
| `ERR_RESERVED_UNICODE` | "Unicode variations of reserved names are not allowed" |

---

## 10. Future Extensions

### 10.1 Multiple Admin Levels
```
admin       - Server owner (reserved)
moderator   - Elected/appointed (not reserved, but protected)
operator    - Channel ops (not reserved)
```

### 10.2 Admin Delegation
- Temporary admin rights via signed certificates
- Time-limited access
- Revocable

### 10.3 Admin Actions Log
- All admin commands logged to `admin.log`
- Immutable append-only log
- Audit trail for accountability

---

## 11. Implementation Checklist

- [ ] ReservedIdentity class with pattern matching
- [ ] Unicode normalization (ICU library?)
- [ ] Leet speak normalization
- [ ] ServerOwner class implementation
- [ ] UserStore registration filter
- [ ] Key generation and storage
- [ ] Admin command handlers
- [ ] TUI integration interface
- [ ] Configuration loading
- [ ] Error codes and messages
- [ ] Documentation updates

---

## Related Documents

- ircord-server-tui architecture
- Crypto identity management
- Server configuration system
