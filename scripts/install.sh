#!/bin/bash
# =============================================================================
# IRCord Server - Install Script
# Ubuntu 22.04+ / Debian 12+, x86_64 or aarch64
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/hittoSepi/ircord/main/ircord-server/scripts/install.sh | sudo bash
#
# Environment variables (optional):
#   IRCORD_DOMAIN          - Domain name
#   IRCORD_PORT            - Server port (default: 6697)
#   IRCORD_USE_LETSENCRYPT - "yes", "selfsigned", or "skip"
#   IRCORD_LE_METHOD       - "standalone", "dns-cloudflare", or "skip"
#   IRCORD_CF_TOKEN        - Cloudflare API token (for DNS method)
#   IRCORD_PUBLIC          - "yes" or "no"
#   IRCORD_DIRECTORY_URL   - Directory service URL
#   IRCORD_TURN_ENABLED    - "yes" or "no"
#   IRCORD_TURN_DOMAIN     - TURN domain (default: IRCORD_DOMAIN)
#   IRCORD_TURN_REALM      - TURN realm (default: TURN domain)
#   IRCORD_TURN_USERNAME   - Static TURN username
#   IRCORD_TURN_PASSWORD   - Static TURN password
# =============================================================================
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()    { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()      { echo -e "${GREEN}[ OK ]${NC}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[FAIL]${NC}  $*"; exit 1; }
step()    {
    echo ""
    echo -e "${CYAN}============================================================${NC}"
    echo -e "${CYAN}  $*${NC}"
    echo -e "${CYAN}============================================================${NC}"
}

[ "$(id -u)" -eq 0 ] || error "Run with sudo: sudo $0"

IRCORD_PORT="${IRCORD_PORT:-6697}"
INSTALL_DIR="/opt/ircord"
DATA_DIR="/var/lib/ircord"
LOG_DIR="/var/log/ircord"
REPO_URL="https://github.com/hittoSepi/ircord-server.git"
BUILD_DIR="/tmp/ircord-build"
SERVICE_USER="ircord"
TURN_CONFIG_PATH="/etc/turnserver.conf"

validate_domain() {
    local domain="$1"
    if [[ ! "$domain" =~ ^[a-zA-Z0-9][a-zA-Z0-9.-]*\.[a-zA-Z]{2,}$ ]]; then
        warn "Domain format looks unusual: $domain"
        read -rp "  Continue anyway? (y/N) " -n 1 -r
        echo ""
        [[ $REPLY =~ ^[Yy]$ ]] || exit 1
    fi
}

prepare_turn_settings() {
    if [ -z "${IRCORD_TURN_ENABLED:-}" ]; then
        read -rp "  Install TURN/ICE server for voice? (Y/n) " -n 1 -r
        echo ""
        [[ -z "${REPLY:-}" || $REPLY =~ ^[Yy]$ ]] && IRCORD_TURN_ENABLED="yes" || IRCORD_TURN_ENABLED="no"
    fi

    if [ "${IRCORD_TURN_ENABLED}" != "yes" ]; then
        IRCORD_TURN_ENABLED="no"
        return
    fi

    if [ -z "${IRCORD_TURN_DOMAIN:-}" ]; then
        read -rp "  TURN domain [${IRCORD_DOMAIN}]: " IRCORD_TURN_DOMAIN
        echo ""
    fi
    IRCORD_TURN_DOMAIN="${IRCORD_TURN_DOMAIN:-$IRCORD_DOMAIN}"
    validate_domain "$IRCORD_TURN_DOMAIN"
    IRCORD_TURN_REALM="${IRCORD_TURN_REALM:-$IRCORD_TURN_DOMAIN}"

    if [ -z "${IRCORD_TURN_USERNAME:-}" ]; then
        read -rp "  TURN username [ircord]: " IRCORD_TURN_USERNAME
        echo ""
    fi
    IRCORD_TURN_USERNAME="${IRCORD_TURN_USERNAME:-ircord}"

    if [ -z "${IRCORD_TURN_PASSWORD:-}" ]; then
        local generated_password
        generated_password="$(openssl rand -hex 16)"
        read -rp "  TURN password [${generated_password}]: " IRCORD_TURN_PASSWORD
        echo ""
        IRCORD_TURN_PASSWORD="${IRCORD_TURN_PASSWORD:-$generated_password}"
    fi
}

detect_turn_external_ip() {
    local ip=""
    ip="$(getent ahostsv4 "$IRCORD_TURN_DOMAIN" 2>/dev/null | awk 'NR==1 {print $1; exit}')"
    if [ -z "$ip" ]; then
        ip="$(curl -4 -fsS https://api.ipify.org 2>/dev/null || true)"
    fi
    if [ -z "$ip" ]; then
        ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
    fi
    printf '%s' "$ip"
}

obtain_letsencrypt_cert() {
    local domain="$1"
    if [ -f "/etc/letsencrypt/live/$domain/fullchain.pem" ] && [ -f "/etc/letsencrypt/live/$domain/privkey.pem" ]; then
        ok "Certificate already exists for $domain"
        return
    fi

    if [ "${IRCORD_LE_METHOD:-standalone}" = "dns-cloudflare" ]; then
        local cf_creds="/root/.cloudflare-ircord.ini"
        printf 'dns_cloudflare_api_token = %s\n' "$IRCORD_CF_TOKEN" > "$cf_creds"
        chmod 600 "$cf_creds"
        certbot certonly \
            --dns-cloudflare \
            --dns-cloudflare-credentials "$cf_creds" \
            -d "$domain" \
            --non-interactive \
            --agree-tos \
            --email "admin@${domain#*.}"
    else
        certbot certonly \
            --standalone \
            -d "$domain" \
            --non-interactive \
            --agree-tos \
            --email "admin@${domain#*.}"
    fi
}

prepare_turn_certificate() {
    TURN_CERT_PATH="$CERT_PATH"
    TURN_KEY_PATH="$KEY_PATH"

    if [ "${IRCORD_TURN_ENABLED}" != "yes" ] || [ "$IRCORD_TURN_DOMAIN" = "$IRCORD_DOMAIN" ]; then
        return
    fi

    case "$IRCORD_USE_LETSENCRYPT" in
        yes)
            obtain_letsencrypt_cert "$IRCORD_TURN_DOMAIN"
            TURN_CERT_PATH="/etc/letsencrypt/live/$IRCORD_TURN_DOMAIN/fullchain.pem"
            TURN_KEY_PATH="/etc/letsencrypt/live/$IRCORD_TURN_DOMAIN/privkey.pem"
            ;;
        selfsigned)
            mkdir -p /etc/ircord
            TURN_CERT_PATH="/etc/ircord/turn-cert.pem"
            TURN_KEY_PATH="/etc/ircord/turn-key.pem"
            openssl req -x509 -newkey rsa:4096 \
                -keyout "$TURN_KEY_PATH" \
                -out "$TURN_CERT_PATH" \
                -days 365 \
                -nodes \
                -subj "/CN=$IRCORD_TURN_DOMAIN" \
                >/dev/null 2>&1
            chmod 644 "$TURN_CERT_PATH"
            chmod 600 "$TURN_KEY_PATH"
            ;;
        skip)
            TURN_CERT_PATH="/etc/ircord/turn-cert.pem"
            TURN_KEY_PATH="/etc/ircord/turn-key.pem"
            warn "TURN uses separate domain ${IRCORD_TURN_DOMAIN}"
            warn "Place TURN certificate at $TURN_CERT_PATH"
            warn "Place TURN private key at $TURN_KEY_PATH"
            ;;
        *)
            error "Unknown SSL option: $IRCORD_USE_LETSENCRYPT"
            ;;
    esac
}

