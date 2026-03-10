# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**IrssiCord** is an end-to-end encrypted chat and voice application for friend groups. It combines irssi's minimal terminal aesthetics with modern features: E2E encryption (Signal Protocol), link preview, voice rooms, and private calls.

**Architecture**: Client-server relay model — the server never sees plaintext messages.

## Technology Stack

| Component | Choice | Rationale |
|-----------|--------|-----------|
| Language | C++20 | Performance, compatibility client/server |
| Async I/O | Boost.Asio | Scalable event loop, cross-platform |
| Serialization | Protobuf | Better tooling, schema evolution vs FlatBuffers |
| TUI (client) | FTXUI | Cross-platform terminal UI, component-based |
| E2E Crypto | libsignal-protocol-c + libsodium | Signal Protocol (X3DH + Double Ratchet) |
| Voice | libdatachannel + libopus | WebRTC with ICE/DTLS-SRTP, lightweight |
| Audio I/O | miniaudio | Header-only, cross-platform |
| Database | SQLite (SQLiteCpp wrapper) | Embedded, minimal overhead |
| Config | toml11 | Simple TOML parsing |
| Logging | spdlog | Fast, structured logging |
| Testing | Catch2 | Header-only test framework |
| Build | CMake + vcpkg | Cross-platform build, manifest-mode deps |

## Build Commands

```bash
# Configure with vcpkg toolchain
cmake -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake ..

# Build
cmake --build .

# Run tests
ctest

# Specific configuration (Debug/Release, x64/x86)
cmake --build . --config Release
```

## High-Level Architecture

### Server (target: Linux aarch64 / RPi)

```
Listener (TLS/TCP) → Session Manager → Services:
  ├── AuthSvc (challenge/identity)
  ├── ChannelMgr (rooms, fanout)
  ├── KeyStore (pre-keys, identity)
  ├── VoiceSignal Svc (ICE relay)
  └── OfflineQueue (TTL-bounded)
         ↓
     SQLite DB
```

**Thread model**: `io_context` + thread pool, strands for per-session and per-channel serialization (no mutexes in hot path).

### Client

```
Main Thread: FTXUI render loop + input polling
         │
         ├─→ IO Thread (Boost.Asio): TLS recv/send, reconnect
         ├─→ Preview Thread (libcurl): OG metadata fetch
         └─→ Audio Thread (miniaudio callback): Opus encode/decode, jitter buffer
```

**Critical rule**: Only Main Thread calls FTXUI functions. Other threads post UI updates via `AppState::post_ui()`.

## Wire Protocol

Length-prefixed framing over TCP/TLS:

```
┌──────────────┬────────────────────────┐
│  4 bytes     │  N bytes               │
│  (uint32 BE) │  Protobuf Envelope     │
└──────────────┴────────────────────────┘
```

Max message size: **64 KB** (enforced server-side).

## Key Abstractions

These interfaces enable technology swaps without major refactoring:

```cpp
class ITransport;        // TCP now, QUIC later
class IRenderer;         // notcurses now, possible GUI later
class IVoiceTransport;   // libdatachannel now, swappable
class ISerializer;       // Protobuf now, maybe FlatBuffers later
```

## Security Model

- Server sees: IP addresses, who messages whom, timestamps
- Server does NOT see: message content, file contents, voice audio (P2P mode), link preview content
- Auth: Ed25519 identity key challenge-response (not password-based)
- E2E: Signal Protocol (X3DH initial + Double Ratchet for ongoing)
- At-rest: Identity keys encrypted with Argon2id + XChaCha20-Poly1305

## Directory Structure (Planned)

```
ircord-server/
├── CMakeLists.txt
├── vcpkg.json
├── config/server.toml.example
├── src/
│   ├── main.cpp
│   ├── server.hpp/.cpp          # io_context bootstrap, signal handling
│   ├── config.hpp/.cpp          # TOML config loader
│   ├── net/
│   │   ├── listener.hpp/.cpp    # TLS TCP acceptor
│   │   ├── session.hpp/.cpp     # Per-connection state machine
│   │   └── tls_context.hpp/.cpp # SSL context factory
│   ├── auth/
│   ├── channel/
│   ├── keys/
│   ├── presence/
│   ├── offline/
│   ├── voice/
│   ├── db/
│   └── proto/                   # Shared with client
└── test/

ircord-client/
├── CMakeLists.txt
├── vcpkg.json
├── src/
│   ├── main.cpp
│   ├── app.hpp/.cpp             # Top-level init, event loop
│   ├── state/
│   │   ├── app_state.hpp/.cpp   # All app state, thread-safe
│   │   ├── channel_state.hpp
│   │   └── voice_state.hpp
│   ├── ui/                      # notcurses rendering
│   ├── net/
│   ├── crypto/                  # Signal Protocol
│   ├── voice/                   # libdatachannel + Opus
│   ├── preview/                 # Link preview
│   ├── db/
│   ├── input/
│   └── proto/
└── resources/
```

## Implementation Phases

1. **Skeleton** - CMake, vcpkg, config, logger, listener, TLS accept, frame read/write
2. **Auth + DB** - SQLite schema, Ed25519 challenge-response, registration
3. **Chat relay** - Channel manager, fanout, 1:1 messages, offline queue
4. **Key distribution** - Pre-key upload/fetch, OPK consumption
5. **Presence + Ping** - Presence broadcast, PING/PONG timeout
6. **Voice signaling** - ICE candidate relay, call invite/accept/reject
7. **Hardening** - Rate limiting, input validation, fuzz testing

## Language Notes

The architecture documentation files in the parent directory (`../ircord-*.md`) are in **Finnish**. Key terms:
- *kanava* = channel
- *viesti* = message
- *käyttäjä* = user
- *palvelin* = server
- *asiakasohjelma/client* = client
- *lähetä* = send
- *vastaanota* = receive
