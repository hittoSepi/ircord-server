# IrssiCord

End-to-end encrypted chat and voice application for friend groups. Combines irssi's minimal terminal aesthetics with modern features: E2E encryption (Signal Protocol), link preview, voice rooms, and private calls.

## Architecture

**Client-server relay model** — the server never sees plaintext messages. The server acts as a relay and handles connection management, but all message content is encrypted end-to-end between clients.

## Technology Stack

| Component | Choice | Rationale |
|-----------|--------|-----------|
| Language | C++20 | Performance, compatibility client/server |
| Async I/O | Boost.Asio | Scalable event loop, cross-platform |
| Serialization | Protobuf | Better tooling, schema evolution vs FlatBuffers |
| E2E Crypto | libsignal-protocol-c + libsodium | Signal Protocol (X3DH + Double Ratchet) |
| Database | SQLite (SQLiteCpp wrapper) | Embedded, minimal overhead |
| Config | toml11 | Simple TOML parsing |
| Logging | spdlog | Fast, structured logging |
| Testing | Catch2 | Header-only test framework |
| Build | CMake + vcpkg | Cross-platform build, manifest-mode deps |

## Build Instructions

### Prerequisites

- **C++20 compatible compiler** (GCC 11+, Clang 13+, MSVC 2022+)
- **CMake 3.20+**
- **vcpkg** package manager
- **Git**

### Windows (Visual Studio 2022)

1. **Install vcpkg**
   ```bash
   git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
   cd C:\vcpkg
   .\bootstrap-vcpkg.bat
   ```

2. **Configure build environment**
   ```bash
   # Navigate to project directory
   cd C:\Users\<username>\Desktop\ircord\IrssiCord

   # Create build directory
   mkdir build
   cd build

   # Configure with vcpkg toolchain
   cmake -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ..
   ```

3. **Build**
   ```bash
   cmake --build . --config Debug
   ```

4. **Run**
   ```bash
   .\Debug\ircord-server.exe
   ```

### Linux (Debian/Ubuntu)

1. **Install dependencies**
   ```bash
   sudo apt-get update
   sudo apt-get install -y cmake g++ git libssl-dev
   ```

2. **Install vcpkg**
   ```bash
   git clone https://github.com/Microsoft/vcpkg.git ~/vcpkg
   ~/vcpkg/bootstrap-vcpkg.sh
   ```

3. **Build**
   ```bash
   mkdir build && cd build
   cmake -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake ..
   cmake --build . -j$(nproc)
   ```

4. **Run**
   ```bash
   ./ircord-server
   ```

## Configuration

Create a `server.toml` configuration file (see `config/server.toml.example`):

```toml
[server]
host = "0.0.0.0"
port = 6697
log_level = "info"
max_connections = 100

[tls]
cert_file = "certs/server.crt"
key_file = "certs/server.key"

[ping]
interval_sec = 60
timeout_sec = 120
```

### Generate TLS Certificates

For development, create self-signed certificates:

```bash
# Create certs directory
mkdir -p certs

# Generate private key
openssl genrsa -out certs/server.key 2048

# Generate certificate
openssl req -new -x509 -key certs/server.key -out certs/server.crt -days 365
```

## Running the Server

```bash
# From build directory
./ircord-server

# Or with custom config
./ircord-server /path/to/server.toml
```

The server will:
1. Load configuration from `server.toml`
2. Initialize TLS context with certificates
3. Start TCP listener on specified host:port
4. Accept client connections

### Testing with Telnet/PuTTY

You can test the server accepts connections:

```bash
telnet localhost 6697
```

Or use PuTTY:
- Host: `localhost`
- Port: `6697`
- Connection type: `telnet`

## Project Structure

```
ircord-server/
├── CMakeLists.txt          # CMake build configuration
├── vcpkg.json              # vcpkg dependencies
├── config/
│   └── server.toml.example # Configuration template
├── src/
│   ├── main.cpp            # Entry point
│   ├── server.cpp/hpp      # Server orchestration
│   ├── config.cpp/hpp      # Configuration loader
│   ├── net/
│   │   ├── listener.cpp/hpp    # TLS/TCP acceptor
│   │   ├── session.cpp/hpp     # Per-connection state machine
│   │   └── tls_context.cpp/hpp # SSL context factory
│   └── proto/
│       └── ircord.proto     # Protobuf schema
└── docs/
    ├── server-architecture.md      # Server design (Finnish)
    ├── client-architecture.md      # Client design (Finnish)
    └── ircord-tech-tradeoffs.md    # Technical decisions (Finnish)
```

## Wire Protocol

Length-prefixed framing over TCP/TLS:

```
┌──────────────┬────────────────────────┐
│  4 bytes     │  N bytes               │
│  (uint32 BE) │  Protobuf Envelope     │
└──────────────┴────────────────────────┘
```

Max message size: **64 KB** (enforced server-side).

## Security Model

- **Server sees**: IP addresses, who messages whom, timestamps
- **Server does NOT see**: message content, file contents, voice audio (P2P mode), link preview content
- **Auth**: Ed25519 identity key challenge-response (not password-based)
- **E2E**: Signal Protocol (X3DH initial + Double Ratchet for ongoing)
- **At-rest**: Identity keys encrypted with Argon2id + XChaCha20-Poly1305

## Current Implementation Status

### ✅ Implemented
- [x] TLS/TCP listener with Boost.Asio
- [x] Session management with strand serialization
- [x] Protobuf wire protocol framing
- [x] TOML configuration loading
- [x] Thread pool architecture
- [x] Basic server infrastructure
- [x] Visual Studio 2022 build support

### 🚧 In Progress
- [ ] Message relay between clients
- [ ] Authentication system (Ed25519)
- [ ] Channel management
- [ ] Key distribution (pre-keys)
- [ ] Presence system

### 📋 Planned
- [ ] Voice signaling (WebRTC ICE relay)
- [ ] Offline message queue
- [ ] Rate limiting
- [ ] File transfer relay
- [ ] Link preview service

## Documentation

**Note:** Most architecture documentation is in Finnish. See the `docs/` directory:
- `server-architecture.md` - Server architecture (Finnish)
- `client-architecture.md` - Client architecture (Finnish)
- `ircord-tech-tradeoffs.md` - Technical decisions (Finnish)

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Contributing

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add some amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## Acknowledgments

- **irssi** - Inspiration for the terminal aesthetic
- **Signal** - End-to-end encryption protocol
- **Boost.Asio** - Excellent async I/O library
- **vcpkg** - C++ package manager
