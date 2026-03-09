# IRCord Server — Arkkitehtuuri

**Stack:** C++20 · Boost.Asio · TLS 1.3 · Protobuf · SQLite
**Target:** Linux (aarch64 / RPi), systemd-service

---

## 1. Komponentit yhdellä silmäyksellä

```
┌─────────────────────────────────────────────────────────┐
│                    ircord-server                        │
│                                                         │
│  ┌──────────────┐   ┌──────────────────────────────┐   │
│  │  Listener    │   │       Session Manager        │   │
│  │  (TLS/TCP)   │──►│  map<UserId, Session>        │   │
│  └──────────────┘   └────────────┬─────────────────┘   │
│                                  │                      │
│         ┌───────────────┬────────┼──────────────────┐  │
│         ▼               ▼        ▼                  ▼  │
│  ┌────────────┐  ┌──────────┐  ┌────────────┐  ┌──────┐│
│  │  AuthSvc   │  │ChannelMgr│  │  KeyStore  │  │Voice ││
│  │ (challenge │  │(rooms,   │  │(pre-keys,  │  │Signal││
│  │  /identity)│  │ fanout)  │  │ identity)  │  │ Svc  ││
│  └────────────┘  └──────────┘  └────────────┘  └──────┘│
│         │               │            │               │  │
│         └───────────────┴────────────┴───────────────┘  │
│                              │                          │
│                    ┌─────────▼────────┐                 │
│                    │   SQLite (DB)    │                 │
│                    │  users, keys,    │                 │
│                    │  offline_msgs,   │                 │
│                    │  channels        │                 │
│                    └──────────────────┘                 │
└─────────────────────────────────────────────────────────┘
```

---

## 2. Thread Model

Boost.Asio **io_context + thread pool**, ei erillistä thread-per-connection.

```cpp
// Konfiguroitava, default: hardware_concurrency()
const int thread_count = std::max(2u, std::thread::hardware_concurrency());
asio::io_context ioc{thread_count};

// Thread pool
std::vector<std::thread> pool;
pool.reserve(thread_count);
for (int i = 0; i < thread_count; ++i)
    pool.emplace_back([&ioc] { ioc.run(); });
```

**RPi 4 (4 ydinä):** 4 io-threadia riittää helposti ~100 concurrent userille.

### Strands — thread safety ilman lockkeja

```
Jokainen Session omistaa oman strand:in:
  session->strand_ = asio::make_strand(ioc)

Jokainen Channel omistaa oman strand:in:
  channel->strand_ = asio::make_strand(ioc)

Fanout-viesti kanavalle:
  asio::post(channel->strand_, [=] { channel->broadcast(envelope); })
    └── channel->broadcast() iteroi jäsenet ja postaa jokaisen strand:iin:
          asio::post(session->strand_, [=] { session->send(envelope); })
```

Ei yhtään `std::mutex` message pathissa → zero contention hot pathissa.

---

## 3. Hakemistorakenne

