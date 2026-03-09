# Let's Encrypt + DNS-01 (Cloudflare) Setup for RPi

Valid TLS certificates for the ircord server using Let's Encrypt with DNS-01 challenge via Cloudflare DNS.

## Prerequisites

- Domain managed by Cloudflare
- RPi with root/sudo access
- Server already running with `cert_file` / `key_file` in `server.toml`

## 1. Install certbot + Cloudflare plugin

```bash
sudo apt update
sudo apt install certbot python3-certbot-dns-cloudflare
```

## 2. Create Cloudflare API token

1. Go to https://dash.cloudflare.com/profile/api-tokens
2. Create Token -> Custom Token
3. Permissions: **Zone : DNS : Edit**
4. Zone Resources: Include -> Specific zone -> your domain
5. Save the token

Create credentials file:

```bash
sudo mkdir -p /etc/letsencrypt
sudo tee /etc/letsencrypt/cloudflare.ini > /dev/null <<EOF
dns_cloudflare_api_token = YOUR_TOKEN_HERE
EOF
sudo chmod 600 /etc/letsencrypt/cloudflare.ini
```

## 3. Request certificate

```bash
sudo certbot certonly \
  --dns-cloudflare \
  --dns-cloudflare-credentials /etc/letsencrypt/cloudflare.ini \
  -d ircord.yourdomain.tld \
  --preferred-challenges dns-01
```

Certificates will be at:
- **Cert:** `/etc/letsencrypt/live/ircord.yourdomain.tld/fullchain.pem`
- **Key:** `/etc/letsencrypt/live/ircord.yourdomain.tld/privkey.pem`

## 4. Update server.toml

```toml
[tls]
cert_file = "/etc/letsencrypt/live/ircord.yourdomain.tld/fullchain.pem"
key_file  = "/etc/letsencrypt/live/ircord.yourdomain.tld/privkey.pem"
```

The ircord server process needs read access to these files. Either run as root (not recommended) or:

```bash
sudo setfacl -m u:ircord:rx /etc/letsencrypt/live/
sudo setfacl -m u:ircord:rx /etc/letsencrypt/archive/
sudo setfacl -m u:ircord:r  /etc/letsencrypt/live/ircord.yourdomain.tld/privkey.pem
```

## 5. Auto-renewal

Certbot installs a systemd timer by default. Verify:

```bash
sudo systemctl status certbot.timer
```

To reload the ircord server after renewal, create a deploy hook:

```bash
sudo tee /etc/letsencrypt/renewal-hooks/deploy/restart-ircord.sh > /dev/null <<'EOF'
#!/bin/bash
systemctl restart ircord-server
EOF
sudo chmod +x /etc/letsencrypt/renewal-hooks/deploy/restart-ircord.sh
```

Test renewal:

```bash
sudo certbot renew --dry-run
```

## 6. Client config

With a valid LE certificate, the client can use standard CA verification. In `client.toml`:

```toml
[tls]
verify_peer = true

[server]
host = "ircord.yourdomain.tld"
port = 6667
# cert_pin is optional but recommended
```
