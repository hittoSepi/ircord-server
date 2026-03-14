#!/bin/bash
# =============================================================================
# IRCord Server — Install Script
# Ubuntu 22.04+ / Debian 12+, x86_64 or aarch64
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/hittoSepi/ircord/main/ircord-server/scripts/install.sh | sudo bash
#
# Or download and run:
#   wget https://raw.githubusercontent.com/hittoSepi/ircord/main/ircord-server/scripts/install.sh
#   chmod +x install.sh
#   sudo ./install.sh
#
# Environment variables (optional):
#   IRCORD_DOMAIN        - Domain name (default: asks interactively)
#   IRCORD_PORT          - Server port (default: 6697)
#   IRCORD_USE_LETSENCRYPT - "yes" or "no" (default: asks interactively)
#   IRCORD_LE_METHOD     - "standalone", "dns-cloudflare", or "skip" (default: asks)
#   IRCORD_CF_TOKEN      - Cloudflare API token (for DNS method)
# =============================================================================
set -euo pipefail

# --- Colors and helper functions --------------------------------------------
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()    { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()      { echo -e "${GREEN}[ OK ]${NC}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[FAIL]${NC}  $*"; exit 1; }
step()    { echo -e "\n${CYAN}══════════════════════════════════════════${NC}"; \
            echo -e "${CYAN}  $*${NC}"; \
            echo -e "${CYAN}══════════════════════════════════════════${NC}"; }

# --- Root check --------------------------------------------------------------
[ "$(id -u)" -eq 0 ] || error "Run with sudo: sudo $0"

# --- Variables ----------------------------------------------------------------
IRCORD_PORT="${IRCORD_PORT:-6697}"
INSTALL_DIR="/opt/ircord"
DATA_DIR="/var/lib/ircord"
LOG_DIR="/var/log/ircord"
REPO_URL="https://github.com/hittoSepi/ircord-server.git"
BUILD_DIR="/tmp/ircord-build"
SERVICE_USER="ircord"

# --- Interactive input -------------------------------------------------------
step "IRCord Server — Installation"
echo ""

# Ask for domain
if [ -z "${IRCORD_DOMAIN:-}" ]; then
    read -rp "  Domain (e.g., chat.example.com): " IRCORD_DOMAIN
echo ""
fi
[ -n "$IRCORD_DOMAIN" ] || error "Domain cannot be empty"

# Validate domain format
if [[ ! "$IRCORD_DOMAIN" =~ ^[a-zA-Z0-9][a-zA-Z0-9.-]*\.[a-zA-Z]{2,}$ ]]; then
    warn "Domain format looks unusual: $IRCORD_DOMAIN"
    read -rp "  Continue anyway? (y/N) " -n 1 -r
    echo ""
    [[ $REPLY =~ ^[Yy]$ ]] || exit 1
fi

# Ask for Let's Encrypt
if [ -z "${IRCORD_USE_LETSENCRYPT:-}" ]; then
    echo "  SSL Certificate options:"
    echo "    1) Let's Encrypt (recommended for public servers)"
    echo "    2) Self-signed certificate (for testing/private use)"
    echo "    3) I'll provide my own certificates"
    read -rp "  Select option [1-3]: " LE_OPTION
    echo ""
    
    case "$LE_OPTION" in
        1) IRCORD_USE_LETSENCRYPT="yes" ;;
        2) IRCORD_USE_LETSENCRYPT="selfsigned" ;;
        3) IRCORD_USE_LETSENCRYPT="skip" ;;
        *) IRCORD_USE_LETSENCRYPT="yes" ;;
    esac
fi

