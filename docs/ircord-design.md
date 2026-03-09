# IRCord — Design Document

> End-to-end encrypted chat & voice for friend groups.
> irssi meets Discord, minus the bloat.

---

## 1. Overview

**IRCord** on TUI-pohjainen (ncurses) chat- ja VoIP-sovellus, joka yhdistää
irssin minimalistisen terminaali-estetiikan moderneihin ominaisuuksiin:
E2E-salaus (Signal Protocol), link preview, voice rooms ja private calls.

Arkkitehtuuri: client–server, jossa serveri toimii relay-nodena eikä koskaan
näe plaintext-viestejä.

---

## 2. High-Level Architecture

```
┌─────────────┐     TLS 1.3 / QUIC      ┌──────────────────┐
│  Client A   │◄────────────────────────►│                  │
│  (ncurses)  │                          │   IRCord Server  │
└─────────────┘                          │                  │
                                         │  - Auth/Identity │
┌─────────────┐     TLS 1.3 / QUIC      │  - Message relay │
│  Client B   │◄────────────────────────►│  - Key distrib.  │
│  (ncurses)  │                          │  - Presence      │
└─────────────┘                          │  - Voice signal  │
                                         └──────┬───────────┘
┌─────────────┐                                 │
│  Client C   │◄────────────────────────────────┘
└─────────────┘

         Voice (P2P kun mahdollista):
         Client A ◄──── DTLS-SRTP ────► Client B
                   (Opus @ 48kHz)
```

### Komponentit

| Komponentti | Rooli |
|---|---|
| **ircord-server** | Relay, key distribution, presence, STUN/TURN, offline store |
| **ircord-client** | ncurses TUI, Signal Protocol session mgmt, voice engine |
| **ircord-proto** | Shared protobuf/flatbuffers message definitions |

---

## 3. Protocol Stack

```
┌─────────────────────────────────────────────┐
│            Application Layer                │
│   Chat messages, commands, link previews    │
├─────────────────────────────────────────────┤
│         Signal Protocol (E2E)               │
│   Double Ratchet, X3DH key agreement       │
├─────────────────────────────────────────────┤
│         Framing / Serialization             │
│   Protobuf or FlatBuffers                   │
├─────────────────────────────────────────────┤
│         Transport Security                  │
│   TLS 1.3 (server link) / DTLS (voice)     │
├─────────────────────────────────────────────┤
│         Transport                           │
│   TCP/QUIC (chat) / UDP (voice RTP)         │
└─────────────────────────────────────────────┘
```

### Miksi QUIC harkinnan arvoinen

- Multiplexoidut streamit ilman head-of-line blockingia
- Sisäänrakennettu TLS 1.3
- 0-RTT reconnect — hyvä mobiili/laptop lid-close tilanteisiin
- Valmis kirjasto: **quiche** (Cloudflare, C/C++) tai **msquic** (Microsoft)

Vaihtoehto: perus TCP + TLS 1.3 on yksinkertaisempi aloittaa.

---

## 4. E2E Encryption — Signal Protocol

### Kirjasto: libsignal-protocol-c

[signalapp/libsignal-protocol-c](https://github.com/signalapp/libsignal-protocol-c) —
puhdas C, linkkaa suoraan C++ projektiin. Vaatii RAII-wrapperit
`SIGNAL_REF`/`SIGNAL_UNREF` makrojen ympärille.

Vaihtoehtoinen moderni lähestymistapa: Signalin uudempi
[Rust-pohjainen libsignal](https://github.com/signalapp/libsignal) +
C FFI, mutta monimutkaisempi integraatio.

### Key Management

```
Registration:
  Client generates:
    - Identity Key Pair (Curve25519, long-term)
    - Signed Pre-Key (rotated periodically)
    - N × One-Time Pre-Keys (single use)

  Client uploads public keys → Server key store

Session Setup (X3DH):
  Alice wants to message Bob:
    1. Alice fetches Bob's pre-key bundle from server
    2. X3DH key agreement → shared secret
    3. Double Ratchet initialized
    4. First message includes Alice's ephemeral key

  Bob receives:
    1. Completes X3DH from his side
    2. Double Ratchet initialized
    3. Messages flow with forward secrecy
```

### Ryhmäviestit

Kaksi vaihtoehtoa:

**A) Sender Keys (Signal Group v2 tyyli)**
- Lähettäjä luo Sender Key → jakaa kaikille ryhmän jäsenille 1:1-sessioiden kautta
- Viesti salataan kerran Sender Keylla → serveri fanout
- Pro: O(1) encryption per message
- Con: ei per-message forward secrecy, member add/remove vaatii rekeying

**B) Pairwise sessions**
- Jokainen viesti salataan erikseen jokaiselle vastaanottajalle
- Pro: täysi forward secrecy
- Con: O(N) encryption per message