configure_turn_cert_access() {
    if [ "${IRCORD_TURN_ENABLED}" != "yes" ]; then
        return
    fi

    if id turnserver >/dev/null 2>&1; then
        usermod -a -G "$SERVICE_USER" turnserver 2>/dev/null || true
    fi

    if [ "$IRCORD_USE_LETSENCRYPT" = "yes" ]; then
        for domain in "$IRCORD_DOMAIN" "$IRCORD_TURN_DOMAIN"; do
            [ -d "/etc/letsencrypt/live/$domain" ] || continue
            chmod 750 /etc/letsencrypt/live /etc/letsencrypt/archive 2>/dev/null || true
            chgrp "$SERVICE_USER" /etc/letsencrypt/live /etc/letsencrypt/archive 2>/dev/null || true
            chgrp -R "$SERVICE_USER" "/etc/letsencrypt/live/$domain" "/etc/letsencrypt/archive/$domain" 2>/dev/null || true
            chmod -R g+r "/etc/letsencrypt/archive/$domain" 2>/dev/null || true
        done
    else
        chgrp "$SERVICE_USER" "$TURN_CERT_PATH" "$TURN_KEY_PATH" 2>/dev/null || true
        chmod 644 "$TURN_CERT_PATH" 2>/dev/null || true
        chmod 640 "$TURN_KEY_PATH" 2>/dev/null || true
    fi
}