# Ask for Let's Encrypt method if using LE
if [ "$IRCORD_USE_LETSENCRYPT" = "yes" ] && [ -z "${IRCORD_LE_METHOD:-}" ]; then
    echo "  Let's Encrypt validation method:"
    echo "    1) Standalone (requires port 80 free during install)"
    echo "    2) DNS (Cloudflare) - supports wildcard domains"
    read -rp "  Select method [1-2]: " LE_METHOD_OPTION
    echo ""
    
    case "$LE_METHOD_OPTION" in
        2) IRCORD_LE_METHOD="dns-cloudflare" ;;
        *) IRCORD_LE_METHOD="standalone" ;;
    esac
fi

# Ask for Cloudflare token if using DNS method
if [ "$IRCORD_USE_LETSENCRYPT" = "yes" ] && [ "${IRCORD_LE_METHOD:-}" = "dns-cloudflare" ]; then
    if [ -z "${IRCORD_CF_TOKEN:-}" ]; then
        echo "  Cloudflare API Token required for DNS validation."
        echo "  Create one at: https://dash.cloudflare.com/profile/api-tokens"
        echo "  Required permissions: Zone:Read, DNS:Edit"
        read -rsp "  Cloudflare API Token: " IRCORD_CF_TOKEN
        echo ""
    fi
    [ -n "$IRCORD_CF_TOKEN" ] || error "Cloudflare API Token is required for DNS validation"
fi

# Public server listing
if [ -z "${IRCORD_PUBLIC:-}" ]; then
    read -rp "  List this server publicly? (y/N) " -n 1 -r
    echo ""
    [[ $REPLY =~ ^[Yy]$ ]] && IRCORD_PUBLIC="yes" || IRCORD_PUBLIC="no"
fi

# Directory URL (only relevant if public)
if [ "$IRCORD_PUBLIC" = "yes" ] && [ -z "${IRCORD_DIRECTORY_URL:-}" ]; then
    read -rp "  Directory server URL [https://directory.ircord.dev]: " IRCORD_DIRECTORY_URL
    echo ""
fi
IRCORD_DIRECTORY_URL="${IRCORD_DIRECTORY_URL:-https://directory.ircord.dev}"

# Display summary
echo ""
info "Configuration summary:"
echo "  Domain:     $IRCORD_DOMAIN"
echo "  Port:       $IRCORD_PORT"
echo "  SSL:        $IRCORD_USE_LETSENCRYPT"
[ "$IRCORD_USE_LETSENCRYPT" = "yes" ] && echo "  LE Method:  ${IRCORD_LE_METHOD:-standalone}"
echo "  Public:     $IRCORD_PUBLIC"
[ "$IRCORD_PUBLIC" = "yes" ] && echo "  Directory:  $IRCORD_DIRECTORY_URL"
echo "  Install:    $INSTALL_DIR"
echo ""
read -rp "  Proceed with installation? (Y/n) " -n 1 -r
echo ""
[[ $REPLY =~ ^[Nn]$ ]] && exit 0

# =============================================================================
step "1/8  Dependencies"
# =============================================================================
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential cmake git curl zip unzip tar pkg-config \
    ninja-build screen openssl \
    libssl-dev libsodium-dev libsqlite3-dev \
    protobuf-compiler libprotobuf-dev \
    libboost-all-dev libspdlog-dev \
    certbot python3-certbot-dns-cloudflare \
    ufw jq
ok "Dependencies installed"

# =============================================================================
step "2/8  vcpkg"
# =============================================================================
VCPKG_DIR="/opt/vcpkg"
if [ ! -f "$VCPKG_DIR/vcpkg" ]; then
    info "Cloning vcpkg → $VCPKG_DIR"
    git clone --depth 1 https://github.com/microsoft/vcpkg.git "$VCPKG_DIR" 2>&1 | tail -3
    "$VCPKG_DIR/bootstrap-vcpkg.sh" -disableMetrics 2>&1 | tail -3
    ok "vcpkg installed"
else
    ok "vcpkg already exists ($VCPKG_DIR)"
fi

