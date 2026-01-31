# Dockerfile for KTPAmxxCurl - Ubuntu 22.04 build with static libraries
FROM ubuntu:22.04

# Avoid interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y \
    build-essential \
    gcc-multilib \
    g++-multilib \
    make \
    wget \
    pkg-config \
    autoconf \
    automake \
    libtool \
    zlib1g-dev:i386 \
    && rm -rf /var/lib/apt/lists/*

# Build static 32-bit libraries
WORKDIR /tmp/build

# Build zlib (32-bit static)
RUN wget https://zlib.net/fossils/zlib-1.2.13.tar.gz && \
    tar xf zlib-1.2.13.tar.gz && \
    cd zlib-1.2.13 && \
    CFLAGS="-m32 -fPIC" ./configure --static --prefix=/usr/local/lib32 && \
    make -j$(nproc) && make install

# Build openssl (32-bit static)
RUN wget https://www.openssl.org/source/openssl-1.1.1w.tar.gz && \
    tar xf openssl-1.1.1w.tar.gz && \
    cd openssl-1.1.1w && \
    ./Configure linux-generic32 no-shared --prefix=/usr/local/lib32 \
    -m32 -fPIC && \
    make -j$(nproc) && make install_sw

# Build c-ares (32-bit static) - using GitHub releases
RUN wget https://github.com/c-ares/c-ares/releases/download/v1.34.4/c-ares-1.34.4.tar.gz && \
    tar xf c-ares-1.34.4.tar.gz && \
    cd c-ares-1.34.4 && \
    CFLAGS="-m32 -fPIC" ./configure --disable-shared --enable-static \
    --prefix=/usr/local/lib32 --host=i686-linux-gnu && \
    make -j$(nproc) && make install

# Build curl (32-bit static)
RUN wget https://curl.se/download/curl-7.88.1.tar.gz && \
    tar xf curl-7.88.1.tar.gz && \
    cd curl-7.88.1 && \
    CFLAGS="-m32 -fPIC" LDFLAGS="-m32" \
    PKG_CONFIG_PATH=/usr/local/lib32/lib/pkgconfig \
    ./configure --disable-shared --enable-static \
    --prefix=/usr/local/lib32 --host=i686-linux-gnu \
    --with-ssl=/usr/local/lib32 \
    --with-zlib=/usr/local/lib32 \
    --enable-ares=/usr/local/lib32 \
    --disable-ldap --disable-ldaps \
    --without-librtmp --without-libidn2 \
    --without-libpsl --without-nghttp2 && \
    make -j$(nproc) && make install

WORKDIR /build