write_turn_config() {
    if [ "${IRCORD_TURN_ENABLED}" != "yes" ]; then
        return
    fi

    local external_ip
    external_ip="$(detect_turn_external_ip)"

    cat > "$TURN_CONFIG_PATH" <<EOF
fingerprint
lt-cred-mech
realm=${IRCORD_TURN_REALM}
user=${IRCORD_TURN_USERNAME}:${IRCORD_TURN_PASSWORD}
listening-ip=0.0.0.0
${external_ip:+external-ip=${external_ip}}
listening-port=3478
tls-listening-port=5349
min-port=49160
max-port=49200
cert=${TURN_CERT_PATH}
pkey=${TURN_KEY_PATH}
no-cli
no-multicast-peers
no-tlsv1
no-tlsv1_1
EOF

    cat > /etc/default/coturn <<EOF
TURNSERVER_ENABLED=1
EOF

    ok "TURN config written: $TURN_CONFIG_PATH"
}

install_update_script() {
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
cp "$INSTALL_DIR/ircord-server" "$INSTALL_DIR/ircord-server.bak" 2>/dev/null || true

rm -rf "$BUILD_DIR"
git clone --depth 1 "$REPO_URL" "$BUILD_DIR" 2>&1 | tail -3

cmake -DCMAKE_TOOLCHAIN_FILE="$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake" \
      -DCMAKE_BUILD_TYPE=Release \
      -S "$SERVER_DIR" \
      -B "$SERVER_DIR/build" 2>&1 | tail -10

cmake --build "$SERVER_DIR/build" -j"$(nproc)" 2>&1 | tail -10
[ -f "$SERVER_DIR/build/ircord-server" ] || error "Build failed"

systemctl stop ircord-server
install -o root -g ircord -m 750 "$SERVER_DIR/build/ircord-server" "$INSTALL_DIR/ircord-server"
systemctl start ircord-server

sleep 2
if [ "$(systemctl is-active ircord-server)" = "active" ]; then
    ok "Update complete - server is running"
    rm -f "$INSTALL_DIR/ircord-server.bak"
else
    error "Update failed - server did not start. Check: journalctl -u ircord-server -n 20"
    mv "$INSTALL_DIR/ircord-server.bak" "$INSTALL_DIR/ircord-server" 2>/dev/null || true
    systemctl start ircord-server 2>/dev/null || true
fi

rm -rf "$BUILD_DIR"
EOF
    chmod +x /usr/local/bin/ircord-update
    ok "Update script: /usr/local/bin/ircord-update"
}

step "IRCord Server Installation"
echo ""

if [ -z "${IRCORD_DOMAIN:-}" ]; then
    read -rp "  Domain (e.g., chat.example.com): " IRCORD_DOMAIN
    echo ""
fi
[ -n "$IRCORD_DOMAIN" ] || error "Domain cannot be empty"
validate_domain "$IRCORD_DOMAIN"

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

if [ "$IRCORD_USE_LETSENCRYPT" = "yes" ] && [ "${IRCORD_LE_METHOD:-}" = "dns-cloudflare" ] && [ -z "${IRCORD_CF_TOKEN:-}" ]; then
    echo "  Cloudflare API Token required for DNS validation."
    read -rsp "  Cloudflare API Token: " IRCORD_CF_TOKEN
    echo ""
fi

if [ -z "${IRCORD_PUBLIC:-}" ]; then
    read -rp "  List this server publicly? (y/N) " -n 1 -r
    echo ""
    [[ $REPLY =~ ^[Yy]$ ]] && IRCORD_PUBLIC="yes" || IRCORD_PUBLIC="no"
fi

if [ "$IRCORD_PUBLIC" = "yes" ] && [ -z "${IRCORD_DIRECTORY_URL:-}" ]; then
    read -rp "  Directory server URL [https://directory.ircord.dev]: " IRCORD_DIRECTORY_URL
    echo ""
fi
IRCORD_DIRECTORY_URL="${IRCORD_DIRECTORY_URL:-https://directory.ircord.dev}"

prepare_turn_settings

