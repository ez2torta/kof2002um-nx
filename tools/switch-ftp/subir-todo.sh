#!/usr/bin/env bash
# Sube la carpeta completa (NRO, toml, game.nxpack, game.nxidx). Tarda: el pack pesa ~580 MB.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/config.sh"
[ -f "$LOCAL_DIR/game.nxpack" ] || { echo "No está $LOCAL_DIR/game.nxpack"; exit 1; }
echo "Subiendo $LOCAL_DIR -> $SWITCH_IP:$REMOTE_DIR (puerto $SWITCH_PORT)"
"$HERE/lftp.sh" "mkdir -p $REMOTE_DIR; mirror -R --verbose --parallel=2 /data $REMOTE_DIR"
echo "Listo."
