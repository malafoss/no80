#!/bin/sh
testport=9898

echo Building no80 image
. ./build.sh

echo Start no80 container
container=`$builder run -d -p $testport:80 no80 redirect https://example.com`
echo $container

echo Test1: redirect to example.com
curl -vL http://localhost:$testport 2>&1 | grep "Connected to example.com" && echo TEST SUCCESS || echo TEST FAILED

echo Stop no80 container
$builder stop $container

echo Remove no80 container
$builder rm $container

echo Test2: no redirect to example.com
curl -vL http://localhost:$testport 2>&1 | grep "Connected to example.com" && echo TEST FAILED || echo TEST SUCCESS

echo Tests complete
