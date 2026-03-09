# Cloudflare Tunnel Setup for ircord

Expose the ircord server to the internet without opening ports.

## Important limitation

Cloudflare Tunnels natively proxy HTTP/WebSocket traffic. The ircord wire protocol is raw TCP (length-prefixed protobuf over TLS), so the tunnel must run in **TCP mode**. This requires `cloudflared` on both server and client sides.

For simpler external access, consider opening port 6667 with a valid Let's Encrypt certificate instead (see `letsencrypt-setup.md`).

## Server side (RPi)

### 1. Install cloudflared

```bash
curl -L https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-arm64 -o /usr/local/bin/cloudflared
chmod +x /usr/local/bin/cloudflared
```

### 2. Authenticate

```bash
cloudflared tunnel login
```

### 3. Create tunnel

```bash
cloudflared tunnel create ircord
```

Note the tunnel UUID printed.

### 4. Configure

Create `~/.cloudflared/config.yml`:

```yaml
tunnel: <TUNNEL_UUID>
credentials-file: /root/.cloudflared/<TUNNEL_UUID>.json

ingress:
  - hostname: ircord.yourdomain.tld
    service: tcp://localhost:6667
  - service: http_status:404
```

### 5. DNS record

```bash
cloudflared tunnel route dns ircord ircord.yourdomain.tld
```

This creates a CNAME pointing to `<TUNNEL_UUID>.cfargotunnel.com`.

### 6. Run as systemd service

```bash
sudo cloudflared service install
sudo systemctl enable cloudflared
sudo systemctl start cloudflared
```

## Client side

Since this is a TCP tunnel (not HTTP), the client machine also needs `cloudflared` to create a local proxy:

```bash
cloudflared access tcp --hostname ircord.yourdomain.tld --url localhost:16667
```

Then configure `client.toml` to connect to the local proxy:

```toml
[server]
host = "127.0.0.1"
port = 16667

[tls]
verify_peer = false   # cloudflared handles the Cloudflare<->origin TLS
```

Keep `cloudflared access tcp` running while using the client.

## Future: WebSocket transport

A cleaner solution would be adding a WebSocket transport to ircord (wrapping the existing length-prefixed protocol in WebSocket frames). This would let Cloudflare Tunnel work natively without requiring `cloudflared` on the client side. This is tracked as a future enhancement.
