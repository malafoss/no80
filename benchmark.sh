#!/bin/bash
requests=100
threads=200

if [ $# -eq 0 ]; then
  echo "benchmark <portno>"
  exit 0
fi

if [ "$1" = "thread" ]; then
  (for i in `seq $requests`; do echo -e "GET /asdfas/asdfas HTTP/1.1\nHost: <server>:9999\n\n" | nc -v 127.0.0.1 $2; done) 2>&1 > /dev/null
  exit 0
fi

if [ "$1" = "threads" ]; then
  set -m
  echo Run $threads threads $requests each
  thread_pids=
  for i in `seq $threads`
  do
    coproc ./benchmark.sh thread $2
    thread_pids+=" $COPROC_PID"
  done

  echo Wait threads
  echo thread_pids=$thread_pids
  for i in $thread_pids
  do
    echo wait $i
    wait $i
  done
  exit 0
fi

testport=$1

echo Run benchmark for no80 at port $testport

time ./benchmark.sh threads $testport
echo for $requests requests in $threads threads `expr $requests \* $threads` requests in total
echo ""

echo Benchmark complete
