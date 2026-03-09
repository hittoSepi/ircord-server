# IRCord Client — Arkkitehtuuri

**Stack:** C++20 · Boost.Asio · notcurses · libsignal-protocol-c · libsodium
          · libdatachannel · libopus · miniaudio · Protobuf · SQLite · libcurl

---

## 1. Komponentit yhdellä silmäyksellä

```
┌───────────────────────────────────────────────────────────────────┐
│                        ircord-client                              │
│                                                                   │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │                        UI Layer                             │ │
│  │   notcurses · InputHandler · CommandParser · LinkPreviewer  │ │
│  └────────────────────────────┬────────────────────────────────┘ │
│                               │  AppState (thread-safe)          │
│         ┌─────────────────────┼──────────────────────┐           │
│         ▼                     ▼                      ▼           │
│  ┌─────────────┐   ┌──────────────────┐   ┌──────────────────┐  │
│  │  NetClient  │   │  CryptoEngine    │   │  VoiceEngine     │  │
│  │ (Boost.Asio │   │ (Signal Protocol │   │ (libdatachannel  │  │
│  │  TLS/TCP)   │   │  + libsodium)    │   │  + Opus          │  │
│  └──────┬──────┘   └────────┬─────────┘   │  + miniaudio)    │  │
│         │                   │             └──────────────────┘  │
│         └─────────┬─────────┘                                    │
│                   ▼                                              │
│         ┌──────────────────┐                                     │
│         │   LocalStore     │                                     │
│         │   (SQLite)       │                                     │
│         │  sessions, keys, │                                     │
│         │  message history │                                     │
│         └──────────────────┘                                     │
└───────────────────────────────────────────────────────────────────┘
```

---

## 2. Thread Model

```
┌─────────────────────────────────────────────────────┐
│  Main Thread                                        │
│   notcurses render loop + input polling             │
│   → käsittelee InputEvent:it                        │
│   → renderöi AppStaten muutokset                    │
│   → ainoa säie joka kutsuu notcurses-funktiota      │
└────────────────────┬────────────────────────────────┘
                     │ post() / AppState
         ┌───────────┼───────────────┐
         ▼           ▼               ▼
┌──────────────┐ ┌──────────┐ ┌───────────────────────┐
│  IO Thread   │ │ Preview  │ │  Audio Thread          │
│ Boost.Asio   │ │ Thread   │ │  (miniaudio callback,  │
│ io_context   │ │ libcurl  │ │   realtime priority)   │
│              │ │ OG fetch │ │                        │
│ - TLS recv   │ └──────────┘ │ capture → Opus encode  │
│ - TLS send   │              │ Opus decode → playback │
│ - reconnect  │              │ jitter buffer          │
└──────────────┘              └───────────────────────┘
```

**Kriittinen sääntö:** Ainoastaan Main Thread kutsuu notcurses-funktioita.
Kaikki muiden threadien UI-päivitykset menevät `AppState`:n kautta ja
Main Thread pollataan tai herätetään signal-kanavaa pitkin.

```cpp
// Muiden threadien tapa pyytää UI-päivitystä
app_state_.post_ui([msg = std::move(decrypted)] {
    // Tätä ajetaan main threadissä seuraavassa render-loopissa
    app_state_.add_message(msg);
});
```

---

## 3. Hakemistorakenne

