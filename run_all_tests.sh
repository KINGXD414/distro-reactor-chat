#!/usr/bin/env bash
set -euo pipefail

# 官方工具压测入口：Apache JMeter TCP Sampler
# 用法: ./run_all_tests.sh [threads] [loops] [port]
THREADS=${1:-50}
LOOPS=${2:-1}
PORT=${3:-6000}
RUN_ID=$(date +%Y%m%d-%H%M%S)
mkdir -p results
jmeter -n -t perf/jmeter/chat_login.jmx \
  -Jthreads="$THREADS" -Jloops="$LOOPS" -Jport="$PORT" \
  -Jtcp.binarylength.prefix.length=4 \
  -l "results/login-${RUN_ID}.jtl" -e -o "results/login-report-${RUN_ID}"
