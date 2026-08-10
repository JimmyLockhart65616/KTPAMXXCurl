#!/bin/bash
# Set KTP_NO_STAGE=1 to build WITHOUT copying into the local test tree -- staging
# overwrites an artifact whose md5 may be pinned to a reviewed build.
# KTPAmxxCurl Linux Build Script
# Run via WSL: wsl bash -c "cd '/mnt/n/Nein_/KTP Git Projects/KTPAmxxCurl' && bash build_linux.sh"

set -e

echo "========================================"
echo "KTPAmxxCurl Linux Build Script (CMake)"
echo "========================================"

# Check for required tools
for tool in cmake make gcc; do
    if ! command -v $tool &> /dev/null; then
        echo "ERROR: $tool is not installed"
        exit 1
    fi
done

# Clean and build
rm -rf build
mkdir -p build
cd build
cmake .. -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
make -j$(nproc)
cd ..

# Check if build succeeded
BINARY_PATH="build/amxxcurl_ktp_i386.so"
if [ -f "$BINARY_PATH" ]; then
    echo ""
    echo "========================================"
    echo "BUILD SUCCESS!"
    echo "========================================"
    echo "Binary: $BINARY_PATH"
    ls -lh "$BINARY_PATH"

    # Deploy to staging folder
    DEPLOY_DIR="/mnt/n/Nein_/KTP Git Projects/KTP DoD Server/serverfiles"
    if [ -d "$DEPLOY_DIR" ]; then
        echo ""
        if [ -n "${KTP_NO_STAGE:-}" ]; then
            echo "Staging SKIPPED (KTP_NO_STAGE set)."
            echo "  Binary left at: $BINARY_PATH"
        else
            echo "Deploying to staging folder..."
            mkdir -p "$DEPLOY_DIR/dod/addons/ktpamx/modules"
            cp "$BINARY_PATH" "$DEPLOY_DIR/dod/addons/ktpamx/modules/"
            echo "  -> Copied amxxcurl_ktp_i386.so"
            echo ""
            echo "Files staged at: $DEPLOY_DIR/dod/addons/ktpamx/modules/"
        fi
    fi
else
    echo ""
    echo "BUILD FAILED!"
    exit 1
fi