```
ircord-client/
├── CMakeLists.txt
├── vcpkg.json
├── src/
│   ├── main.cpp
│   ├── app.hpp / .cpp              # Top-level: init, event loop, shutdown
│   │
│   ├── state/
│   │   ├── app_state.hpp / .cpp    # Kaikki sovellustila, thread-safe
│   │   ├── channel_state.hpp       # Viestit, scroll, unread count
│   │   └── voice_state.hpp         # Kuka puhuu, mute/deafen
│   │
│   ├── ui/
│   │   ├── ui_manager.hpp / .cpp   # notcurses init + render loop
│   │   ├── layout.hpp / .cpp       # Plane layout (tab bar, msg area, input)
│   │   ├── message_view.hpp / .cpp # Viestien renderöinti + scroll
│   │   ├── input_line.hpp / .cpp   # Input + historia + tab-complete
│   │   ├── tab_bar.hpp / .cpp      # Kanavatabit + unread indikaattorit
│   │   ├── status_bar.hpp / .cpp   # Voice participants + online users
│   │   ├── link_preview.hpp / .cpp # Inline preview box -renderöinti
│   │   └── color_scheme.hpp        # Väripaletti (true color)
│   │
│   ├── net/
│   │   ├── net_client.hpp / .cpp   # TLS TCP, frame read/write, reconnect
│   │   └── message_handler.hpp     # Envelope → oikea handler
│   │
│   ├── crypto/
│   │   ├── crypto_engine.hpp / .cpp    # Signal Protocol sessiot
│   │   ├── identity.hpp / .cpp         # Key gen, export/import, pinning
│   │   ├── signal_store.hpp / .cpp     # SQLite-backed Signal stores
│   │   └── group_session.hpp / .cpp    # Sender Key -ryhmäsessiot
│   │
│   ├── voice/
│   │   ├── voice_engine.hpp / .cpp     # Peer connections, track mgmt
│   │   ├── audio_device.hpp / .cpp     # miniaudio capture + playback
│   │   ├── opus_codec.hpp / .cpp       # libopus RAII wrapper
│   │   └── jitter_buffer.hpp / .cpp    # Packet reorder + playout delay
│   │
│   ├── preview/
│   │   └── link_previewer.hpp / .cpp   # Async OG metadata fetch
│   │
│   ├── db/
│   │   ├── local_store.hpp / .cpp      # SQLite wrapper
│   │   └── migrations.hpp / .cpp
│   │
│   ├── input/
│   │   ├── input_handler.hpp / .cpp    # notcurses ncinput → InputEvent
│   │   ├── command_parser.hpp / .cpp   # /join, /voice, /call, ...
│   │   └── tab_complete.hpp / .cpp     # Nick + channel + command complete
│   │
│   └── proto/                          # Shared kanssa serverin kanssa
│       └── ircord.proto
│
├── test/
└── resources/
    └── default_colors.toml
```

---

## 4. AppState — Yhteinen Totuus

Kaikki sovellustila on `AppState`:ssa. Se on ainoa jaettu rakenne
threadien välillä, ja se suojaa itsensä sisäisesti.

```cpp
class AppState {
public:
    // ── Channels ──────────────────────────────────────────────
    void add_channel(std::string id);
    void remove_channel(const std::string& id);
    void set_active_channel(const std::string& id);
    std::string active_channel() const;

    void push_message(const std::string& channel, Message msg);
    std::span<const Message> messages(const std::string& channel) const;
    void mark_read(const std::string& channel);
    int  unread_count(const std::string& channel) const;

    // ── Users ─────────────────────────────────────────────────
    void set_online(const std::string& user_id, PresenceStatus status);
    std::vector<std::string> online_users() const;

    // ── Voice ─────────────────────────────────────────────────
    void set_voice_participants(std::vector<std::string> users);
    bool is_muted() const;
    void set_muted(bool);
    bool is_deafened() const;
    void set_deafened(bool);

    // ── UI notify ─────────────────────────────────────────────
    // Muut threadit postaa tänne, main thread ajaa render-loopissa
    void post_ui(std::function<void()> fn);
    void drain_ui_queue(); // kutsutaan main threadissä

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, ChannelState> channels_;
    std::string active_channel_;
    std::unordered_map<std::string, PresenceStatus> online_users_;
    VoiceState voice_state_;

    std::mutex ui_queue_mu_;
    std::vector<std::function<void()>> ui_queue_;
};
```

---

## 5. UI — notcurses Layout

### Plane-rakenne

```
┌─ notcurses stdplane (koko terminaali) ──────────────────────────────┐
│                                                                     │
│ ┌─ tab_plane (1 rivi, top) ───────────────────────────────────────┐ │
│ │ #general  #random [3] #dev  🔊general-voice                    │ │
│ └─────────────────────────────────────────────────────────────────┘ │
│                                                                     │
│ ┌─ msg_plane (keski, scrollable) ────────────────────────────────┐ │
│ │ [21:33] <Matti>  Kattokaas: https://example.com               │ │
│ │         ┌──────────────────────────────────────┐              │ │
│ │         │ 🔗 Example Site · Cool article...    │  ← preview  │ │
│ │         └──────────────────────────────────────┘              │ │
│ │ [21:34] <Teppo>  nice                                         │ │
│ │ [21:35] <Sepi>   jep                                          │ │
│ └────────────────────────────────────────────────────────────────┘ │
│                                                                     │
│ ┌─ status_plane (1-2 riviä) ─────────────────────────────────────┐ │
│ │ 🔊 Matti, Teppo  │  🟢 Matti  Teppo  Sepi  Pekka             │ │
│ └─────────────────────────────────────────────────────────────────┘ │
│                                                                     │
│ ┌─ input_plane (1 rivi, bottom) ─────────────────────────────────┐ │
│ │ > _                                                            │ │
│ └─────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
```

