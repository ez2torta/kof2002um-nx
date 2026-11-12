#!/usr/bin/env bash
# Muestra qué hay en la carpeta de la Switch (y tamaños) para comprobar la subida.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/config.sh"
"$HERE/lftp.sh" "ls -l $REMOTE_DIR"