```
ircord-server/
├── CMakeLists.txt
├── vcpkg.json
├── config/
│   └── server.toml.example
├── src/
│   ├── main.cpp
│   ├── server.hpp / .cpp          # io_context bootstrap, signal handling
│   ├── config.hpp / .cpp          # TOML config loader
│   │
│   ├── net/
│   │   ├── listener.hpp / .cpp    # TLS TCP acceptor
│   │   ├── session.hpp / .cpp     # Per-connection state machine
│   │   └── tls_context.hpp / .cpp # SSL context factory
│   │
│   ├── auth/
│   │   ├── auth_service.hpp / .cpp     # Challenge-response flow
│   │   └── identity_store.hpp / .cpp   # Public key lookup
│   │
│   ├── channel/
│   │   ├── channel.hpp / .cpp          # Room + member list + fanout
│   │   └── channel_manager.hpp / .cpp  # Global channel registry
│   │
│   ├── keys/
│   │   ├── key_store.hpp / .cpp        # Pre-key bundles, SPK rotation
│   │   └── key_service.hpp / .cpp      # Upload/fetch pre-keys
│   │
│   ├── presence/
│   │   └── presence_service.hpp / .cpp # Online/away/offline broadcast
│   │
│   ├── offline/
│   │   └── offline_queue.hpp / .cpp    # TTL-bounded message buffer
│   │
│   ├── voice/
│   │   └── voice_signal_service.hpp    # ICE candidate relay
│   │
│   ├── db/
│   │   ├── database.hpp / .cpp         # SQLite wrapper (SQLiteCpp)
│   │   └── migrations.hpp / .cpp       # Schema versioning
│   │
│   └── proto/                          # Shared kanssa client
│       └── ircord.proto
├── test/
│   ├── auth_test.cpp
│   ├── channel_test.cpp
│   └── session_test.cpp
└── deploy/
    ├── ircord-server.service           # systemd unit
    ├── Dockerfile                      # valinnainen
    └── update-ddns.sh                  # DynDNS päivitys
```

---

## 4. Wire Protocol

### Framing

Yksinkertainen length-prefixed framing TCP:n päällä:

```
┌──────────────┬────────────────────────┐
│  4 bytes     │  N bytes               │
│  (uint32 BE) │  Protobuf Envelope     │
│  payload len │                        │
└──────────────┴────────────────────────┘
```

Maksimi viestin koko: **64 KB** (enforce serverillä → disconnect jos ylitetään).
Tällä estetään memory exhaustion -hyökkäykset.

```cpp
// Async read: ensin 4-byte header, sitten payload
asio::async_read(socket_, asio::buffer(&len_buf_, 4),
    [this](auto ec, auto) {
        uint32_t len = ntohl(len_buf_);
        if (len > kMaxMessageSize) { disconnect(); return; }
        payload_buf_.resize(len);
        asio::async_read(socket_, asio::buffer(payload_buf_), ...);
    });
```

### Protobuf Schema

