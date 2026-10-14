#!/usr/bin/env bash
set -euo pipefail

# Usage: ./run_tsung.sh [target_host] [target_port] [connections] [arrival_per_second] [hold_seconds]
TARGET_HOST=${1:-127.0.0.1}
TARGET_PORT=${2:-6000}
MAX_USERS=${3:-1000}
ARRIVAL_RATE=${4:-20}
HOLD_SECONDS=${5:-300}

if ! command -v tsung >/dev/null 2>&1; then
  echo "Tsung is required: sudo apt install tsung" >&2
  exit 1
fi
if ! command -v envsubst >/dev/null 2>&1; then
  echo "envsubst is required: sudo apt install gettext-base" >&2
  exit 1
fi

RAMP_SECONDS=$(( (MAX_USERS + ARRIVAL_RATE - 1) / ARRIVAL_RATE + 5 ))
LOAD_MINUTES=$(( (RAMP_SECONDS + HOLD_SECONDS + 59) / 60 + 1 ))
export TARGET_HOST TARGET_PORT MAX_USERS ARRIVAL_RATE HOLD_SECONDS RAMP_SECONDS LOAD_MINUTES
CLIENT_MAX_USERS=$((MAX_USERS + 100))
export CLIENT_MAX_USERS

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
RUN_ID=$(date +%Y%m%d-%H%M%S)
RUN_DIR="$ROOT_DIR/results/$RUN_ID"
mkdir -p "$RUN_DIR"
mkdir -p "$RUN_DIR/logs"
envsubst < "$ROOT_DIR/tcp_connections.xml.in" > "$RUN_DIR/tsung.xml"

ulimit -n 65535 || true
echo "Starting Tsung TCP connection test: target=$TARGET_HOST:$TARGET_PORT users=$MAX_USERS arrival=${ARRIVAL_RATE}/s hold=${HOLD_SECONDS}s"
echo "Raise the ChatServer process open-file limit separately before starting it."
# Tsung 1.7.0 on Ubuntu rejects some nested custom -l paths; keep the
# generated configuration under RUN_DIR and let Tsung use ~/.tsung/log.
tsung -n -f "$RUN_DIR/tsung.xml" start