# =============================================================================
step "3/8  Source code + build"
# =============================================================================
rm -rf "$BUILD_DIR"
info "Cloning $REPO_URL → $BUILD_DIR"
git clone --depth 1 "$REPO_URL" "$BUILD_DIR/ircord-server" 2>&1 | tail -3

# Use ircord-server subdirectory
SERVER_DIR="$BUILD_DIR/ircord-server"
[ -d "$SERVER_DIR" ] || error "Server directory not found: $SERVER_DIR"

info "CMake configure (vcpkg will download packages, this may take 15-30 min)..."
cmake \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -S "$SERVER_DIR" \
    -B "$SERVER_DIR/build" \
    2>&1 | grep -E '^(--|CMake|error:|warning:|\[)' | tail -40

info "CMake build..."
cmake --build "$SERVER_DIR/build" -j"$(nproc)" 2>&1 | grep -E '(%\]|error:|warning:)' | tail -20

BINARY="$SERVER_DIR/build/ircord-server"
[ -f "$BINARY" ] || error "Binary not found: $BINARY"
ok "Binary built: $(du -sh "$BINARY" | cut -f1)"

# =============================================================================
step "4/8  SSL Certificate"
# =============================================================================
CERT_PATH="/etc/letsencrypt/live/$IRCORD_DOMAIN/fullchain.pem"
KEY_PATH="/etc/letsencrypt/live/$IRCORD_DOMAIN/privkey.pem"

if [ "$IRCORD_USE_LETSENCRYPT" = "skip" ]; then
    ok "Skipping certificate - you'll provide your own"
    CERT_PATH="/etc/ircord/cert.pem"
    KEY_PATH="/etc/ircord/key.pem"
    mkdir -p /etc/ircord
    
elif [ "$IRCORD_USE_LETSENCRYPT" = "selfsigned" ]; then
    info "Generating self-signed certificate..."
    mkdir -p /etc/ircord
    CERT_PATH="/etc/ircord/cert.pem"
    KEY_PATH="/etc/ircord/key.pem"
    
    openssl req -x509 -newkey rsa:4096 \
        -keyout "$KEY_PATH" \
        -out "$CERT_PATH" \
        -days 365 \
        -nodes \
        -subj "/CN=$IRCORD_DOMAIN" \
        2>/dev/null
    
    chmod 644 "$CERT_PATH"
    chmod 600 "$KEY_PATH"
    ok "Self-signed certificate generated"
    
elif [ "$IRCORD_USE_LETSENCRYPT" = "yes" ]; then
    if [ -f "$CERT_PATH" ]; then
        ok "Certificate already exists: $CERT_PATH"
    elif [ "${IRCORD_LE_METHOD:-standalone}" = "dns-cloudflare" ]; then
        info "Obtaining Let's Encrypt certificate via Cloudflare DNS..."
        CF_CREDS="/root/.cloudflare-ircord.ini"
        cat > "$CF_CREDS" <<EOF
dns_cloudflare_api_token = ${IRCORD_CF_TOKEN}
EOF
        chmod 600 "$CF_CREDS"

        certbot certonly \
            --dns-cloudflare \
            --dns-cloudflare-credentials "$CF_CREDS" \
            -d "$IRCORD_DOMAIN" \
            --non-interactive \
            --agree-tos \
            --email "admin@${IRCORD_DOMAIN#*.}" \
            2>&1 | tail -10

        [ -f "$CERT_PATH" ] || error "Certificate acquisition failed"
        ok "Certificate obtained: $CERT_PATH"
        
    else
        # Standalone method
        info "Obtaining Let's Encrypt certificate (standalone mode)..."
        info "Make sure port 80 is free and domain points to this server"
        
        # Check if port 80 is available
        if ss -tlnp | grep -q ":80 "; then
            warn "Port 80 appears to be in use. Let's Encrypt standalone may fail."
            read -rp "  Continue anyway? (y/N) " -n 1 -r
            echo ""
            [[ $REPLY =~ ^[Yy]$ ]] || exit 1
        fi
        
        certbot certonly \
            --standalone \
            -d "$IRCORD_DOMAIN" \
            --non-interactive \
            --agree-tos \
            --email "admin@${IRCORD_DOMAIN#*.}" \
            2>&1 | tail -10
        
        [ -f "$CERT_PATH" ] || error "Certificate acquisition failed"
        ok "Certificate obtained: $CERT_PATH"
    fi
