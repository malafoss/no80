#!/bin/bash
testport=9898

# Helper functions for test.sh

# Start container with specified options
start_container() {
    local options="$1"
    shift  # Remove the first argument (options)
    echo "Starting no80 container with: $options"
    $builder stop -i no80test 2>/dev/null || true
    $builder rm -i no80test 2>/dev/null || true
    $builder run --name no80test -d --memory=32m $options no80 "$@"
    
    # Wait for server to be ready by testing actual HTTP response
    local port=$(echo "$options" | grep -o '\-p [0-9]*:' | head -1 | cut -d' ' -f2 | cut -d':' -f1)
    if [ -n "$port" ]; then
        echo "Waiting for server to be ready on port $port..."
        local max_attempts=50
        local attempt=0
        while [ $attempt -lt $max_attempts ]; do
            if curl -s --max-time 1 http://localhost:"$port" >/dev/null 2>&1; then
                echo "Server ready after $attempt attempts"
                return 0
            fi
            sleep 0.1
            attempt=$((attempt + 1))
        done
        echo "Warning: Server may not be ready after $max_attempts attempts"
    else
        sleep 1  # Fallback for containers without port mapping
    fi
}

# Stop and remove container
stop_container() {
    echo "Stop no80 container"
    $builder stop -i no80test 2>/dev/null || true
    $builder rm -i no80test 2>/dev/null || true
}

# Test HTTP redirect and report result
test_redirect() {
    local test_name="$1"
    local url="$2" 
    local expected="$3"
    echo "$test_name"
    local response=$(curl -v "$url" 2>&1)
    if echo "$response" | grep -q "$expected"; then
        echo TEST SUCCESS
    else
        echo TEST FAILED
        echo "Expected: $expected"
        echo "Response: $response"
    fi
}

# Test HTTPS redirect and report result  
test_https_redirect() {
    local test_name="$1"
    local url="$2"
    local expected="$3"
    echo "$test_name"
    local response=$(curl -v -k "$url" 2>&1)
    if echo "$response" | grep -q "$expected"; then
        echo TEST SUCCESS
    else
        echo TEST FAILED
        echo "Expected: $expected"
        echo "Response: $response"
    fi
}

# Run performance benchmark with multithreaded coproc approach
run_benchmark() {
    local test_name="$1"
    local threads="$2"
    local requests_per_thread="$3"
    local port="$4"
    local protocol="$5"
    local path="$6"
    local description="$7"
    
    local total_requests=$((threads * requests_per_thread))
    
    echo "$test_name"
    echo "Testing $description with $threads threads × $requests_per_thread requests = $total_requests total requests..."
    echo "CPU usage breakdown:"
    
    # Function to run threaded benchmark using coproc like benchmark.sh
    run_threaded_benchmark() {
        set -m  # Enable job control
        local thread_pids=""
        local coproc_fds=""
        
        # Start worker threads using coproc
        for i in $(seq 1 $threads); do
            if [ "$protocol" = "https" ]; then
                coproc bash -c "for j in \$(seq 1 $requests_per_thread); do curl -k -s https://localhost:$port$path > /dev/null 2>&1; done"
            else
                coproc bash -c "for j in \$(seq 1 $requests_per_thread); do echo -e 'GET $path HTTP/1.1\\nHost: localhost:$port\\n\\n' | nc -w 1 localhost $port > /dev/null 2>&1; done"
            fi
            thread_pids="$thread_pids $COPROC_PID"
            coproc_fds="$coproc_fds $COPROC"
            
            # Close file descriptors to avoid warnings
            exec {COPROC[0]}<&-
            exec {COPROC[1]}>&-
        done
        
        # Wait for all threads to complete
        for pid in $thread_pids; do
            wait $pid 2>/dev/null
        done
    }
    
    # Run with CPU monitoring and timing using dedicated benchmark.sh
    start_time=$(date +%s%N)
    temp_output=$(mktemp)
    /usr/bin/time -v ./benchmark.sh $threads $requests_per_thread $port $protocol $path 2>"$temp_output"
    end_time=$(date +%s%N)
    
    # Display CPU metrics
    echo "Script CPU time:"
    grep -E "(User time|System time)" "$temp_output"
    echo "Total resource usage:"
    grep -E "(Percent of CPU|Maximum resident set size)" "$temp_output"
    rm -f "$temp_output"
    
    # Calculate performance metrics
    duration_ns=$((end_time - start_time))
    duration_s=$((duration_ns / 1000000000))
    duration_ms=$((duration_ns / 1000000))
    
    if [ "$duration_ms" -lt 1000 ]; then
        rps=$((total_requests * 1000 / duration_ms))
        echo "$test_name completed in ${duration_ms}ms (~${rps} redirects/second)"
    else
        duration_display=$(echo "scale=3; $duration_ms / 1000" | bc -l)
        rps=$(echo "scale=0; $total_requests * 1000 / $duration_ms" | bc -l)
        echo "$test_name completed in ${duration_display}s (~${rps} redirects/second)"
    fi
    
    # Check if benchmark completed within 60 seconds
    if [ "$duration_s" -lt "60" ]; then
        echo "$test_name: TEST SUCCESS"
    else
        echo "$test_name: TEST FAILED (took too long: ${duration_s}s)"
    fi
}

echo Building no80 image
. ./build.sh

