#!/usr/bin/env bash
# Ejecuta un comando lftp dentro de Alpine (ZimaOS no permite instalar paquetes en el host).
# Uso: ./lftp.sh "<comandos lftp separados por ;>"
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/config.sh"
CMDS="${1:?uso: lftp.sh \"comandos\"}"
exec docker run --rm --network host \
  -v "$LOCAL_DIR:/data:ro" \
  alpine sh -c "apk add --no-cache -q lftp >/dev/null 2>&1 && lftp -p $SWITCH_PORT -u '$FTP_USER','$FTP_PASS' -e 'set ftp:passive-mode on; set net:timeout 15; set net:max-retries 3; $CMDS; bye' '$SWITCH_IP'"
