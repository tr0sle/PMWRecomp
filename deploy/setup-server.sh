#!/usr/bin/env bash
# One-time setup of a fresh Ubuntu/Debian server (e.g. a Spaceship Starlight VM).
# Runs ON the server, as root:  bash setup-server.sh "example.com, www.example.com"
# (one or more site addresses, comma-separated, as Caddy expects them)
# Installs Node.js 22 and Caddy (automatic HTTPS), creates /opt/pmwr and the pmwr service.
set -euo pipefail
DOMAIN=${1:?usage: setup-server.sh "DOMAIN[, OTHER]" [SSH_PORT]}
SSH_PORT=${2:-22}

export DEBIAN_FRONTEND=noninteractive
# a fresh VM often runs its automatic updates first: wait for apt to be free
wait_apt() { for i in $(seq 180); do pgrep -x apt-get >/dev/null || pgrep -x dpkg >/dev/null || pgrep -f '^/usr/bin/python3 /usr/bin/unattended-upgrade' >/dev/null || return 0; sleep 5; done; }
wait_apt
apt-get update -y
wait_apt; apt-get -o DPkg::Lock::Timeout=600 install -y curl ca-certificates gnupg debian-keyring debian-archive-keyring apt-transport-https ufw

if ! command -v node >/dev/null || [ "$(node -v | cut -d. -f1 | tr -d v)" -lt 20 ]; then
  wait_apt; curl -fsSL https://deb.nodesource.com/setup_22.x | bash -
  wait_apt; apt-get -o DPkg::Lock::Timeout=600 install -y nodejs
fi

if ! command -v caddy >/dev/null; then
  curl -1sLf https://dl.cloudsmith.io/public/caddy/stable/gpg.key | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
  curl -1sLf https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt > /etc/apt/sources.list.d/caddy-stable.list
  wait_apt; apt-get update -y
  wait_apt; apt-get -o DPkg::Lock::Timeout=600 install -y caddy
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

# firewall: keep every port sshd listens on (some hosts move SSH off port 22), plus HTTP/HTTPS
for port in $(ss -tlnpH | awk '/sshd/ {n=split($4,a,":"); print a[n]}' | sort -u) "$SSH_PORT" 22; do ufw allow "$port/tcp" >/dev/null; done
ufw allow 80/tcp >/dev/null; ufw allow 443/tcp >/dev/null; ufw --force enable >/dev/null

systemctl daemon-reload
systemctl enable pmwr caddy >/dev/null 2>&1
systemctl restart caddy
echo "setup ok: node $(node -v), caddy $(caddy version | cut -d' ' -f1), sites: $DOMAIN"
