#!/bin/bash
# LISA Ultra — install script

if [[ "$OSTYPE" != "darwin"* ]]; then
    echo "❌ This binary is for macOS ARM64 (Apple Silicon) only."
    exit 1
fi

if [[ "$(uname -m)" != "arm64" ]]; then
    echo "❌ This binary requires ARM64 (Apple Silicon)."
    exit 1
fi

cp lisa_mac /usr/local/bin/lisa
chmod +x /usr/local/bin/lisa
echo "✅ LISA Ultra installed successfully!"
echo "   Run: lisa --index vectors.bin --dim 768 --topk 5 --query query.txt"