```protobuf
syntax = "proto3";
package ircord;

// ── Serverin näkemä kuori ──────────────────────────────
message Envelope {
  uint64 seq           = 1;  // monotoninen laskuri, replay detection
  uint64 timestamp_ms  = 2;  // unix epoch ms
  MessageType type     = 3;
  bytes  payload       = 4;  // tyypistä riippuva inner message
}

enum MessageType {
  MT_UNKNOWN          = 0;
  MT_HELLO            = 1;   // version negotiation
  MT_AUTH_CHALLENGE   = 2;
  MT_AUTH_RESPONSE    = 3;
  MT_AUTH_OK          = 4;
  MT_AUTH_FAIL        = 5;
  MT_CHAT_ENVELOPE    = 10;  // E2E-salattu blob — serveri ei avaa
  MT_KEY_UPLOAD       = 20;  // Pre-key bundle upload
  MT_KEY_REQUEST      = 21;  // Hae kaverin pre-key bundle
  MT_KEY_BUNDLE       = 22;  // Vastaus key requestiin
  MT_PRESENCE         = 30;
  MT_VOICE_SIGNAL     = 40;  // ICE candidate / SDP offer/answer
  MT_INVITE           = 50;  // Kutsu serverille
  MT_PING             = 90;
  MT_PONG             = 91;
  MT_ERROR            = 99;
}

// ── Auth ───────────────────────────────────────────────
message Hello {
  uint32 protocol_version = 1;  // tällä hetkellä 1
  string client_version   = 2;  // "ircord-client/0.1.0"
}

message AuthChallenge {
  bytes nonce = 1;  // 32 random bytes
}

message AuthResponse {
  string  user_id       = 1;  // nick tai UUID
  bytes   identity_pub  = 2;  // Ed25519 public key (32 bytes)
  bytes   signature     = 3;  // Ed25519 sign(nonce || user_id)
  bytes   signed_prekey = 4;  // X25519 SPK (ensimmäisessä rekisteröinnissä)
  bytes   spk_sig       = 5;  // Identity keyn allekirjoitus SPK:sta
}

// ── Chat relay ─────────────────────────────────────────
message ChatEnvelope {
  string sender_id     = 1;
  string recipient_id  = 2;  // user_id tai "#channel_name"
  bytes  ciphertext    = 3;  // Signal Protocol ciphertext — serveri ei avaa
  uint32 ciphertext_type = 4; // WHISPER_MESSAGE=1, PRE_KEY_MESSAGE=3
}

// ── Key distribution ───────────────────────────────────
message KeyBundle {
  bytes  identity_pub       = 1;  // Ed25519 public key
  bytes  signed_prekey      = 2;  // X25519 SPK
  bytes  spk_signature      = 3;  // sig(identity_priv, SPK)
  uint32 spk_id             = 4;
  bytes  one_time_prekey    = 5;  // X25519 OPK (poistetaan käytön jälkeen)
  uint32 opk_id             = 6;
}

message KeyUpload {
  bytes              signed_prekey     = 1;
  bytes              spk_signature     = 2;
  uint32             spk_id           = 3;
  repeated bytes     one_time_prekeys  = 4;
  repeated uint32    opk_ids          = 5;
}

// ── Presence ───────────────────────────────────────────
message PresenceUpdate {
  string user_id = 1;
  enum Status { OFFLINE = 0; ONLINE = 1; AWAY = 2; }
  Status status  = 2;
}

// ── Voice signaling ────────────────────────────────────
message VoiceSignal {
  string from_user       = 1;
  string to_user         = 2;   // tai "#channel" voice roomille
  enum SignalType {
    OFFER         = 0;
    ANSWER        = 1;
    ICE_CANDIDATE = 2;
    CALL_INVITE   = 3;
    CALL_ACCEPT   = 4;
    CALL_REJECT   = 5;
    CALL_HANGUP   = 6;
  }
  SignalType signal_type = 3;
  bytes      sdp_or_ice  = 4;   // JSON-serialized SDP tai ICE candidate
}

// ── Error ──────────────────────────────────────────────
message Error {
  uint32 code    = 1;
  string message = 2;
}
```

---

## 5. Session State Machine

```
          CONNECTED (TLS OK)
               │
               ▼
         ┌──────────┐    hello mismatch    ┌─────────────┐
         │  HELLO   │──────────────────────► DISCONNECTED │
         └────┬─────┘                      └─────────────┘
              │ version OK
              ▼
      ┌───────────────┐   invalid sig      ┌─────────────┐
      │  AUTH_PENDING │──────────────────► │ DISCONNECTED │
      │ (nonce sent)  │                    └─────────────┘
      └───────┬───────┘
              │ sig valid
              ▼
      ┌───────────────┐
      │  ESTABLISHED  │◄─────────────────────────────────┐
      │               │                                  │
      │  PING/PONG    │  30s timer → PING                │
      │  MSG relay    │  60s no PONG → disconnect        │
      │  KEY ops      │                                  │
      │  VOICE signal │──────────────────────────────────┘
      └───────────────┘
```

```cpp
class Session : public std::enable_shared_from_this<Session> {
public:
    enum class State { Handshake, Hello, AuthPending, Established, Dead };

    explicit Session(asio::ssl::stream<tcp::socket> sock,
                     asio::strand<asio::io_context::executor_type> strand,
                     ServerContext& ctx);

    void start();      // Käynnistä TLS handshake → async loop
    void send(const Envelope& env);
    void disconnect(std::string_view reason = "");

private:
    void do_read_frame();
    void on_frame(std::span<const uint8_t> data);
    void handle_hello(const Hello&);
    void handle_auth_response(const AuthResponse&);
    void handle_chat_envelope(const ChatEnvelope&);
    void handle_key_upload(const KeyUpload&);
    void handle_key_request(const KeyRequest&);
    void handle_presence(const PresenceUpdate&);
    void handle_voice_signal(const VoiceSignal&);
    void start_ping_timer();

    asio::ssl::stream<tcp::socket> socket_;
    asio::strand<asio::io_context::executor_type> strand_;
    ServerContext& ctx_;       // shared: ChannelMgr, KeyStore, DB, ...
    State state_ = State::Handshake;
    std::string user_id_;
    std::array<uint8_t, 4> len_buf_{};
    std::vector<uint8_t> payload_buf_;
    asio::steady_timer ping_timer_;
    uint64_t seq_counter_ = 0;
};
```