echo ""
info "Configuration summary:"
echo "  Domain:     $IRCORD_DOMAIN"
echo "  Port:       $IRCORD_PORT"
echo "  SSL:        $IRCORD_USE_LETSENCRYPT"
[ "$IRCORD_USE_LETSENCRYPT" = "yes" ] && echo "  LE Method:  ${IRCORD_LE_METHOD:-standalone}"
echo "  Public:     $IRCORD_PUBLIC"
[ "$IRCORD_PUBLIC" = "yes" ] && echo "  Directory:  $IRCORD_DIRECTORY_URL"
echo "  TURN:       $IRCORD_TURN_ENABLED"
if [ "$IRCORD_TURN_ENABLED" = "yes" ]; then
    echo "  TURN domain: $IRCORD_TURN_DOMAIN"
    echo "  TURN user:   $IRCORD_TURN_USERNAME"
fi
echo "  Install:    $INSTALL_DIR"
echo ""
read -rp "  Proceed with installation? (Y/n) " -n 1 -r
echo ""
[[ $REPLY =~ ^[Nn]$ ]] && exit 0

step "1/8 Dependencies"
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential cmake git curl zip unzip tar pkg-config \
    ninja-build screen openssl \
    libssl-dev libsodium-dev libsqlite3-dev \
    protobuf-compiler libprotobuf-dev \
    libboost-all-dev libspdlog-dev \
    certbot ufw jq
if [ "$IRCORD_USE_LETSENCRYPT" = "yes" ] && [ "${IRCORD_LE_METHOD:-}" = "dns-cloudflare" ]; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq python3-certbot-dns-cloudflare
fi
if [ "$IRCORD_TURN_ENABLED" = "yes" ]; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq coturn
fi
ok "Dependencies installed"

step "2/8 vcpkg"
VCPKG_DIR="/opt/vcpkg"
if [ ! -f "$VCPKG_DIR/vcpkg" ]; then
    info "Cloning vcpkg -> $VCPKG_DIR"
    git clone --depth 1 https://github.com/microsoft/vcpkg.git "$VCPKG_DIR" 2>&1 | tail -3
    "$VCPKG_DIR/bootstrap-vcpkg.sh" -disableMetrics 2>&1 | tail -3
    ok "vcpkg installed"
else
    ok "vcpkg already exists ($VCPKG_DIR)"
fi

step "3/8 Source code + build"
rm -rf "$BUILD_DIR"
info "Cloning $REPO_URL -> $BUILD_DIR"
git clone --depth 1 "$REPO_URL" "$BUILD_DIR/ircord-server" 2>&1 | tail -3
SERVER_DIR="$BUILD_DIR/ircord-server"
[ -d "$SERVER_DIR" ] || error "Server directory not found: $SERVER_DIR"

info "CMake configure..."
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

step "4/8 SSL Certificate"
CERT_PATH="/etc/letsencrypt/live/$IRCORD_DOMAIN/fullchain.pem"
KEY_PATH="/etc/letsencrypt/live/$IRCORD_DOMAIN/privkey.pem"

if [ "$IRCORD_USE_LETSENCRYPT" = "skip" ]; then
    mkdir -p /etc/ircord
    CERT_PATH="/etc/ircord/cert.pem"
    KEY_PATH="/etc/ircord/key.pem"
    ok "Skipping certificate generation for IRCord server"
elif [ "$IRCORD_USE_LETSENCRYPT" = "selfsigned" ]; then
    mkdir -p /etc/ircord
    CERT_PATH="/etc/ircord/cert.pem"
    KEY_PATH="/etc/ircord/key.pem"
    openssl req -x509 -newkey rsa:4096 \
        -keyout "$KEY_PATH" \
        -out "$CERT_PATH" \
        -days 365 \
        -nodes \
        -subj "/CN=$IRCORD_DOMAIN" \
        >/dev/null 2>&1
    chmod 644 "$CERT_PATH"
    chmod 600 "$KEY_PATH"
    ok "Self-signed certificate generated"
else
    obtain_letsencrypt_cert "$IRCORD_DOMAIN"
    ok "Certificate obtained: $CERT_PATH"
fi

prepare_turn_certificate

step "5/8 User, directories, files"
id "$SERVICE_USER" &>/dev/null || useradd -r -s /bin/false -M "$SERVICE_USER"
ok "User: $SERVICE_USER"

mkdir -p "$INSTALL_DIR" "$DATA_DIR" "$LOG_DIR"
chown root:"$SERVICE_USER" "$INSTALL_DIR"
chown "$SERVICE_USER":"$SERVICE_USER" "$DATA_DIR" "$LOG_DIR"
chmod 750 "$INSTALL_DIR" "$DATA_DIR" "$LOG_DIR"