start_container "-p $testport:80" https://nonexistingtest.site
test_redirect "Test1: redirect to https://nonexistingtest.site" "http://localhost:$testport" "Location: https://nonexistingtest.site"
stop_container

start_container "-p $testport:80" -a https://nonexistingtest.site
test_redirect "Test2: redirect to https://nonexistingtest.site/hello" "http://localhost:$testport/hello" "Location: https://nonexistingtest.site/hello"
stop_container

echo Test3: no redirect to https://nonexistingtest.site
curl -vL http://localhost:$testport 2>&1 | grep "URL: 'https://nonexistingtest.site" && echo TEST FAILED || echo TEST SUCCESS

start_container "-p $testport:80" -m /match https://nonexistingtest.site/m -s /starting https://nonexistingtest.site/s -r /redirect https://nonexistingtest.site/r https://nonexistingtest.site
test_redirect "Test4: redirect no match to https://nonexistingtest.site" "http://localhost:$testport" "Location: https://nonexistingtest.site"
test_redirect "Test5: redirect /match to https://nonexistingtest.site/m" "http://localhost:$testport/match" "Location: https://nonexistingtest.site/m"
test_redirect "Test6: redirect /starting/path to https://nonexistingtest.site/s" "http://localhost:$testport/starting/path" "Location: https://nonexistingtest.site/s"
test_redirect "Test7: redirect /redirect/path to https://nonexistingtest.site/r/path" "http://localhost:$testport/redirect/path" "Location: https://nonexistingtest.site/r/path"
stop_container

# Test8: HTTPS functionality
start_container "-p $testport:80 -p $((testport+1)):443" https://nonexistingtest.site
test_https_redirect "Test8: HTTPS server functionality" "https://localhost:$((testport+1))" "Location: https://nonexistingtest.site"
stop_container

# Test9: Simultaneous HTTP and HTTPS functionality
start_container "-p $testport:80 -p $((testport+1)):443" -a https://nonexistingtest.site
echo Test9: Simultaneous HTTP and HTTPS servers
# Test HTTP redirect
if curl -vL http://localhost:$testport/testpath 2>&1 | grep -q "URL: 'https://nonexistingtest.site/testpath'"; then
    http_success=1
else
    http_success=0
fi
# Test HTTPS redirect  
if curl -vL -k https://localhost:$((testport+1))/testpath 2>&1 | grep -q "URL: 'https://nonexistingtest.site/testpath'"; then
    https_success=1
else
    https_success=0
fi
if [ "$http_success" = "1" ] && [ "$https_success" = "1" ]; then
    echo TEST SUCCESS
else
    echo TEST FAILED - HTTP: $http_success, HTTPS: $https_success
fi
stop_container

# Test10: Permanent redirect (301) vs temporary (302)
start_container "-p $testport:80" -P https://nonexistingtest.site
test_redirect "Test10: Permanent redirect with -P option" "http://localhost:$testport" "301 Moved Permanently"
stop_container

# Test11: Custom port configuration
customport=9999
start_container "-p $customport:$customport" -p $customport https://nonexistingtest.site
test_redirect "Test11: Custom HTTP port with -p option" "http://localhost:$customport" "Location: https://nonexistingtest.site"
stop_container

# Test12: Help and version commands
echo Test12: Help and version options
$builder run --rm no80 -h 2>&1 | grep -q "Usage: no80" && echo "Help: TEST SUCCESS" || echo "Help: TEST FAILED"
$builder run --rm no80 -v 2>&1 | grep -q "no80 - The resource effective redirecting http server" && echo "Version: TEST SUCCESS" || echo "Version: TEST FAILED"

# Test13: Error handling for invalid parameters
echo Test13: Error handling for invalid port
$builder run --rm no80 -p 99999 https://example.com 2>&1 | grep -q "Invalid port number" && echo TEST SUCCESS || echo TEST FAILED

# Test14: HTTP performance benchmark
start_container "-p $testport:80" https://nonexistingtest.site
run_benchmark "Test14: HTTP performance benchmark" 100 100 $testport "http" "/test" "HTTP performance"
stop_container

# Test15: HTTPS performance benchmark  
start_container "-p $testport:80 -p $((testport+1)):443" https://nonexistingtest.site
run_benchmark "Test15: HTTPS performance benchmark" 50 100 $((testport+1)) "https" "/test" "HTTPS performance"
stop_container

# Test16: Combined HTTP/HTTPS load test
start_container "-p $testport:80 -p $((testport+1)):443" -a https://nonexistingtest.site

# Combined test: 70 HTTP threads + 30 HTTPS threads, each doing 100 requests
echo "Test16: Combined HTTP/HTTPS concurrent load test"
echo "Testing concurrent HTTP and HTTPS load with 70×100 HTTP + 30×100 HTTPS = 10000 total requests..."

# Start HTTP benchmark in background
run_benchmark "Test16a: HTTP part" 70 100 $testport "http" "/load" "HTTP part of combined load" &
http_pid=$!

# Start HTTPS benchmark in background  
run_benchmark "Test16b: HTTPS part" 30 100 $((testport+1)) "https" "/load" "HTTPS part of combined load" &
https_pid=$!

# Wait for both to complete
wait $http_pid
wait $https_pid

echo "Test16: Combined HTTP/HTTPS concurrent load test: TEST SUCCESS"
stop_container

echo Tests complete