### notcurses Plane -hierarkia

```cpp
class UIManager {
    struct nc_deleter {
        void operator()(notcurses* nc) { notcurses_stop(nc); }
    };
    std::unique_ptr<notcurses, nc_deleter> nc_;

    // Planen elinkaari sidottu UIManageriin
    ncplane* std_plane_;   // stdplane, ei omista
    ncplane* tab_plane_;   // 1 rivi, top
    ncplane* msg_plane_;   // keski — scroll tapahtuu tässä
    ncplane* status_plane_;// 1-2 riviä
    ncplane* input_plane_; // bottom

    void rebuild_layout(int term_rows, int term_cols);
    void handle_resize();   // SIGWINCH → rebuild_layout
};
```

### Väripaletti (true color)

```cpp
// color_scheme.hpp
struct ColorScheme {
    uint32_t bg              = 0x1a1b26;  // Tokyo Night bg
    uint32_t fg              = 0xc0caf5;
    uint32_t timestamp       = 0x565f89;  // himmeä
    uint32_t nick_self       = 0x7aa2f7;  // sininen
    uint32_t nick_other      = 0x9ece6a;  // vihreä (tai hash-pohjainen)
    uint32_t nick_system     = 0xe0af68;  // oranssi system-viesteille
    uint32_t unread_badge    = 0xf7768e;  // punainen
    uint32_t active_tab      = 0x7aa2f7;
    uint32_t inactive_tab    = 0x565f89;
    uint32_t preview_border  = 0x3b4261;
    uint32_t preview_title   = 0x7aa2f7;
    uint32_t voice_active    = 0x9ece6a;
    uint32_t muted           = 0xf7768e;
    uint32_t input_cursor    = 0xc0caf5;
    uint32_t encrypt_lock    = 0x9ece6a;  // 🔒 E2E OK
};

// Nick-väri hash-pohjaisesti — jokaisella kaverilla oma väri
uint32_t nick_color(std::string_view nick) {
    auto h = std::hash<std::string_view>{}(nick);
    static constexpr uint32_t palette[] = {
        0x7aa2f7, 0x9ece6a, 0xe0af68, 0xbb9af7,
        0x7dcfff, 0xf7768e, 0x73daca, 0xff9e64
    };
    return palette[h % std::size(palette)];
}
```

---

## 6. Input & Komennot

### InputHandler

```cpp
// notcurses antaa ncinput-structin per event
// → muunnetaan InputEvent-tyypeiksi → command parser / tekstiinsertio

struct InputEvent {
    enum class Type {
        Char,           // printattava merkki
        Backspace,
        Enter,
        Tab,            // tab complete
        ArrowUp,        // historia
        ArrowDown,
        ArrowLeft,
        ArrowRight,
        CtrlC,          // quit
        CtrlW,          // poista sana
        CtrlN,          // seuraava kanava
        CtrlP,          // edellinen kanava
        PageUp,         // scroll viestejä
        PageDown,
        Resize,         // SIGWINCH
    };
    Type type;
    char32_t codepoint = 0;   // Type::Char tapauksessa
};
```

### CommandParser

```cpp
// Kaikki / -alkuiset syötteet
struct ParsedCommand {
    std::string name;
    std::vector<std::string> args;
};

// Komennot ja niiden minimiargumentit:
static const std::unordered_map<std::string, int> kCommands = {
    {"join",    1},   // /join #kanava
    {"part",    0},   // /part [syy]
    {"msg",     2},   // /msg <nick> <viesti>
    {"voice",   0},   // /voice  — liity/poistu voice roomista
    {"call",    1},   // /call <nick>
    {"hangup",  0},
    {"mute",    0},
    {"deafen",  0},
    {"nick",    1},
    {"invite",  1},
    {"trust",   1},   // Safety Number verification
    {"keys",    0},
    {"whois",   1},
    {"set",     2},
    {"quit",    0},
    {"help",    0},
    {"clear",   0},
};
```

