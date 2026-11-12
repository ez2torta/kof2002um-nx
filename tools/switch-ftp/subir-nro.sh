#!/usr/bin/env bash
# Actualiza solo el NRO y el toml (para probar builds nuevos sin volver a subir el pack).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/config.sh"
[ -f "$LOCAL_DIR/kof2002um-nx.nro" ] || { echo "No está $LOCAL_DIR/kof2002um-nx.nro"; exit 1; }
"$HERE/lftp.sh" "mkdir -p $REMOTE_DIR; put -O $REMOTE_DIR /data/kof2002um-nx.nro; put -O $REMOTE_DIR /data/kof2002um_nx.toml"
echo "NRO y toml actualizados."
