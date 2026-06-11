# Buildroot build environment — Buildroot requires a Linux host, so on
# macOS the image build runs inside this container.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        bc bzip2 ca-certificates cpio file g++ gcc git libncurses-dev \
        libssl-dev make patch perl python3 rsync unzip wget xz-utils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
