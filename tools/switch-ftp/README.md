# Subir el build a la Switch por FTP

Requisitos: Docker (ya lo tienes en ZimaOS) y un servidor FTP anónimo corriendo en la consola en el puerto **5000**.

1. Edita `config.sh` y pon la IP de la Switch en `SWITCH_IP` (o exporta la variable).
2. Primera vez: `./subir-todo.sh` (sube el NRO, el toml y el pack de ~580 MB).
3. Cada build nuevo: `./subir-nro.sh` (solo NRO y toml).
4. Para revisar: `./listar.sh`.

Si tu servidor no es anónimo, exporta `FTP_USER` y `FTP_PASS`.
Carpeta de origen: `LOCAL_DIR` (por defecto `switch-sd/kof2002um-nx`). Destino en la consola: `REMOTE_DIR` (por defecto `/switch/kof2002um-nx`).
