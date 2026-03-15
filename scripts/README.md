# IRCord Server Scripts

## install.sh

Stage-1 bootstrap for the IRCord Linux installer.

### Quick Install

```bash
curl -fsSL https://chat.rausku.com/downloads/install.sh | sudo bash
```

The bootstrap script does only four things:

1. Detect Linux architecture
2. Download the matching `ircord-installer` binary
3. Elevate with `sudo` if needed
4. Launch the installer wizard

## install-legacy-build.sh

Legacy source-build installer kept for maintainers and fallback use.

### Legacy Usage

```bash
wget https://raw.githubusercontent.com/hittoSepi/ircord/main/ircord-server/scripts/install-legacy-build.sh
chmod +x install-legacy-build.sh
sudo ./install-legacy-build.sh
```

### What It Does

1. **Asks for configuration:**
   - Domain name (e.g., `chat.example.com`)
   - SSL certificate option:
     - Let's Encrypt (standalone or Cloudflare DNS)
     - Self-signed (for testing)
     - Use your own certificates
   - Public listing (yes/no)

2. **Installs dependencies:**
   - build-essential, cmake, git
   - vcpkg (C++ package manager)
   - certbot (for Let's Encrypt)
   - ufw (firewall)

3. **Builds the server:**
   - Clones the repository
   - Downloads dependencies via vcpkg
   - Compiles in Release mode

4. **Configures the system:**
   - Creates `ircord` user
   - Sets up directories (`/opt/ircord`, `/var/lib/ircord`)
   - Obtains SSL certificates
   - Creates `server.toml` config
   - Installs systemd service
   - Configures firewall

5. **Starts the server** and verifies it's running

### After Installation

```bash
# Check status
systemctl status ircord-server

# View logs
journalctl -u ircord-server -f

# Edit config
nano /opt/ircord/server.toml
systemctl restart ircord-server

# Update to latest version
sudo ircord-update
```

### Environment Variables

You can automate installation by setting these before running:

```bash
export IRCORD_DOMAIN="chat.example.com"
export IRCORD_PORT="6697"
export IRCORD_USE_LETSENCRYPT="yes"        # yes | selfsigned | skip
export IRCORD_LE_METHOD="standalone"       # standalone | dns-cloudflare
export IRCORD_CF_TOKEN="your-cloudflare-token"  # for DNS method
export IRCORD_PUBLIC="yes"                 # yes | no

curl -fsSL ... | sudo bash
```

### SSL Options

| Option | Use Case | Requirements |
|--------|----------|--------------|
| Let's Encrypt (standalone) | Public server with domain | Port 80 free during install |
| Let's Encrypt (Cloudflare) | Public server, wildcard domains | Cloudflare API token |
| Self-signed | Testing, private networks | None |
| Skip | You provide certificates | Manual certificate setup |

### Troubleshooting

**Port 80 in use for Let's Encrypt standalone:**
```bash
# Stop other web server temporarily
sudo systemctl stop nginx  # or apache2
sudo ./install.sh
sudo systemctl start nginx
```

**Build fails:**
```bash
# Check disk space (needs ~10GB)
df -h

# Check memory (needs ~2GB RAM)
free -h

# Try with fewer parallel jobs
cd /tmp/ircord-build/build
cmake --build . -j1
```

**Certificate issues:**
```bash
# Test certificate renewal
sudo certbot renew --dry-run

# Force certificate reissue
sudo certbot certonly --force-renew -d your-domain.com
```

## ircord-generate-file-encryption-key

Helper script to generate a secure key for file encryption.

```bash
./ircord-generate-file-encryption-key
# Output: 64-character hex string

# Add to server.toml:
echo "file_encryption_key = \"$(./ircord-generate-file-encryption-key)\"" >> /opt/ircord/server.toml
```