---

## 6. Channel ja Fanout

```cpp
class Channel {
public:
    explicit Channel(std::string id, asio::io_context& ioc);

    void add_member(std::shared_ptr<Session> s);
    void remove_member(const std::string& user_id);

    // Fanout: relay E2E-salattu blob kaikille jäsenille paitsi lähettäjälle
    void broadcast(const ChatEnvelope& env,
                   const std::string& exclude_user);

private:
    std::string id_;
    asio::strand<asio::io_context::executor_type> strand_;
    // Strand suojaa members_-mutteja — ei lockkeja tarvita
    std::unordered_map<std::string, std::weak_ptr<Session>> members_;
};
```

**Fanout-järjestys:**

```
Client A lähettää #general -kanavalle
  → Session A:n strand: parse Envelope, tunnista kanava
  → post(channel->strand_):
      channel->broadcast(env, "A")
        → iteroi members_: [B, C, D]
        → post(session_B->strand_): session_B->send(env)
        → post(session_C->strand_): session_C->send(env)
        → post(session_D->strand_): session_D->send(env)
```

Kaikki asynkronista, ei blocking.

---

## 7. Tietokanta — SQLite Schema

```sql
-- Käyttäjät ja identity keyt
CREATE TABLE users (
    user_id         TEXT PRIMARY KEY,     -- nick tai UUID
    identity_pub    BLOB NOT NULL,         -- Ed25519 pubkey (32 bytes)
    registered_at   INTEGER NOT NULL,
    last_seen_at    INTEGER
);

-- Signed Pre-Keys (yksi per käyttäjä kerrallaan)
CREATE TABLE signed_prekeys (
    user_id         TEXT NOT NULL REFERENCES users(user_id),
    spk_id          INTEGER NOT NULL,
    spk_pub         BLOB NOT NULL,         -- X25519 pubkey
    spk_signature   BLOB NOT NULL,         -- identity allekirjoittama
    uploaded_at     INTEGER NOT NULL,
    PRIMARY KEY (user_id, spk_id)
);

-- One-Time Pre-Keys (kulutetaan X3DH:ssa)
CREATE TABLE one_time_prekeys (
    user_id         TEXT NOT NULL REFERENCES users(user_id),
    opk_id          INTEGER NOT NULL,
    opk_pub         BLOB NOT NULL,
    used            INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (user_id, opk_id)
);

-- Kanavat
CREATE TABLE channels (
    channel_id      TEXT PRIMARY KEY,     -- "#general"
    created_at      INTEGER NOT NULL,
    created_by      TEXT REFERENCES users(user_id)
);

-- Kanavan jäsenet
CREATE TABLE channel_members (
    channel_id      TEXT REFERENCES channels(channel_id),
    user_id         TEXT REFERENCES users(user_id),
    joined_at       INTEGER NOT NULL,
    PRIMARY KEY (channel_id, user_id)
);

-- Offline-viestit (TTL: 7 päivää)
CREATE TABLE offline_messages (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    recipient_id    TEXT NOT NULL,
    sender_id       TEXT NOT NULL,
    ciphertext      BLOB NOT NULL,
    ciphertext_type INTEGER NOT NULL,
    created_at      INTEGER NOT NULL
);
CREATE INDEX idx_offline_recipient ON offline_messages(recipient_id, created_at);

-- Kutsut (invite codes serverille)
CREATE TABLE invites (
    code            TEXT PRIMARY KEY,
    created_by      TEXT REFERENCES users(user_id),
    used_by         TEXT REFERENCES users(user_id),
    created_at      INTEGER NOT NULL,
    expires_at      INTEGER NOT NULL,
    used_at         INTEGER
);
```