else
    error "Unknown SSL option: $IRCORD_USE_LETSENCRYPT"
fi

# =============================================================================
step "5/8  User, directories, files"
# =============================================================================
# User
id "$SERVICE_USER" &>/dev/null || useradd -r -s /bin/false -M "$SERVICE_USER"
ok "User: $SERVICE_USER"

# Directories
mkdir -p "$INSTALL_DIR" "$DATA_DIR" "$LOG_DIR"
chown root:"$SERVICE_USER" "$INSTALL_DIR"
chown "$SERVICE_USER":"$SERVICE_USER" "$DATA_DIR" "$LOG_DIR"
chmod 750 "$INSTALL_DIR" "$DATA_DIR" "$LOG_DIR"

# Binary
install -o root -g "$SERVICE_USER" -m 750 "$BINARY" "$INSTALL_DIR/ircord-server"
ok "Binary installed: $INSTALL_DIR/ircord-server"

# Certificate permissions (for Let's Encrypt)
if [ "$IRCORD_USE_LETSENCRYPT" = "yes" ] && [ -d "/etc/letsencrypt/live/$IRCORD_DOMAIN" ]; then
    chmod 750 /etc/letsencrypt/live /etc/letsencrypt/archive 2>/dev/null || true
    chgrp "$SERVICE_USER" /etc/letsencrypt/live /etc/letsencrypt/archive 2>/dev/null || true
    chgrp -R "$SERVICE_USER" "/etc/letsencrypt/live/$IRCORD_DOMAIN" \
                          "/etc/letsencrypt/archive/$IRCORD_DOMAIN" 2>/dev/null || true
    chmod -R g+r "/etc/letsencrypt/archive/$IRCORD_DOMAIN" 2>/dev/null || true
fi

# Generate server.toml

FILE_ENCRYPTION_KEY=$(openssl rand -hex 32)

cat > "$INSTALL_DIR/server.toml" <<EOF
# IRCord Server Configuration
# Generated on $(date -Iseconds)

[server]
host = "0.0.0.0"
port = ${IRCORD_PORT}
log_level = "info"
max_connections = 100
public = $( [ "$IRCORD_PUBLIC" = "yes" ] && echo "true" || echo "false" )

[tls]
cert_file = "${CERT_PATH}"
key_file  = "${KEY_PATH}"

[database]
path = "${DATA_DIR}/ircord.db"

[limits]
# Maximum message size in bytes (64 KB default)
max_message_bytes = 65536

# Ping interval in seconds (server sends PING)
ping_interval_sec = 30

# Ping timeout in seconds (disconnect if no PONG)
ping_timeout_sec = 60

# Maximum messages per second per authenticated user
msg_rate_per_sec = 20

# Maximum connection attempts per minute per IP address
conn_rate_per_min = 10

# Command rate limiting
# Maximum commands per minute per user (includes all /commands)
commands_per_min = 30

# Maximum channel joins per minute per user
joins_per_min = 5

# Abuse detection
# Number of rate limit violations before auto-ban
abuse_threshold = 5

# Time window for counting violations (minutes)
abuse_window_min = 10

# Ban duration for repeat offenders (minutes)
ban_duration_min = 30


[security]
# Master key for file encryption at rest (64 hex characters = 32 bytes)
# Generate a key with: openssl rand -hex 32
# Leave empty to disable file encryption (files stored unencrypted - NOT RECOMMENDED)
file_encryption_key = "${FILE_ENCRYPTION_KEY}"

