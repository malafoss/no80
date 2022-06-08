FROM docker.io/gcc:12 AS compile
WORKDIR /no80-src
COPY Makefile no80.c VERSION ./
RUN make

FROM scratch AS build
COPY --from=compile /no80-src/no80 /
EXPOSE 80
ENTRYPOINT [ "/no80" ]
