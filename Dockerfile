FROM docker.io/gcc:15-trixie AS compile
WORKDIR /no80-src
# Install dependencies for building WolfSSL
RUN apt-get update && apt-get install -y \
    git \
    autoconf \
    automake \
    libtool \
    openssl \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Build WolfSSL from source with static library
RUN git clone --depth 1 https://github.com/wolfSSL/wolfssl.git \
    && cd wolfssl \
    && ./autogen.sh \
    && ./configure --enable-static --disable-shared --enable-tls13 \
        --enable-asm --enable-sp-asm --enable-intelasm --enable-aesni \
        --disable-examples --disable-crypttests \
    && make \
    && make install \
    && cd .. \
    && rm -rf wolfssl

# Generate self-signed certificates
RUN mkdir -p /certs \
    && openssl req -x509 -newkey rsa:2048 -keyout /certs/key.pem -out /certs/cert.pem -days 3650 -nodes -subj "/CN=localhost"

COPY Makefile no80.c VERSION ./
RUN make

FROM scratch AS build
LABEL org.opencontainers.image.title="no80 - The resource effective redirecting http/https server" \
      org.opencontainers.image.url="https://github.com/malafoss/no80" \
      org.opencontainers.image.licenses="MIT"
COPY --from=compile /no80-src/no80 /
COPY --from=compile /certs /certs
EXPOSE 80 443
ENTRYPOINT [ "/no80" ]