### Tab Complete

```cpp
class TabCompleter {
public:
    // Täydentää /komennot, #kanavat, @nickit
    // "sepi" → "Sepimorph", "/jo" → "/join", "#ge" → "#general"
    std::string complete(std::string_view partial,
                         std::span<const std::string> online_users,
                         std::span<const std::string> channels);
    void reset();  // kun käyttäjä muokkaa inputtia

private:
    std::vector<std::string> candidates_;
    size_t idx_ = 0;
};
```

### Input Line — Historia

```cpp
class InputLine {
    static constexpr size_t kHistoryMax = 200;
    std::deque<std::string> history_;
    int hist_pos_ = -1;  // -1 = ei selata historiaa
    std::string current_; // nykyinen muokattu teksti
    std::u32string buf_;  // unicode-tietoinen buffer
    size_t cursor_ = 0;
};
```

---

## 7. Network Client

Sama Boost.Asio -malli kuin serverissä, mutta clientin logiikka.

```cpp
class NetClient {
public:
    // Yhdistä, autentikoi, käynnistä read loop
    asio::awaitable<void> connect(std::string host, uint16_t port);

    // Lähettää Envelope:n — voi kutsua mistä threadista tahansa
    void send(Envelope env);

    // Reconnect-logiikka: exp. backoff 1s → 2s → 4s → ... → 60s
    void on_disconnect();

private:
    asio::awaitable<void> do_read_loop();
    asio::awaitable<void> do_auth();
    void dispatch(const Envelope& env);  // → MessageHandler

    asio::io_context& ioc_;
    asio::ssl::stream<tcp::socket> socket_;
    asio::steady_timer reconnect_timer_;

    // Send queue — thread-safe
    asio::strand<asio::io_context::executor_type> send_strand_;
    std::deque<std::vector<uint8_t>> send_queue_;
};
```

**Reconnect-flow:**
```
yhteys katkeaa
  → wait(backoff)   // 1s, 2s, 4s, 8s... max 60s
  → connect()
  → TLS handshake
  → auth()
  → server lähettää offline_messages
  → UI: "Reconnected ✓"
```

**Certificate Pinning:**
```cpp
// Ensimmäinen yhteys: tallenna serverin cert fingerprint
// Myöhemmät yhteydet: vertaa
SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, [](int ok, X509_STORE_CTX* ctx) {
    auto cert = X509_STORE_CTX_get0_cert(ctx);
    // laske SHA-256 fingerprint
    // vertaa tallennettuun ~/.config/ircord/server_cert.pin
    return pinned_matches(cert) ? 1 : 0;
});
```

---

## 8. CryptoEngine — Signal Protocol

### Initialisointi

```cpp
class CryptoEngine {
public:
    void init(LocalStore& db, const Config& cfg);

    // Ensimmäinen käynnistys: generoi identity keypair
    void generate_identity();

    // Rekisteröinti: luo N one-time pre-keyä, lähettää serverille
    KeyUpload prepare_registration(int num_opks = 100);

    // Salaus: hae tai luo sessio → encrypt
    std::vector<uint8_t> encrypt(const std::string& recipient_id,
                                  std::span<const uint8_t> plaintext);

    // Purku: decrypt → plaintext
    std::vector<uint8_t> decrypt(const std::string& sender_id,
                                  std::span<const uint8_t> ciphertext,
                                  uint32_t ciphertext_type);

    // Ryhmä (Sender Keys)
    void init_group_session(const std::string& channel_id,
                            std::span<const std::string> members);
    std::vector<uint8_t> encrypt_group(const std::string& channel_id,
                                        std::span<const uint8_t> plaintext);
    std::vector<uint8_t> decrypt_group(const std::string& sender_id,
                                        const std::string& channel_id,
                                        std::span<const uint8_t> ciphertext);

    // Safety Number (human-verifiable fingerprint)
    std::string safety_number(const std::string& peer_id) const;

    // Ed25519 sign (auth)
    std::vector<uint8_t> sign_challenge(std::span<const uint8_t> nonce) const;

private:
    signal_context* signal_ctx_ = nullptr;
    SignalStore store_;  // SQLite-backed: session, pre-key, signed-pre-key stores
    identity_key_pair identity_;
};
```

### SQLite-backed Signal Stores

libsignal-protocol-c vaatii 5 store-interfacea jotka on toteutettava:

```cpp
// Jokainen store on thin wrapper SQLiten päälle
class SignalStore {
public:
    // session_store_t — tallennetaan session per (user_id, device_id)
    signal_protocol_session_store session_store();

    // pre_key_store_t — one-time pre-keyt
    signal_protocol_pre_key_store pre_key_store();

    // signed_pre_key_store_t
    signal_protocol_signed_pre_key_store signed_pre_key_store();

    // identity_key_store_t — identity keyt kavereille
    signal_protocol_identity_key_store identity_key_store();

    // sender_key_store_t — ryhmä Sender Keyt
    signal_protocol_sender_key_store sender_key_store();
};
```

### Safety Number UI

```
/trust Matti
┌─────────────────────────────────────────────────────────────┐
│  Safety Number — Sinä ↔ Matti                               │
│                                                             │
│  05820 27193  83627 49201  71920 38471                      │
│  62910 48271  93827 10492  82736 19204                      │
│                                                             │
│  Vertaa tätä numeroa Matin kanssa puhelimessa tai IRL.      │
│  Jos numerot täsmäävät, yhteys on turvallinen.              │
│                                                             │
│  [V]erify  [C]ancel                                         │
└─────────────────────────────────────────────────────────────┘
```

---

## 9. VoiceEngine

### Komponentit

```cpp
class VoiceEngine {
public:
    // Liity kanavan voice roomiin
    asio::awaitable<void> join_room(const std::string& channel_id);
    asio::awaitable<void> leave_room();

    // 1:1 puhelu
    asio::awaitable<void> call(const std::string& peer_id);
    asio::awaitable<void> accept_call(const std::string& caller_id);
    void hangup();

    // Ohjaus
    void set_muted(bool);
    void set_deafened(bool);

    // ICE signaling: serveri lähettää SDP/candidate → tänne
    void on_voice_signal(const VoiceSignal&);

private:
    // Per-peer yhteys
    struct PeerConn {
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::Track> track;
        JitterBuffer jitter;
    };
    std::unordered_map<std::string, PeerConn> peers_;

    AudioDevice audio_device_;
    OpusCodec   encoder_;   // yksi per instanssi (encode vain kerran)
    std::unordered_map<std::string, OpusCodec> decoders_; // per peer

    NetClient& net_;   // ICE candidaatit välitetään serverin kautta
};
```

### Audio Pipeline

```
CAPTURE SIDE (audio callback thread, high priority):
  miniaudio capture callback (5ms frames @ 48kHz)
    → if muted: skip
    → VAD check (WebRTC VAD tai yksinkertainen RMS threshold)
    → if silence: DTX (discontinuous transmission) — ei lähetystä
    → Opus encode: PCM → compressed frame (~20-100 bytes @ 64kbps)
    → for each PeerConn:
         encrypt frame (DTLS-SRTP via libdatachannel)
         → send UDP

PLAYBACK SIDE (audio callback thread):
  for each PeerConn:
    JitterBuffer::pop(frame)   // kokoaa out-of-order paketit
      → if missing: PLC (Packet Loss Concealment, Opus built-in)
    Opus decode: compressed → PCM
  mix all peer PCM streams (simple additive mix, clamp)
  → miniaudio playback callback
```

### JitterBuffer

```cpp
class JitterBuffer {
    // Ring buffer jossa paikka seq-numeron mukaan
    // Target delay: 40ms (dynaaminen: kasvaa jos paljon jitter)
    static constexpr size_t kBufSize = 64;   // pakettipaikkaa

    std::array<std::optional<AudioFrame>, kBufSize> slots_;
    uint32_t write_seq_ = 0;
    uint32_t read_seq_  = 0;
    size_t   target_delay_ms_ = 40;

public:
    void push(uint32_t seq, AudioFrame frame);
    std::optional<AudioFrame> pop();  // nullptr → PLC tarvitaan
};
```

### libdatachannel Setup