install -o root -g "$SERVICE_USER" -m 750 "$BINARY" "$INSTALL_DIR/ircord-server"
ok "Binary installed: $INSTALL_DIR/ircord-server"

configure_turn_cert_access

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
max_message_bytes = 65536
ping_interval_sec = 30
ping_timeout_sec = 60
msg_rate_per_sec = 20
conn_rate_per_min = 10
commands_per_min = 30
joins_per_min = 5
abuse_threshold = 5
abuse_window_min = 10
ban_duration_min = 30

[security]
file_encryption_key = "${FILE_ENCRYPTION_KEY}"

[antivirus]
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

write_turn_config

step "6/8 Systemd service + firewall + renewal"
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

if [ "$IRCORD_TURN_ENABLED" = "yes" ]; then
    systemctl enable coturn
    ok "coturn service enabled"
fi

if command -v ufw &>/dev/null; then
    ufw allow 22/tcp comment "SSH" 2>/dev/null || true
    ufw allow "${IRCORD_PORT}/tcp" comment "IRCord" 2>/dev/null || true
    if [ "$IRCORD_TURN_ENABLED" = "yes" ]; then
        ufw allow 3478/tcp comment "TURN TCP" 2>/dev/null || true
        ufw allow 3478/udp comment "TURN UDP" 2>/dev/null || true
        ufw allow 5349/tcp comment "TURNS TCP" 2>/dev/null || true
        ufw allow 5349/udp comment "TURNS UDP" 2>/dev/null || true
        ufw allow 49160:49200/udp comment "TURN relay UDP" 2>/dev/null || true
    fi
    ufw --force enable 2>/dev/null || true
    ok "UFW configured"
fi

if [ "$IRCORD_USE_LETSENCRYPT" = "yes" ]; then
    mkdir -p /etc/letsencrypt/renewal-hooks/post
    cat > /etc/letsencrypt/renewal-hooks/post/ircord.sh <<EOF
#!/bin/bash
systemctl reload-or-restart ircord-server
$( [ "$IRCORD_TURN_ENABLED" = "yes" ] && echo "systemctl restart coturn" )
EOF
    chmod +x /etc/letsencrypt/renewal-hooks/post/ircord.sh
    ok "Certbot renewal hook installed"
fi

step "7/8 Start + verification"
systemctl start ircord-server
sleep 3
if [ "$(systemctl is-active ircord-server 2>/dev/null || echo unknown)" = "active" ]; then
    ok "Server is running"
else
    warn "Server failed to start - check: journalctl -u ircord-server -n 30"
fi

if ss -tlnp | grep -q ":${IRCORD_PORT}"; then
    ok "Port $IRCORD_PORT is listening"
else
    warn "Port $IRCORD_PORT is NOT listening"
fi

if [ "$IRCORD_TURN_ENABLED" = "yes" ]; then
    systemctl restart coturn
    sleep 2
    if [ "$(systemctl is-active coturn 2>/dev/null || echo unknown)" = "active" ]; then
        ok "coturn is running"
    else
        warn "coturn failed to start - check: journalctl -u coturn -n 30"
    fi

    if ss -tuln | grep -qE ':(3478|5349)\s'; then
        ok "TURN ports are listening"
    else
        warn "TURN ports are not listening"
    fi
fi

step "8/8 Public listing"
if [ "$IRCORD_PUBLIC" = "yes" ]; then
    info "Server is configured as PUBLIC"
    info "It will appear in the public server list at ${IRCORD_DIRECTORY_URL}"
else
    info "Server is configured as PRIVATE"
fi

echo ""
echo -e "${GREEN}============================================================${NC}"
echo -e "${GREEN}  IRCord Server installed successfully!${NC}"
echo -e "${GREEN}============================================================${NC}"
echo ""
echo -e "  Address:  ${CYAN}${IRCORD_DOMAIN}:${IRCORD_PORT}${NC}"
echo -e "  Config:   ${INSTALL_DIR}/server.toml"
echo -e "  Data:     ${DATA_DIR}"
echo -e "  Logs:     journalctl -u ircord-server -f"
if [ "$IRCORD_TURN_ENABLED" = "yes" ]; then
    echo -e "  TURN:     ${CYAN}${IRCORD_TURN_DOMAIN}${NC} (3478 / 5349)"
    echo -e "  TURN cfg: ${TURN_CONFIG_PATH}"
fi
echo ""

install_update_script
rm -rf "$BUILD_DIR"
