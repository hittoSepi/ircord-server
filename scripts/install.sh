#!/bin/bash
# =============================================================================
# IrsCord Server — install script
# Ubuntu 22.04+ / Debian 12+, x86_64 tai aarch64
#
# Käyttö:
#   curl -fsSL https://raw.githubusercontent.com/hittoSepi/ircord-server/main/scripts/install.sh | sudo bash
#
# Tai lataa ja aja:
#   wget https://raw.githubusercontent.com/hittoSepi/ircord-server/main/scripts/install.sh
#   chmod +x install.sh
#   sudo ./install.sh
#
# Ympäristömuuttujat (valinnainen):
#   IRCORD_DOMAIN        - domain nimi (oletus: kysytään)
#   IRCORD_CF_TOKEN      - Cloudflare API token (oletus: kysytään)
#   IRCORD_PORT          - palvelimen portti (oletus: 6667)
#   IRCORD_SKIP_CERT     - "1" jos sertifikaatti on jo /etc/letsencrypt/live/$DOMAIN/
# =============================================================================
set -euo pipefail

# --- Värit ja apufunktiot ---------------------------------------------------
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()    { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()      { echo -e "${GREEN}[ OK ]${NC}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[FAIL]${NC}  $*"; exit 1; }
step()    { echo -e "\n${CYAN}══════════════════════════════════════════${NC}"; \
            echo -e "${CYAN}  $*${NC}"; \
            echo -e "${CYAN}══════════════════════════════════════════${NC}"; }

# --- Root-tarkistus -----------------------------------------------------------
[ "$(id -u)" -eq 0 ] || error "Aja sudo:lla: sudo $0"

# --- Muuttujat ----------------------------------------------------------------
IRCORD_PORT="${IRCORD_PORT:-6697}"
INSTALL_DIR="/opt/ircord"
DATA_DIR="/var/lib/ircord"
LOG_DIR="/var/log/ircord"
REPO_URL="https://github.com/hittoSepi/ircord-server.git"
BUILD_DIR="/tmp/ircord-build"
SERVICE_USER="ircord"

# --- Interaktiivinen syöttö (jos ympäristömuuttujia ei annettu) --------------
step "IrsCord Server — asennuksen aloitus"
echo ""

if [ -z "${IRCORD_DOMAIN:-}" ]; then
    read -rp "  Domain (esim. chat.rausku.com): " IRCORD_DOMAIN
fi
[ -n "$IRCORD_DOMAIN" ] || error "Domain ei voi olla tyhjä"

if [ -z "${IRCORD_SKIP_CERT:-}" ]; then
    if [ -z "${IRCORD_CF_TOKEN:-}" ]; then
        read -rsp "  Cloudflare API Token (tai ENTER jos skip): " IRCORD_CF_TOKEN
        echo ""
    fi
fi

info "Domain:   $IRCORD_DOMAIN"
info "Portti:   $IRCORD_PORT"
info "Hakemisto: $INSTALL_DIR"
echo ""

# =============================================================================
step "1/7  Riippuvuudet"
# =============================================================================
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential cmake git curl zip unzip tar pkg-config \
    ninja-build screen \
    libssl-dev libsodium-dev libsqlite3-dev \
    protobuf-compiler libprotobuf-dev \
    libboost-all-dev libspdlog-dev \
    certbot python3-certbot-dns-cloudflare \
    ufw
ok "Riippuvuudet asennettu"

# =============================================================================
step "2/7  vcpkg"
# =============================================================================
VCPKG_DIR="/opt/vcpkg"
if [ ! -f "$VCPKG_DIR/vcpkg" ]; then
    info "Kloonataan vcpkg → $VCPKG_DIR"
    git clone --depth 1 https://github.com/microsoft/vcpkg.git "$VCPKG_DIR" 2>&1 | tail -3
    "$VCPKG_DIR/bootstrap-vcpkg.sh" -disableMetrics 2>&1 | tail -3
    ok "vcpkg asennettu"
else
    ok "vcpkg jo olemassa ($VCPKG_DIR)"
fi

# =============================================================================
step "3/7  Lähdekoodi + build"
# =============================================================================
rm -rf "$BUILD_DIR"
info "Kloonataan $REPO_URL → $BUILD_DIR"
git clone "$REPO_URL" "$BUILD_DIR" 2>&1 | tail -3

info "cmake configure (vcpkg lataa paketit, voi kestää 15-30 min)..."
cmake \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -S "$BUILD_DIR" \
    -B "$BUILD_DIR/build" \
    2>&1 | grep -E '^(--|CMake|error:|warning:|\[)' | tail -40

info "cmake build..."
cmake --build "$BUILD_DIR/build" -j"$(nproc)" 2>&1 | grep -E '(%\]|error:|warning:)' | tail -20

BINARY="$BUILD_DIR/build/ircord-server"
[ -f "$BINARY" ] || error "Binary ei löydy: $BINARY"
ok "Binary builattu: $(du -sh "$BINARY" | cut -f1)"

# =============================================================================
step "4/7  TLS-sertifikaatti"
# =============================================================================
CERT_PATH="/etc/letsencrypt/live/$IRCORD_DOMAIN/fullchain.pem"
KEY_PATH="/etc/letsencrypt/live/$IRCORD_DOMAIN/privkey.pem"

if [ "${IRCORD_SKIP_CERT:-}" = "1" ] || [ -f "$CERT_PATH" ]; then
    ok "Sertifikaatti jo olemassa: $CERT_PATH"
elif [ -n "${IRCORD_CF_TOKEN:-}" ]; then
    info "Haetaan Let's Encrypt cert Cloudflare DNS-haasteella..."
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

    [ -f "$CERT_PATH" ] || error "Sertifikaatin haku epäonnistui"
    ok "Sertifikaatti haettu: $CERT_PATH"
else
    warn "Ei Cloudflare-tokenia — OHITETAAN sertifikaatti"
    warn "Asenna sertifikaatti manuaalisesti ja päivitä $INSTALL_DIR/server.toml"
    CERT_PATH="PUUTTUU_ASENNA_MANUAALISESTI"
    KEY_PATH="PUUTTUU_ASENNA_MANUAALISESTI"
fi

# =============================================================================
step "5/7  Käyttäjä, hakemistot, tiedostot"
# =============================================================================
# Käyttäjä
id "$SERVICE_USER" &>/dev/null || useradd -r -s /bin/false -M "$SERVICE_USER"
ok "Käyttäjä: $SERVICE_USER"

# Hakemistot
mkdir -p "$INSTALL_DIR" "$DATA_DIR" "$LOG_DIR"
chown root:"$SERVICE_USER" "$INSTALL_DIR"
chown "$SERVICE_USER":"$SERVICE_USER" "$DATA_DIR" "$LOG_DIR"
chmod 750 "$INSTALL_DIR" "$DATA_DIR" "$LOG_DIR"

# Binary
install -o root -g "$SERVICE_USER" -m 750 "$BINARY" "$INSTALL_DIR/ircord-server"
ok "Binary asennettu: $INSTALL_DIR/ircord-server"

# Let's Encrypt oikeudet
if [ -d "/etc/letsencrypt/live/$IRCORD_DOMAIN" ]; then
    chmod 750 /etc/letsencrypt/live /etc/letsencrypt/archive 2>/dev/null || true
    chgrp "$SERVICE_USER" /etc/letsencrypt/live /etc/letsencrypt/archive 2>/dev/null || true
    chgrp -R "$SERVICE_USER" "/etc/letsencrypt/live/$IRCORD_DOMAIN" \
                              "/etc/letsencrypt/archive/$IRCORD_DOMAIN" 2>/dev/null || true
    chmod -R g+r "/etc/letsencrypt/archive/$IRCORD_DOMAIN" 2>/dev/null || true
fi

# server.toml
cat > "$INSTALL_DIR/server.toml" <<EOF
[server]
host = "0.0.0.0"
port = ${IRCORD_PORT}
log_level = "info"
max_connections = 100

[tls]
cert_file = "${CERT_PATH}"
key_file  = "${KEY_PATH}"

[database]
path = "${DATA_DIR}/ircord.db"
EOF
ok "Config kirjoitettu: $INSTALL_DIR/server.toml"

# =============================================================================
step "6/7  Systemd + palomuuri + certbot-hook"
# =============================================================================
cat > /etc/systemd/system/ircord-server.service <<EOF
[Unit]
Description=IrsCord Chat Server
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
ReadOnlyPaths=/etc/letsencrypt
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable ircord-server
ok "Systemd service asennettu ja enabled"

# UFW
if command -v ufw &>/dev/null; then
    ufw allow 22/tcp  comment "SSH"    2>/dev/null || true
    ufw allow "${IRCORD_PORT}/tcp" comment "IrsCord" 2>/dev/null || true
    ufw --force enable 2>/dev/null || true
    ok "UFW: portti $IRCORD_PORT auki"
fi

# Certbot renewal hook
mkdir -p /etc/letsencrypt/renewal-hooks/post
cat > /etc/letsencrypt/renewal-hooks/post/ircord.sh <<'EOF'
#!/bin/bash
systemctl reload-or-restart ircord-server
EOF
chmod +x /etc/letsencrypt/renewal-hooks/post/ircord.sh
ok "Certbot renewal hook asennettu"

# =============================================================================
step "7/7  Käynnistys + tarkistus"
# =============================================================================
systemctl start ircord-server
sleep 3

ACTIVE=$(systemctl is-active ircord-server 2>/dev/null || echo "unknown")
if [ "$ACTIVE" = "active" ]; then
    ok "Palvelin käynnissä"
else
    warn "Palvelin ei käynnisty — tarkista: journalctl -u ircord-server -n 30"
fi

if ss -tlnp | grep -q ":${IRCORD_PORT}"; then
    ok "Portti $IRCORD_PORT kuuntelee"
else
    warn "Portti $IRCORD_PORT EI kuuntele"
fi

# TLS-tarkistus (jos openssl saatavilla ja domain ei ole localhost)
if command -v openssl &>/dev/null && [ "$IRCORD_DOMAIN" != "localhost" ]; then
    if echo "Q" | timeout 5 openssl s_client \
        -connect "${IRCORD_DOMAIN}:${IRCORD_PORT}" \
        -servername "$IRCORD_DOMAIN" 2>/dev/null \
        | grep -q "Verify return code: 0"; then
        ok "TLS OK — sertifikaatti validi"
    else
        warn "TLS-tarkistus epäonnistui (DNS ei ehkä propagoitunut vielä)"
    fi
fi

# =============================================================================
echo ""
echo -e "${GREEN}═══════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  IrsCord Server asennettu!${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════════${NC}"
echo ""
echo -e "  Osoite:   ${CYAN}${IRCORD_DOMAIN}:${IRCORD_PORT}${NC}"
echo -e "  Config:   ${INSTALL_DIR}/server.toml"
echo -e "  Loki:     journalctl -u ircord-server -f"
echo -e "  Status:   systemctl status ircord-server"
echo ""
echo -e "  Päivitys (uusi versio):"
echo -e "    ${YELLOW}ircord-update${NC}  (tai aja tämä skripti uudelleen)"
echo ""

# Luo päivitysskripti
cat > /usr/local/bin/ircord-update <<UPDEOF
#!/bin/bash
set -euo pipefail
BUILD_DIR="/tmp/ircord-build"
VCPKG_DIR="/opt/vcpkg"
INSTALL_DIR="/opt/ircord"
echo "Haetaan päivitys..."
rm -rf "\$BUILD_DIR"
git clone --depth 1 ${REPO_URL} "\$BUILD_DIR"
cmake -DCMAKE_TOOLCHAIN_FILE="\$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake" \
      -DCMAKE_BUILD_TYPE=Release -S "\$BUILD_DIR" -B "\$BUILD_DIR/build"
cmake --build "\$BUILD_DIR/build" -j\$(nproc)
systemctl stop ircord-server
install -o root -g ircord -m 750 "\$BUILD_DIR/build/ircord-server" "\$INSTALL_DIR/ircord-server"
systemctl start ircord-server
echo "Päivitys valmis — \$(systemctl is-active ircord-server)"
UPDEOF
chmod +x /usr/local/bin/ircord-update
ok "Päivitysskripti: /usr/local/bin/ircord-update"

# Siivoa build-hakemisto
rm -rf "$BUILD_DIR"