**Offline-viestien siivous:**

```cpp
// Aja kerran tunnissa SQLite-threadin kautta
db_.execute(
    "DELETE FROM offline_messages "
    "WHERE created_at < ?",
    unix_now() - 7 * 24 * 3600
);
```

---

## 8. TLS Setup

Serveri käyttää **self-signed sertifikaattia** sisäiseen käyttöön,
tai Let's Encryptiä jos DynDNS-nimi on käytössä.

```cpp
asio::ssl::context make_tls_context(const Config& cfg) {
    asio::ssl::context ctx{asio::ssl::context::tls_server};

    // Vain TLS 1.3
    ctx.set_options(
        asio::ssl::context::default_workarounds |
        asio::ssl::context::no_sslv2  |
        asio::ssl::context::no_sslv3  |
        asio::ssl::context::no_tlsv1  |
        asio::ssl::context::no_tlsv1_1|
        asio::ssl::context::no_tlsv1_2
    );

    ctx.use_certificate_chain_file(cfg.tls.cert_file);
    ctx.use_private_key_file(cfg.tls.key_file, asio::ssl::context::pem);

    // Rajoita cipher suiteja (TLS 1.3:ssa serverit eivät voi valita,
    // mutta varmuuden vuoksi):
    SSL_CTX_set_ciphersuites(ctx.native_handle(),
        "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256");

    return ctx;
}
```

**Sertifikaatin generointi (self-signed):**

```bash
openssl req -x509 -newkey ed25519 \
  -keyout server.key -out server.crt \
  -days 3650 -nodes \
  -subj "/CN=ircord-server"

# Client pinnataan tähän sertifikaattiin (certificate pinning)
# → MITM-hyökkäys ei onnistu vaikka CA:ta ei käytetä
```

Clientit tekevät **certificate pinning**: tallentavat serverin julkisen
avaimen ensimmäisellä yhteydellä ja tarkistavat sen joka kerta.
Ei tarvita CA-ketjua.

---

## 9. Config (TOML)

```toml
[server]
host     = "0.0.0.0"
port     = 6667           # IRC:n historiallinen portti :)
log_level = "info"        # debug, info, warn, error

[tls]
cert_file = "/etc/ircord/server.crt"
key_file  = "/etc/ircord/server.key"

[database]
path      = "/var/lib/ircord/ircord.db"

[limits]
max_connections   = 100
max_message_bytes = 65536    # 64 KB
max_offline_msgs  = 500      # per user
offline_ttl_days  = 7
ping_interval_sec = 30
ping_timeout_sec  = 60
opk_low_watermark = 10       # Client lähettää uusia kun alle tämän

[voice]
stun_server = "stun.example.com:3478"   # tai julkinen STUN

[invite]
require_invite = true   # Suljettu serveri — vain kutsulla
```

---

## 10. RPi-spesifiset asiat

### Käännös aarch64

```cmake
# CMakeLists.txt — RPi 4
set(CMAKE_CXX_STANDARD 20)

# Optimointi ARM:lle
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    add_compile_options(-march=armv8-a -mtune=cortex-a72)
endif()

# vcpkg triplet: arm64-linux
# cmake -DVCPKG_TARGET_TRIPLET=arm64-linux ..
```

Cross-compile kehityskoneelta jos kääntäminen RPi:llä on liian hidasta:

```bash
# Docker-pohjainen cross-compile
docker run --rm -v $(pwd):/src \
  ghcr.io/cross-rs/aarch64-unknown-linux-gnu:latest \
  cmake --build /src/build
```

