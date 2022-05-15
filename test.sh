#!/bin/bash
builder=`which podman || which docker`

echo Start no80 container
container=`$builder run -d -p 9898:80 no80 redirect https://example.com`
echo Test redirect to example.com
curl -vL http://localhost:9898 2>&1 | grep "Connected to example.com" || echo TEST FAILED
echo Stop container
$builder stop $container
echo Remove container
$builder rm $container
