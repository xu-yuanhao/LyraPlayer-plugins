#!/usr/bin/env bash
# Uses the caller's Git credential helpers. No credentials are stored here.
set -euo pipefail
cd "$(dirname "$0")/.."
git push origin main
git push origin --tags
git push gitlab main
git push gitlab --tags