[antivirus]
# ClamAV daemon (clamd) configuration for virus scanning
# Install ClamAV: apt-get install clamav-daemon (Debian/Ubuntu)
# 
# Option 1: Unix socket (Linux/macOS only, faster)
# clamav_socket = "/var/run/clamav/clamd.ctl"
#
# Option 2: TCP socket (cross-platform)
# clamav_host = "127.0.0.1"
# clamav_port = 3310
#
# Leave empty/disabled to skip virus scanning
clamav_socket = ""
clamav_host = "127.0.0.1"
clamav_port = 0

[directory]
enabled = $( [ "$IRCORD_PUBLIC" = "yes" ] && echo "true" || echo "false" )
url = "${IRCORD_DIRECTORY_URL}"
ping_interval_sec = 300
server_name = "${IRCORD_DOMAIN}"
description = "An IRCord encrypted chat server"
EOF
ok "Config written: $INSTALL_DIR/server.toml"

# =============================================================================
step "6/8  Systemd service + firewall + renewal"
# =============================================================================
cat > /etc/systemd/system/ircord-server.service <<EOF
[Unit]
Description=IRCord Chat Server
After=network.target
StartLimitIntervalSec=0

[Service]
Type=simple
User=${SERVICE_USER}
Group=${SERVICE_USER}
WorkingDirectory=${INSTALL_DIR}
ExecStart=${INSTALL_DIR}/ircord-server --config ${INSTALL_DIR}/server.toml
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=ircord-server

NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=${DATA_DIR} ${LOG_DIR}
ReadOnlyPaths=/etc/letsencrypt /etc/ircord
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable ircord-server
ok "Systemd service installed and enabled"

# UFW
if command -v ufw &>/dev/null; then
    ufw allow 22/tcp comment "SSH" 2>/dev/null || true
    ufw allow "${IRCORD_PORT}/tcp" comment "IRCord" 2>/dev/null || true
    ufw --force enable 2>/dev/null || true
    ok "UFW: port $IRCORD_PORT open"
fi

# Certbot renewal hook (only for Let's Encrypt)
if [ "$IRCORD_USE_LETSENCRYPT" = "yes" ]; then
    mkdir -p /etc/letsencrypt/renewal-hooks/post
    cat > /etc/letsencrypt/renewal-hooks/post/ircord.sh <<'EOF'
#!/bin/bash
systemctl reload-or-restart ircord-server
EOF
    chmod +x /etc/letsencrypt/renewal-hooks/post/ircord.sh
    ok "Certbot renewal hook installed"
fi

# =============================================================================
step "7/8  Start + verification"
# =============================================================================
systemctl start ircord-server
sleep 3

ACTIVE=$(systemctl is-active ircord-server 2>/dev/null || echo "unknown")
if [ "$ACTIVE" = "active" ]; then
    ok "Server is running"
else
    warn "Server failed to start — check: journalctl -u ircord-server -n 30"
fi

if ss -tlnp | grep -q ":${IRCORD_PORT}"; then
    ok "Port $IRCORD_PORT is listening"
else
    warn "Port $IRCORD_PORT is NOT listening"
fi

# TLS check
if command -v openssl &>/dev/null && [ "$IRCORD_DOMAIN" != "localhost" ]; then
    if echo "Q" | timeout 5 openssl s_client \
        -connect "localhost:${IRCORD_PORT}" \
        2>/dev/null \
        | grep -q "BEGIN CERTIFICATE"; then
        ok "TLS responding"
    else
        warn "TLS check failed (server may still be starting)"
    fi
fi

# =============================================================================
step "8/8  Public listing"
# =============================================================================
if [ "$IRCORD_PUBLIC" = "yes" ]; then
    info "Server is configured as PUBLIC"
    info "It will appear in the public server list at ${IRCORD_DIRECTORY_URL}"
    
    # Wait a moment for the server to register
    sleep 2
    
    # Check registration status from logs
    if journalctl -u ircord-server -n 10 --no-pager 2>/dev/null | grep -q "directory"; then
        ok "Directory client is active"
    else
        info "Check registration status: journalctl -u ircord-server -f"
    fi
