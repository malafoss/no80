#!/bin/bash
builder=`which podman || which docker`
make
$builder build . -t no80