```cpp
// Luo PeerConnection kaveria varten
rtc::Configuration rtc_cfg;
rtc_cfg.iceServers = {rtc::IceServer{"stun:stun.l.google.com:19302"}};
// + serverin oma STUN jos sellainen on

auto pc = std::make_shared<rtc::PeerConnection>(rtc_cfg);

// ICE kandidaatit → serverin kautta kaverille
pc->onLocalCandidate([&](rtc::Candidate c) {
    VoiceSignal sig;
    sig.set_signal_type(VoiceSignal::ICE_CANDIDATE);
    sig.set_sdp_or_ice(c.candidate());
    net_.send(wrap(sig));
});

// Audio track (Opus / RTP)
rtc::Description::Audio audio_desc("audio", rtc::Description::Direction::SendRecv);
audio_desc.addOpusCodec(111);
auto track = pc->addTrack(audio_desc);

track->onFrame([&](rtc::binary frame, rtc::FrameInfo info) {
    // Saapuva RTP-paketti → jitter buffer
    peers_[peer_id].jitter.push(info.timestamp, {frame.begin(), frame.end()});
});
```

---

## 10. LinkPreviewer

Täysin asynkroninen, ei koskaan blokkaa UI:ta.

```cpp
class LinkPreviewer {
public:
    struct Preview {
        std::string url;
        std::string title;
        std::string description;  // max 120 merkkiä, truncated
        // thumbnail: ei TUI:ssa toistaiseksi (sixel olisi hauska bonus)
    };

    using Callback = std::function<void(Preview)>;

    // Kutsutaan IO-threadistä kun viesti sisältää URLin
    // Fetches async, kutsuu cb main threadissä kun valmis
    void fetch(std::string url, Callback cb);

private:
    // libcurl multi handle — ei-blocking
    CURLM* curl_multi_ = nullptr;

    // Välimuisti: URL → Preview, max 200 entryä, LRU eviction
    struct CacheEntry { Preview preview; std::chrono::steady_clock::time_point ts; };
    std::unordered_map<std::string, CacheEntry> cache_;

    // OG-tagien parsinta (minimaali regex-pohjainen)
    std::optional<Preview> parse_og(std::string_view html, std::string_view url);
};
```

**Turvallisuus — mitä fetchataan:**
- Vain `http://` ja `https://` — ei `file://`, `ftp://` tms.
- Follow redirects max 3 kertaa
- Max response size: 512 KB (riittää OG-tageille headissä)
- Timeout: 5 sekuntia
- User-Agent: `ircord-preview/1.0` (rehellinen)
- **Ei** lähetä evästeitä eikä autentikointitietoja

---

## 11. LocalStore — SQLite Schema

```sql
-- Oma identiteetti
CREATE TABLE identity (
    id              INTEGER PRIMARY KEY CHECK (id = 1),
    user_id         TEXT NOT NULL,
    identity_pub    BLOB NOT NULL,   -- Ed25519 pub (32 bytes)
    identity_priv   BLOB NOT NULL,   -- salattu Argon2id+XChaCha20
    server_cert_pin BLOB             -- serverin TLS cert fingerprint
);

-- Signal Protocol session store
CREATE TABLE signal_sessions (
    address         TEXT NOT NULL,   -- "user_id.1" (user + device)
    record          BLOB NOT NULL,   -- session record (libsignal binary)
    updated_at      INTEGER NOT NULL,
    PRIMARY KEY (address)
);

-- Pre-keyt (omat, uploadattu serverille)
CREATE TABLE pre_keys (
    key_id      INTEGER PRIMARY KEY,
    record      BLOB NOT NULL,
    uploaded    INTEGER NOT NULL DEFAULT 0
);

-- Signed Pre-Key
CREATE TABLE signed_pre_keys (
    key_id      INTEGER PRIMARY KEY,
    record      BLOB NOT NULL
);

-- Kavereiden identity keyt + trust status
CREATE TABLE peer_identities (
    user_id         TEXT PRIMARY KEY,
    identity_pub    BLOB NOT NULL,
    trust_status    TEXT NOT NULL DEFAULT 'unverified',
    -- 'unverified' | 'verified' | 'blocked'
    verified_at     INTEGER,
    safety_number   TEXT            -- tallennettu Safety Number
);

-- Sender Keys (ryhmät)
CREATE TABLE sender_keys (
    group_id    TEXT NOT NULL,
    sender_id   TEXT NOT NULL,
    record      BLOB NOT NULL,
    PRIMARY KEY (group_id, sender_id)
);

-- Viestihistoria (plaintext — jo purettu)
CREATE TABLE messages (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    channel     TEXT NOT NULL,
    sender_id   TEXT NOT NULL,
    content     TEXT NOT NULL,
    timestamp   INTEGER NOT NULL,
    msg_type    TEXT NOT NULL DEFAULT 'chat'  -- 'chat' | 'system' | 'voice_event'
);
CREATE INDEX idx_msg_channel_ts ON messages(channel, timestamp);

-- Kanavat ja jäsenyydet
CREATE TABLE channels (
    channel_id  TEXT PRIMARY KEY,
    display_name TEXT,
    joined_at   INTEGER NOT NULL
);

-- UI-tila (persistoitu sessioiden välillä)
CREATE TABLE ui_state (
    key     TEXT PRIMARY KEY,
    value   TEXT NOT NULL
    -- active_channel, scroll_positions, ...
);
```

