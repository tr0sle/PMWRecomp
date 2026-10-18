#!/usr/bin/env bash
# One-time setup of a fresh Ubuntu/Debian server (e.g. a Spaceship Starlight VM).
# Runs ON the server, as root:  bash setup-server.sh your-domain.com
# Installs Node.js 22 and Caddy (automatic HTTPS), creates /opt/pmwr and the pmwr service.
set -euo pipefail
DOMAIN=${1:?usage: setup-server.sh DOMAIN}

export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y curl ca-certificates gnupg debian-keyring debian-archive-keyring apt-transport-https ufw

if ! command -v node >/dev/null || [ "$(node -v | cut -d. -f1 | tr -d v)" -lt 20 ]; then
  curl -fsSL https://deb.nodesource.com/setup_22.x | bash -
  apt-get install -y nodejs
fi

if ! command -v caddy >/dev/null; then
  curl -1sLf https://dl.cloudsmith.io/public/caddy/stable/gpg.key | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
  curl -1sLf https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt > /etc/apt/sources.list.d/caddy-stable.list
  apt-get update -y
  apt-get install -y caddy
fi

id pmwr >/dev/null 2>&1 || useradd --system --home /opt/pmwr --shell /usr/sbin/nologin pmwr
mkdir -p /opt/pmwr/dist /opt/pmwr/server
chown -R pmwr:pmwr /opt/pmwr

cat > /etc/systemd/system/pmwr.service <<EOF
[Unit]
Description=Pac-Man World Rally web (site + online rooms)
After=network.target

[Service]
User=pmwr
WorkingDirectory=/opt/pmwr
ExecStart=/usr/bin/node /opt/pmwr/server/server.js --web /opt/pmwr/dist --no-game --port 8080
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/caddy/Caddyfile <<EOF
$DOMAIN {
    encode zstd gzip
    reverse_proxy localhost:8080
}
EOF

ufw allow OpenSSH >/dev/null; ufw allow 80/tcp >/dev/null; ufw allow 443/tcp >/dev/null; ufw --force enable >/dev/null

systemctl daemon-reload
systemctl enable pmwr caddy >/dev/null 2>&1
systemctl restart caddy
echo "setup ok: node $(node -v), caddy $(caddy version | cut -d' ' -f1), site https://$DOMAIN"
