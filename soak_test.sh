#!/usr/bin/env bash
set -euo pipefail

# 官方工具浸泡入口：Apache JMeter + 系统观测工具
# 用法: ./soak_test.sh [duration_seconds] [threads] [port]
DURATION=${1:-300}
THREADS=${2:-50}
PORT=${3:-6000}
RUN_ID=$(date +%Y%m%d-%H%M%S)
mkdir -p results
jmeter -n -t perf/jmeter/chat_login.jmx \
  -Jthreads="$THREADS" -Jloops=-1 -Jscheduler=true -Jduration="$DURATION" \
  -Jport="$PORT" -Jtcp.binarylength.prefix.length=4 \
  -l "results/soak-${RUN_ID}.jtl"
