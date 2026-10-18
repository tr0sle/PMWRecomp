#!/usr/bin/env bash
# Publishes the current build (dist/) and the room server to the production server.
#   deploy/deploy.sh            upload + restart
#   deploy/deploy.sh --setup    first time: also prepares the server (Node, Caddy, HTTPS, service)
# Settings come from deploy/config.env (copy config.example.env).
set -euo pipefail
cd "$(dirname "$0")/.."
source deploy/config.env
KEY=${SSH_KEY:-~/.ssh/pmwr_deploy}
SSH="ssh -i $KEY -o BatchMode=yes -o StrictHostKeyChecking=accept-new $SSH_USER@$SERVER"

[ -f dist/pmr.wasm ] || { echo "dist/pmr.wasm missing: run ./build.sh first"; exit 1; }

if [ "${1:-}" = "--setup" ]; then
  $SSH "bash -s $DOMAIN" < deploy/setup-server.sh
fi

echo "uploading build ($(du -sh dist --exclude=_test | cut -f1)) ..."
tar czf - --exclude=dist/_test dist server | $SSH "rm -rf /opt/pmwr/new && mkdir -p /opt/pmwr/new && tar xzf - -C /opt/pmwr/new \
  && rm -rf /opt/pmwr/dist /opt/pmwr/server && mv /opt/pmwr/new/dist /opt/pmwr/new/server /opt/pmwr/ && rm -rf /opt/pmwr/new \
  && chown -R pmwr:pmwr /opt/pmwr && systemctl restart pmwr && sleep 1 && systemctl is-active pmwr"
code=$(curl -s -o /dev/null -w '%{http_code}' "https://$DOMAIN/" || true)
echo "https://$DOMAIN -> HTTP $code"
