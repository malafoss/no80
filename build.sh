#!/bin/sh
builder=`which podman || which docker`
$builder build . -t no80 --label org.opencontainers.image.version=`cat VERSION`
