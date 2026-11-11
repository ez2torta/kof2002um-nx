# Entorno de build de kof2002um-nx: devkitA64 + libnx (Switch) + clang-20 + cmake/ninja (codegen de ReXGlue en el host).
# No contiene datos del juego ni el SDK de Mesa/NVK: se montan como volúmenes (ver docker-compose.yml).
FROM devkitpro/devkita64:latest AS toolchain

ENV DEBIAN_FRONTEND=noninteractive \
    DEVKITPRO=/opt/devkitpro \
    DEVKITARM=/opt/devkitpro/devkitARM \
    DEVKITA64=/opt/devkitpro/devkitA64 \
    PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH

# Herramientas del host: clang-20 (preset linux-base del SDK), cmake >= 3.25, ninja, python, git.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl wget gnupg lsb-release software-properties-common \
        git python3 python3-pip pkg-config make ninja-build cmake xz-utils unzip \
        libx11-dev libxcb1-dev libwayland-dev libvulkan-dev libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh \
    && bash /tmp/llvm.sh 20 \
    && apt-get install -y --no-install-recommends clang-20 lld-20 libc++-20-dev libc++abi-20-dev \
    && ln -sf /usr/bin/clang-20 /usr/local/bin/clang && ln -sf /usr/bin/clang++-20 /usr/local/bin/clang++ \
    && rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

# Dependencias de desarrollo del host para compilar el codegen (UI/SDL3 del SDK; x11-xcb y wayland-client son obligatorias).
RUN apt-get update && apt-get install -y --no-install-recommends \
        libx11-xcb-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxfixes-dev libxss-dev libxkbcommon-dev \
        libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libgtk-3-dev \
    && rm -rf /var/lib/apt/lists/*

# La imagen base ya trae libnx y los portlibs de Switch (zlib, zstd, expat); solo aseguramos switch-dev.
RUN dkp-pacman -S --noconfirm --needed switch-dev && dkp-pacman -Scc --noconfirm || true

COPY docker/build.sh /usr/local/bin/kof-build
RUN chmod +x /usr/local/bin/kof-build
WORKDIR /work
ENTRYPOINT ["/usr/local/bin/kof-build"]
CMD ["all"]

# --- Etapa del codegen ---------------------------------------------------------------------------------------------
# El SDK usa std::chrono::clock_time_conversion (libstdc++ de GCC 13+); devkitpro/devkita64 es Debian 12 (GCC 12), así que
# el rexglue del host (codegen) se compila y corre en Debian 13, con el clang de Debian 13 (19).
FROM debian:trixie-slim AS codegen
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git python3 pkg-config make ninja-build cmake xz-utils unzip libstdc++-14-dev clang lld \
        libx11-dev libx11-xcb-dev libxcb1-dev libwayland-dev libvulkan-dev libssl-dev zlib1g-dev \
        libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxfixes-dev libxss-dev libxkbcommon-dev \
        libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libgtk-3-dev \
    && rm -rf /var/lib/apt/lists/*
COPY docker/build.sh /usr/local/bin/kof-build
RUN chmod +x /usr/local/bin/kof-build
WORKDIR /work
ENTRYPOINT ["/usr/local/bin/kof-build"]
CMD ["codegen"]
