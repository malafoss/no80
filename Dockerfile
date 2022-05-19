FROM docker.io/gcc:12 AS compile
COPY Makefile no80.c VERSION /
RUN make VERSION=`cat VERSION`

FROM scratch AS build
COPY --from=compile no80 /
EXPOSE 80
ENTRYPOINT [ "/no80" ]
