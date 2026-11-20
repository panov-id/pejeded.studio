# Build and test image. Same Debian as the dev machine (section 4).
FROM debian:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git ca-certificates \
    libasound2-dev libjack-jackd2-dev pipewire-jack \
    libfreetype-dev libfontconfig1-dev libcurl4-openssl-dev \
    libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libxcomposite-dev libxrender-dev \
    libgl1-mesa-dev libglu1-mesa-dev mesa-utils xvfb xauth \
    ffmpeg \
 && rm -rf /var/lib/apt/lists/*
ARG UID=1000
ARG GID=1000
RUN groupadd -g ${GID} dev || true; useradd -m -u ${UID} -g ${GID} dev || true
USER ${UID}:${GID}
WORKDIR /src
