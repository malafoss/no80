#!/bin/bash

# benchmark.sh - Multithreaded HTTP/HTTPS benchmark script
# Usage: benchmark.sh <threads> <requests_per_thread> <port> <protocol> <path>

if [ $# -ne 5 ]; then
    echo "Usage: $0 <threads> <requests_per_thread> <port> <protocol> <path>"
    exit 1
fi

threads=$1
requests_per_thread=$2
port=$3
protocol=$4
path=$5

set -m  # Enable job control
thread_pids=""

# Start worker threads using coproc
for i in $(seq 1 $threads); do
    if [ "$protocol" = "https" ]; then
        coproc bash -c "for j in \$(seq 1 $requests_per_thread); do curl -k -s https://localhost:$port$path > /dev/null 2>&1; done"
    else
        coproc bash -c "for j in \$(seq 1 $requests_per_thread); do echo -e 'GET $path HTTP/1.1\\nHost: localhost:$port\\n\\n' | nc -w 1 localhost $port > /dev/null 2>&1; done"
    fi
    thread_pids="$thread_pids $COPROC_PID"
    
    # Close file descriptors to avoid warnings
    exec {COPROC[0]}<&-
    exec {COPROC[1]}>&-
done

# Wait for all threads to complete
for pid in $thread_pids; do
    wait $pid 2>/dev/null
done