**Suositus:** Sender Keys kaveriporukan kokoiselle ryhmälle (5-20 hlö).
Rekeying kun joku poistuu ryhmästä.

### Client-Side Storage

```
~/.config/ircord/
├── identity.key          # Identity key pair (encrypted at rest)
├── sessions.db           # SQLite: Signal sessions, ratchet state
├── prekeys.db            # Pre-key store
├── messages.db           # Message history (encrypted)
└── config.toml           # Server addr, nick, UI prefs
```

Avaimet at rest: salattu käyttäjän passphrasella (Argon2id → AES-256-GCM).

---

## 5. Server Design

### Vastuut

Serveri **ei koskaan** näe plaintext-viestejä. Se käsittelee:

1. **Autentikointi** — SRP tai challenge-response identity keyllä
2. **Key Distribution** — Tallentaa ja jakaa pre-key bundlet
3. **Message Relay** — Välittää E2E-salatut blobbit
4. **Offline Store** — Puskuroi viestit offline-käyttäjille (TTL-rajattu)
5. **Presence** — Online/away/offline status
6. **Kanavahallinta** — Kanavien luonti, kutsut, oikeudet
7. **Voice Signaling** — ICE candidate exchange, STUN/TURN relay

### Teknologiat

| Osa | Valinta | Perustelu |
|---|---|---|
| Kieli | C++ (17/20) | Yhteensopiva clientin kanssa, suorituskyky |
| Async I/O | io_uring (Linux) / libuv | Skaalautuva event loop |
| Serialization | Protobuf tai FlatBuffers | Zero-copy mahdollinen FlatBuffersilla |
| Tietokanta | SQLite (pieni) / PostgreSQL (iso) | Offline viestit, key store |
| STUN/TURN | libnice tai coturn | NAT traversal voicelle |

### Wire Protocol (pelkistetty)

```protobuf
// Envelope — serverin näkemä taso
message Envelope {
  uint64 timestamp         = 1;
  string sender_id         = 2;
  string recipient_id      = 3;  // user or channel
  uint32 message_type      = 4;  // CHAT, KEY_EXCHANGE, VOICE_SIGNAL, PRESENCE, ...
  bytes  encrypted_payload = 5;  // Signal Protocol ciphertext
  bytes  sender_identity   = 6;  // public identity key (for key exchange msgs)
}

// Decrypted payload (client-side only)
message ChatMessage {
  string text              = 1;
  repeated Attachment attachments = 2;
  LinkPreview link_preview = 3;
  uint64 reply_to          = 4;  // message ID for threads
}

message LinkPreview {
  string url               = 1;
  string title             = 2;
  string description       = 3;
  bytes  thumbnail         = 4;  // optional, small
}
```

---

## 6. Voice Architecture

### Yleinen voice room (per kanava)

Jokainen kanava voi omistaa yhden "voice loobin":

```
                    ┌──────────────┐
        ┌──────────►│  SFU / Mesh  │◄──────────┐
        │           │  (server)    │            │
        │           └──────┬───────┘            │
        │                  │                    │
   ┌────┴─────┐     ┌─────┴──────┐      ┌─────┴──────┐
   │ Client A │     │  Client B  │      │  Client C  │
   │  (Opus)  │     │  (Opus)    │      │  (Opus)    │
   └──────────┘     └────────────┘      └────────────┘
```

**Pienelle ryhmälle (≤6):** Full mesh P2P
- Jokainen lähettää jokaiselle suoraan
- Ei serveri-bottleneckia, minimaalinen latenssi

**Isommalle ryhmälle (>6):** SFU (Selective Forwarding Unit)
- Serveri vastaanottaa jokaisen streamin ja forwardoi muille
- Ei transcoding-kuormaa, mutta säästää uplink-kaistaa

### Private Calls

Suora P2P-yhteys kahden clientin välillä:
1. Caller lähettää CALL_INVITE serverin kautta
2. Server relayaa callee:lle
3. ICE candidate exchange serverin kautta
4. DTLS-SRTP P2P-yhteys muodostuu
5. Opus audio flow

