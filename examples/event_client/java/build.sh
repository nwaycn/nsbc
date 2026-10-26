#!/usr/bin/env bash
# Compile NSBC Event Client (Java 8+, pure stdlib)
# Usage: ./build.sh

set -euo pipefail
cd "$(dirname "$0")"

SRC=src
OUT=out

if [ ! -f "$SRC/nsbc/NsbcClient.java" ]; then
    echo "[FAIL] NsbcClient.java not found. Run from examples/event_client/java/"
    exit 1
fi

if [ ! -x "$(command -v javac)" ]; then
    echo "[FAIL] javac not found. Install JDK 8+ (JDK, not JRE)."
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"

echo "[compile] javac ..."
javac -encoding UTF-8 -source 1.8 -target 1.8 -d "$OUT" \
      "$SRC/nsbc/NsbcClient.java" "$SRC/nsbc/EventClient.java"

echo "[OK] compiled to $OUT/nsbc/"
echo "     run: java -cp $OUT nsbc.EventClient --help"