**Koko tietokantatiedosto salataan:** SQLite Encryption Extension (SEE)
tai simplistisempi vaihtoehto: käynnistyksen yhteydessä avainten
derivointi passphrasesta (Argon2id) ja SQLite WAL-mode + per-record
XChaCha20-Poly1305 content-kentille.

---

## 12. Config

```toml
# ~/.config/ircord/config.toml

[server]
host         = "ircord.example.duckdns.org"
port         = 6667

[identity]
user_id      = "Sepi"

[ui]
theme        = "tokyo-night"  # tai polku custom .toml:iin
timestamp_format = "%H:%M"
show_seconds = false
compact_mode = false          # tiivistää viestit (ei tyhjää spacea)
bell_on_mention = true

[notifications]
mention_sound = true
dm_sound      = true

[voice]
input_device  = ""    # tyhjä = default
output_device = ""
push_to_talk  = false
ptt_key       = "ctrl+space"
vad_threshold = -40   # dBFS, silence threshold
opus_bitrate  = 64000

[preview]
enabled       = true
max_width     = 60    # merkkiä
fetch_timeout = 5     # sekuntia
```

---

## 13. Dependencies (vcpkg.json)

```json
{
  "name": "ircord-client",
  "version-string": "0.1.0",
  "dependencies": [
    "boost-asio",
    "openssl",
    "protobuf",
    "sqlitecpp",
    "libsignal-protocol-c",
    "libsodium",
    "libdatachannel",
    "opus",
    "libcurl",
    "toml11",
    "spdlog",
    "catch2",
    { "name": "notcurses", "features": ["minimal"] }
  ]
}
```

> **Huom:** `libsignal-protocol-c` ei välttämättä ole vcpkg:ssa — saattaa
> vaatia manuaalisen CMake ExternalProject tai FetchContent -integraation.
> miniaudio on header-only, lisätään suoraan sourceen.

---

## 14. Toteutusjärjestys

```
Vaihe 1 — Skeleton + TUI (1 vko)
  ✦ CMake + vcpkg, notcurses init
  ✦ Layout: tab_plane, msg_plane, input_plane, status_plane
  ✦ InputHandler: ncinput → InputEvent → dispatch
  ✦ Hard-coded test-viestit näkyvät oikein
  ✦ SIGWINCH (terminaalin resize) toimii

Vaihe 2 — Verkko (plaintext) (1 vko)
  ✦ NetClient: TLS connect, frame read/write
  ✦ Auth (identity key + challenge)
  ✦ Viestien lähetys/vastaanotto serveriltä
  ✦ Kanaviin liittyminen, presence-päivitykset näkyvät UI:ssa

Vaihe 3 — Crypto (1-2 vko)
  ✦ CryptoEngine init, key generation, SQLite stores
  ✦ Pre-key upload serverille
  ✦ X3DH + Double Ratchet 1:1 viesteille
  ✦ Sender Keys ryhmäkanaville
  ✦ Safety Number UI (/trust)

Vaihe 4 — Voice (1-2 vko)
  ✦ miniaudio capture + playback testataan erikseen
  ✦ Opus encode/decode pipeline
  ✦ libdatachannel PeerConnection + ICE signaling serverin kautta
  ✦ JitterBuffer + PLC
  ✦ /voice, /call, /hangup, /mute UI

Vaihe 5 — Polish (1 vko)
  ✦ LinkPreviewer (libcurl async, OG parse, TUI box)
  ✦ Tab complete (nicks, channels, commands)
  ✦ Input historia (↑↓)
  ✦ Scroll (PageUp/PageDown), unread badges
  ✦ Reconnect + offline message delivery
  ✦ /set + config persistointi
  ✦ Terminaalin bell @mention-kohdalla
```
