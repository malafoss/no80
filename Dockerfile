FROM docker.io/gcc:12 AS compile
COPY Makefile no80.c /
RUN make

FROM scratch AS build
COPY --from=compile no80 /
EXPOSE 80
ENTRYPOINT [ "/no80" ]
