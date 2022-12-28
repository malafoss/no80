#!/bin/sh
testport=9898

echo Building no80 image
. ./build.sh

echo Start no80 container
$builder stop -i no80test
$builder rm -i no80test
$builder run --name no80test -d -p $testport:80 no80 https://nonexistingtest.site

echo Test1: redirect to https://nonexistingtest.site
curl -vL http://localhost:$testport 2>&1 | grep "URL: 'https://nonexistingtest.site/'" && echo TEST SUCCESS || echo TEST FAILED

echo Stop no80 container
$builder stop -i no80test
$builder rm -i no80test

$builder run --name no80test -d -p $testport:80 no80 -a https://nonexistingtest.site

echo Test2: redirect to https://nonexistingtest.site/hello
curl -vL http://localhost:$testport/hello 2>&1 | grep "URL: 'https://nonexistingtest.site/hello'" && echo TEST SUCCESS || echo TEST FAILED

echo Stop no80 container
$builder stop -i no80test
$builder rm -i no80test

echo Test3: no redirect to https://nonexistingtest.site
curl -vL http://localhost:$testport 2>&1 | grep "URL: 'https://nonexistingtest.site" && echo TEST FAILED || echo TEST SUCCESS

echo Start no80 container
$builder stop -i no80test
$builder rm -i no80test
$builder run --name no80test -d -p $testport:80 no80 -m /match https://nonexistingtest.site/m -s /starting https://nonexistingtest.site/s -r /redirect https://nonexistingtest.site/r https://nonexistingtest.site

echo Test4: redirect no match to https://nonexistingtest.site
curl -vL http://localhost:$testport 2>&1 | grep "URL: 'https://nonexistingtest.site/'" && echo TEST SUCCESS || echo TEST FAILED

echo Test5: redirect /match to https://nonexistingtest.site/m
curl -vL http://localhost:$testport/match 2>&1 | grep "URL: 'https://nonexistingtest.site/m'" && echo TEST SUCCESS || echo TEST FAILED

echo Test6: redirect /starting/path to https://nonexistingtest.site/s
curl -vL http://localhost:$testport/starting/path 2>&1 | grep "URL: 'https://nonexistingtest.site/s'" && echo TEST SUCCESS || echo TEST FAILED

echo Test7: redirect /redirect/path to https://nonexistingtest.site/r/path
curl -vL http://localhost:$testport/redirect/path 2>&1 | grep "URL: 'https://nonexistingtest.site/r/path'" && echo TEST SUCCESS || echo TEST FAILED

echo Stop no80 container
$builder stop -i no80test
$builder rm -i no80test

echo Tests complete
