#!/usr/bin/env bash
# Descarga la carpeta de logs de la Switch (incluye subcarpetas) a una carpeta local.
# Uso: ./bajar-logs.sh [carpeta_destino]   (por defecto: tools/switch-ftp/logs)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/config.sh"
DEST="${1:-$HERE/logs}"
REMOTE_LOGS="${REMOTE_LOGS:-$REMOTE_DIR/logs}"
mkdir -p "$DEST"
DEST="$(cd "$DEST" && pwd)"
echo "Bajando $SWITCH_IP:$REMOTE_LOGS -> $DEST"
# Montamos el destino como /out (lectura/escritura) y usamos mirror sin -R (descarga).
docker run --rm --network host \
  -v "$DEST:/out" \
  alpine sh -c "apk add --no-cache -q lftp >/dev/null 2>&1 && lftp -p $SWITCH_PORT -u '$FTP_USER','$FTP_PASS' -e 'set ftp:passive-mode on; set net:timeout 15; set net:max-retries 3; mirror --verbose --parallel=2 $REMOTE_LOGS /out; bye' '$SWITCH_IP'"
echo "Listo. Archivos:"
(cd "$DEST" && ls -lR)