### Audio Stack

```
Mikrofoni → PortAudio capture → Opus encode (48kHz/64kbps)
→ SRTP encrypt → UDP send

UDP recv → SRTP decrypt → Opus decode → Jitter buffer
→ PortAudio playback → Kaiutin
```

| Komponentti | Kirjasto |
|---|---|
| Audio I/O | PortAudio tai miniaudio |
| Codec | libopus |
| Transport | libdatachannel (kevyt WebRTC) tai oma UDP + libsrtp |
| NAT traversal | libjuice (kevyt ICE) tai libnice |

[libdatachannel](https://github.com/paullouisageneau/libdatachannel) on
erinomainen kevyt vaihtoehto täydelle libWebRTC:lle — tukee DTLS-SRTP:tä
ja ICE:tä ilman Googlen massiivista buildisysteemiä.

### Voice E2E Encryption

DTLS-SRTP antaa hop-by-hop salauksen. Jos halutaan aito E2E myös
SFU-moodissa (serveri ei voi kuunnella):

- **Double Ratchet frame encryption**: jokainen Opus frame salataan
  erikseen lähettäjän ja vastaanottajan välisellä avaimella
- Toteutus: SFrame (Secure Frame) tai custom symmetric ratchet
- Trade-off: lisää latenssia ~1ms, mutta aito E2E

---

## 7. TUI Design (ncurses)

```
┌─ #general ── #random ── #dev ── 🔊general-voice ───────────────────┐
│                                                                     │
│ [21:33] <Matti>  Kattokaas tää: https://example.com/cool           │
│         ┌──────────────────────────────┐                           │
│         │ 🔗 Example Site              │                           │
│         │ Cool article about things    │                           │
│         └──────────────────────────────┘                           │
│ [21:34] <Teppo>  nice                                              │
│ [21:35] <Sepi>   jep, E2E FTW 🔒                                  │
│                                                                     │
│─────────────────────────────────────────────────────────────────────│
│ 🔊 Voice: Matti, Teppo (2)  │ 👤 Online: Matti Teppo Sepi Pekka  │
│─────────────────────────────────────────────────────────────────────│
│ > _                                                                 │
└─────────────────────────────────────────────────────────────────────┘
```

### UI-elementit

- **Tab bar** ylhäällä: kanavat, aktiivinen korostettu, unread-indikaattori
- **Message area**: scrollable, aikaleima + nick + viesti
- **Link preview**: inline box viestien seassa (fetched client-side)
- **Status bar**: voice room participants + online users
- **Input line**: irssi-tyylinen, `/`-komennot

### Komennot (irssi-henkiset)

```
/join #kanava          Liity kanavalle
/part                  Poistu kanavalta
/msg <nick> <viesti>   Yksityisviesti (E2E 1:1 session)
/voice                 Liity kanavan voice roomiin
/call <nick>           Aloita private call
/hangup                Lopeta puhelu
/mute                  Mykistä mikki
/deafen                Mykistä kaikki äänet
/nick <uusi>           Vaihda nick
/invite <nick>         Kutsu kaverille serveri-invite
/trust <nick>          Verifioi kaverin identity key (Safety Number)
/keys                  Näytä omat identity key fingerprints
/whois <nick>          Näytä käyttäjätiedot + key fingerprint
/set <key> <value>     Konfiguraatio
/quit                  Poistu
```

### Link Preview

Client-side toteutus:
1. Regex tunnistaa URLit viestistä
2. Client fetchaa OG-metatiedot (title, description, image)
3. Renderöi inline-boxina viestin alle
4. **Tärkeä**: fetch tapahtuu vain vastaanottajan clientissä, ei serverin kautta
   (muuten serveri näkisi mitä linkkejä jaetaan)

---

## 8. Dependency Map

### Client

| Dependency | Versio | Käyttö |
|---|---|---|
| ncurses / notcurses | latest | TUI rendering |
| libsignal-protocol-c | 2.3.x | E2E encryption |
| libsodium | 1.0.x | Crypto primitives (Argon2, XChaCha20) |
| protobuf / flatbuffers | 3.x / 24.x | Message serialization |
| libcurl | 8.x | Link preview fetching |
| SQLite | 3.x | Local storage |
| libopus | 1.5.x | Voice codec |
| PortAudio / miniaudio | latest | Audio I/O |
| libdatachannel | 0.21.x | WebRTC (voice transport) |
| toml++ | 3.x | Config parsing |

### Server

| Dependency | Versio | Käyttö |
|---|---|---|
| libuv / io_uring | latest | Async I/O |
| protobuf / flatbuffers | 3.x / 24.x | Serialization |
| SQLite / PostgreSQL | | Persistence |
| coturn (external) | | TURN relay |
| OpenSSL / BoringSSL | 3.x | TLS 1.3 |

### Build System

**CMake** + **vcpkg** tai **Conan** dependency managementiin.
Suositus: vcpkg manifest mode (`vcpkg.json` repossa).

---

## 9. Security Model

### Threat Model

| Uhka | Suojaus |
|---|---|
| Serveri kompromissoitu | E2E encryption — serveri ei näe plaintextia |
| Verkkokuuntelu | TLS 1.3 + Signal Protocol |
| Key impersonation | Safety Numbers (identity key verification) |
| Replay attacks | Signal Protocol ratchet + message counters |
| Metadata leakage | Serveri näkee: kuka puhuu kenelle, milloin. Tätä ei voi välttää relay-mallissa. |
| Local key theft | At-rest encryption (Argon2id + AES-256-GCM) |
| Forward secrecy break | Double Ratchet: jokainen viesti eri avaimella |

### Mitä serveri näkee (metadata)

- Käyttäjien IP-osoitteet
- Kuka lähettää kenelle/mille kanavalle
- Viestien aikaleimat ja koot
- Presence-tiedot

### Mitä serveri EI näe

- Viestien sisältö
- Tiedostojen sisältö
- Voice-audio (P2P-moodissa)
- Link preview -sisältö

---

## 10. Development Roadmap

### Phase 1 — Foundation (4-6 viikkoa)
- [ ] Projektistruktuuri, CMake, vcpkg
- [ ] Protobuf message definitions
- [ ] Server: TCP listener, auth, basic relay
- [ ] Client: ncurses scaffold, connection, basic chat
- [ ] Plaintext-viestit toimii end-to-end

### Phase 2 — E2E Encryption (3-4 viikkoa)
- [ ] libsignal-protocol-c integraatio + C++ wrapperit
- [ ] Key generation, registration, pre-key upload
- [ ] X3DH session setup
- [ ] Double Ratchet messaging
- [ ] Sender Keys ryhmille
- [ ] At-rest encryption local storagelle

### Phase 3 — Voice (4-6 viikkoa)
- [ ] PortAudio/miniaudio capture & playback
- [ ] Opus encode/decode pipeline
- [ ] ICE/STUN signaling serverin kautta
- [ ] P2P mesh voice room (≤6 users)
- [ ] DTLS-SRTP encryption
- [ ] Voice UI: /voice, /call, /hangup, /mute
- [ ] Push-to-talk & voice activity detection

### Phase 4 — Polish (3-4 viikkoa)
- [ ] Link preview (OG metadata fetch + TUI render)
- [ ] Offline message delivery
- [ ] File transfer (E2E encrypted)
- [ ] Message history search
- [ ] /trust & Safety Number verification UX
- [ ] Notification system (terminal bell / desktop notify)
- [ ] Config system (/set, config.toml)

### Phase 5 — Hardening
- [ ] SFU mode isommille voice groupeille
- [ ] QUIC transport option
- [ ] Fuzz testing (protobuf parsing, crypto)
- [ ] Security audit
- [ ] Packaging (AUR, Homebrew, .deb)

---

## 11. Open Questions

1. **Protobuf vs FlatBuffers?** — Protobuf on tutumpi ja paremmin tuettu,
   FlatBuffers on zero-copy mutta vähemmän tooling-tukea. Alkuun protobuf?

2. **notcurses vs ncurses?** — notcurses tukee true coloria, imageja
   terminaalissa (sixel/kitty) ja on modernimpi API. Tradeoff: vähemmän
   portaabeli.

3. **Voice: libdatachannel vs oma UDP + libsrtp?** — libdatachannel antaa
   valmiin ICE + DTLS stackin. Oma on kevyempi mutta enemmän työtä.

4. **Auth: SRP vs asymmetric challenge?** — SRP on zero-knowledge password
   proof mutta monimutkainen. Identity key -pohjainen challenge on
   yksinkertaisempi jos ei tarvita password-pohjaista autentikaatiota.

5. **Transport: TCP+TLS vs QUIC?** — QUIC on tulevaisuus mutta lisää
   kompleksisuutta. Aloitetaanko TCP:llä ja lisätään QUIC myöhemmin?
