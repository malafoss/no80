#!/bin/sh
builder=`which podman || which docker`
$builder build . -t no80