else
    info "Server is configured as PRIVATE"
    info "To list publicly, edit $INSTALL_DIR/server.toml and set:"
    info "  [server] public = true"
    info "  [directory] enabled = true"
    info "Then: systemctl restart ircord-server"
fi

# =============================================================================
echo ""
echo -e "${GREEN}═══════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  IRCord Server installed successfully!${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════════${NC}"
echo ""
echo -e "  Address:  ${CYAN}${IRCORD_DOMAIN}:${IRCORD_PORT}${NC}"
echo -e "  Config:   ${INSTALL_DIR}/server.toml"
echo -e "  Data:     ${DATA_DIR}"
echo -e "  Logs:     journalctl -u ircord-server -f"
echo -e "  Status:   systemctl status ircord-server"
echo ""

if [ "$IRCORD_USE_LETSENCRYPT" = "skip" ]; then
echo -e "  ${YELLOW}⚠️  Action required:${NC}"
echo -e "     Place your certificates at:"
echo -e "       ${CERT_PATH}"
echo -e "       ${KEY_PATH}"
echo -e "     Then: systemctl restart ircord-server"
echo ""
fi

echo -e "  Update (new version):"
echo -e "    ${YELLOW}ircord-update${NC}  (or run this script again)"
echo ""

# Create update script
cat > /usr/local/bin/ircord-update <<'EOF'
#!/bin/bash
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
ok()   { echo -e "${GREEN}[ OK ]${NC} $*"; }
error(){ echo -e "${RED}[FAIL]${NC} $*"; exit 1; }

REPO_URL="https://github.com/hittoSepi/ircord-server.git"
BUILD_DIR="/tmp/ircord-update"
VCPKG_DIR="/opt/vcpkg"
INSTALL_DIR="/opt/ircord"
SERVER_DIR="$BUILD_DIR/ircord-server"

[ "$(id -u)" -eq 0 ] || error "Run with sudo: sudo ircord-update"

info "Updating IRCord Server..."

# Backup current binary
cp "$INSTALL_DIR/ircord-server" "$INSTALL_DIR/ircord-server.bak" 2>/dev/null || true

# Clone and build
rm -rf "$BUILD_DIR"
git clone --depth 1 "$REPO_URL" "$BUILD_DIR" 2>&1 | tail -3

cmake -DCMAKE_TOOLCHAIN_FILE="$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake" \
      -DCMAKE_BUILD_TYPE=Release \
      -S "$SERVER_DIR" \
      -B "$SERVER_DIR/build" 2>&1 | tail -10

cmake --build "$SERVER_DIR/build" -j"$(nproc)" 2>&1 | tail -10

[ -f "$SERVER_DIR/build/ircord-server" ] || error "Build failed"

# Install and restart
systemctl stop ircord-server
install -o root -g ircord -m 750 "$SERVER_DIR/build/ircord-server" "$INSTALL_DIR/ircord-server"
systemctl start ircord-server

sleep 2
if [ "$(systemctl is-active ircord-server)" = "active" ]; then
    ok "Update complete — server is running"
    rm -f "$INSTALL_DIR/ircord-server.bak"
else
    error "Update failed — server did not start. Check: journalctl -u ircord-server -n 20"
    # Restore backup
    mv "$INSTALL_DIR/ircord-server.bak" "$INSTALL_DIR/ircord-server" 2>/dev/null || true
    systemctl start ircord-server 2>/dev/null || true
fi

rm -rf "$BUILD_DIR"
EOF
chmod +x /usr/local/bin/ircord-update
ok "Update script: /usr/local/bin/ircord-update"

# Cleanup
rm -rf "$BUILD_DIR"
