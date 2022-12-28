FROM docker.io/gcc:12 AS compile
WORKDIR /no80-src
COPY Makefile no80.c VERSION ./
RUN make

FROM scratch AS build
LABEL org.opencontainers.image.title="no80 - The resource effective redirecting http server" \
      org.opencontainers.image.url="https://github.com/malafoss/no80" \
      org.opencontainers.image.licenses="MIT"
COPY --from=compile /no80-src/no80 /
EXPOSE 80
ENTRYPOINT [ "/no80" ]