### systemd Service

```ini
# /etc/systemd/system/ircord-server.service
[Unit]
Description=IRCord Chat Server
After=network.target
Wants=network.target

[Service]
Type=simple
User=ircord
Group=ircord
ExecStart=/usr/local/bin/ircord-server --config /etc/ircord/server.toml
Restart=on-failure
RestartSec=5
# Muistiraja RPi:lle — estää OOM koko systeemin
MemoryMax=256M
# Ei oikeuksia tarpeen ulkopuolelle
PrivateTmp=true
NoNewPrivileges=true
ProtectSystem=strict
ReadWritePaths=/var/lib/ircord

[Install]
WantedBy=multi-user.target
```

### DynDNS (kotipalvelimen dynaaminen IP)

```bash
#!/bin/bash
# deploy/update-ddns.sh — aja cronista 5 min välein

DOMAIN="ircord.sinundomainisi.fi"
API_TOKEN="cloudflare_api_token"
ZONE_ID="cloudflare_zone_id"
RECORD_ID="cloudflare_record_id"

CURRENT_IP=$(curl -s https://api.ipify.org)

curl -s -X PATCH \
  "https://api.cloudflare.com/client/v4/zones/$ZONE_ID/dns_records/$RECORD_ID" \
  -H "Authorization: Bearer $API_TOKEN" \
  -H "Content-Type: application/json" \
  --data "{\"content\":\"$CURRENT_IP\",\"type\":\"A\",\"name\":\"$DOMAIN\"}"
```

Vaihtoehto: **DuckDNS** (ilmainen, yksinkertaisempi, ei tarvitse omaa domainia).

---

## 11. Dependencies (vcpkg.json)

```json
{
  "name": "ircord-server",
  "version-string": "0.1.0",
  "dependencies": [
    "boost-asio",
    "boost-system",
    "openssl",
    "protobuf",
    "sqlitecpp",
    "toml11",
    "spdlog",
    "catch2"
  ]
}
```

---

## 12. Toteutusjärjestys (server-first)

```
Vaihe 1 — Skeleton (1 vko)
  ✦ CMake + vcpkg setup, aarch64-triplet
  ✦ Config loader (toml11)
  ✦ Logger (spdlog)
  ✦ Listener + TLS accept
  ✦ Session: frame read/write loop
  ✦ Hello + version check → disconnect jos mismatch

Vaihe 2 — Auth + DB (1 vko)
  ✦ SQLite schema + migrations
  ✦ Ed25519 challenge-response (libsodium)
  ✦ Rekisteröinti: tallenna identity pub + SPK
  ✦ Kirjautuminen: tarkista sig, luo session

Vaihe 3 — Chat relay (1 vko)
  ✦ Channel manager: join/part/list
  ✦ ChatEnvelope fanout (strand-malli)
  ✦ 1:1 viestit user_id:n mukaan
  ✦ Offline queue: puskuroi, toimita reconnectissa

Vaihe 4 — Key distribution (3-4 pv)
  ✦ KeyUpload: tallenna OPK:t ja SPK
  ✦ KeyRequest: palauta bundle + kuluta OPK
  ✦ OPK low-watermark ilmoitus clientille

Vaihe 5 — Presence + Ping (2-3 pv)
  ✦ Presence broadcast kanavajäsenille
  ✦ PING/PONG + disconnect timeout

Vaihe 6 — Voice signaling (2-3 pv)
  ✦ VoiceSignal relay: OFFER/ANSWER/ICE forward
  ✦ CALL_INVITE → CALL_ACCEPT/REJECT

Vaihe 7 — Hardening
  ✦ Rate limiting per session
  ✦ Max OPK count per user
  ✦ Input validation kaikilla kentillä
  ✦ Fuzz testing (protobuf deserialisointi)
  ✦ systemd service + deploy skriptit
```
