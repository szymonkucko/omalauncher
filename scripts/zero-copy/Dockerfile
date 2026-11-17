# Build environment for oma-zc-client (aarch64, glibc 2.35 baseline: runs on Arch Linux ARM's newer glibc).
FROM --platform=linux/arm64 ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc libc6-dev libvulkan-dev glslang-tools file \
    libwayland-dev libwayland-bin wayland-protocols \
 && rm -rf /var/lib/apt/lists/*
