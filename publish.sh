#!/usr/bin/env bash
# Saves the work to GitHub and updates the production site:  ./publish.sh "what changed"
set -euo pipefail
cd "$(dirname "$0")"
./build.sh
git add -A
git commit -m "${1:-Update}" || echo "(nothing new to commit)"
git push
deploy/deploy.sh
