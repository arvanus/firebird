FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        libtool-bin \
        autoconf \
        automake \
        libtomcrypt-dev \
        libtommath-dev \
        libicu-dev \
        zlib1g-dev \
        cmake \
        git \
        pkg-config \
        ca-certificates \
        libncurses-dev \
        libedit-dev \
        libatomic1 \
        unzip \
        zip \
        procps \
    && rm -rf /var/lib/apt/lists/*

RUN git config --global --add safe.directory '*'

WORKDIR /work
