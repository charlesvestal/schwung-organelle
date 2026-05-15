#!/usr/bin/env bash
# Build Organelle module for Schwung (ARM64)
#
# Uses Docker for cross-compilation by default.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-organelle-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Organelle Module Build (via Docker) ==="
    echo ""

    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building Organelle Module ==="
echo "Cross prefix: $CROSS_PREFIX"

mkdir -p build
mkdir -p dist/organelle/patches

# --- Build libpd (multi-instance, no GUI, with extras) ---
LIBPD_DIR="libs/libpd"
if [ ! -d "$LIBPD_DIR" ]; then
    echo "Error: $LIBPD_DIR not found. Run: git submodule update --init --recursive"
    exit 1
fi

echo "Building libpd..."
LIBPD_BUILD="build/libpd"
mkdir -p "$LIBPD_BUILD"
if [ ! -f "$LIBPD_BUILD/libpd-multi.a" ]; then
    (
        cd "$LIBPD_BUILD"
        cmake "../../$LIBPD_DIR" \
            -DCMAKE_C_COMPILER=${CROSS_PREFIX}gcc \
            -DCMAKE_CXX_COMPILER=${CROSS_PREFIX}g++ \
            -DCMAKE_SYSTEM_NAME=Linux \
            -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
            -DCMAKE_C_FLAGS="-O3 -fPIC -march=armv8-a -mtune=cortex-a72 -DPD" \
            -DCMAKE_CXX_FLAGS="-O3 -fPIC -march=armv8-a -mtune=cortex-a72" \
            -DPD_MULTI=ON \
            -DPD_UTILS=OFF \
            -DPD_EXTRA=ON \
            -DPD_BUILD_C_EXAMPLES=OFF \
            -DPD_BUILD_C_TESTS=OFF
        make -j$(nproc)
    )
fi

# --- Compile DSP plugin ---
echo "Compiling DSP plugin..."
${CROSS_PREFIX}g++ -O3 -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -std=c++17 \
    -DNDEBUG -DPD_MULTI=1 -DPDINSTANCE=1 -DPDTHREADS=1 \
    src/dsp/pd_host.cpp \
    src/dsp/screen_translator.cpp \
    src/dsp/patch_loader.cpp \
    src/dsp/sandbox.cpp \
    -Isrc/dsp \
    -I$LIBPD_DIR/libpd_wrapper \
    -I$LIBPD_DIR/pure-data/src \
    -Wl,--wrap=open -Wl,--wrap=open64 -Wl,--wrap=fopen -Wl,--wrap=fopen64 \
    "$LIBPD_BUILD/libpd-multi.a" \
    -o build/dsp.so \
    -lm -lpthread

# --- Package ---
echo "Packaging..."
cat src/module/module.json > dist/organelle/module.json
cat src/module/ui_chain.js > dist/organelle/ui_chain.js
cat src/module/font_organelle.mjs > dist/organelle/font_organelle.mjs
cat src/dsp/mother.pd > dist/organelle/mother.pd
cat build/dsp.so > dist/organelle/dsp.so
[ -f src/module/help.json ] && cat src/module/help.json > dist/organelle/help.json
chmod +x dist/organelle/dsp.so

# Copy bundled patches
if [ -d patches ]; then
    cp -r patches/* dist/organelle/patches/ 2>/dev/null || true
fi

# Tarball for release
cd dist
tar -czf organelle-module.tar.gz organelle/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output:  dist/organelle/"
echo "Tarball: dist/organelle-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
