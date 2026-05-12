#!/bin/bash
# Install Organelle module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/organelle" ]; then
    echo "Error: dist/organelle not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing Organelle Module ==="

echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/sound_generators/organelle"
scp -r dist/organelle/* ableton@move.local:/data/UserData/schwung/modules/sound_generators/organelle/

# Seed patch store only if empty (preserves user patches across re-installs)
echo "Seeding patch store..."
ssh ableton@move.local "bash -c '
    mkdir -p /data/UserData/schwung/organelle-patches
    if [ -z \"\$(ls -A /data/UserData/schwung/organelle-patches 2>/dev/null)\" ]; then
        if [ -d /data/UserData/schwung/modules/sound_generators/organelle/patches ]; then
            cp -r /data/UserData/schwung/modules/sound_generators/organelle/patches/* \
                  /data/UserData/schwung/organelle-patches/
            echo \"  Seeded starter patch set.\"
        fi
    else
        echo \"  Patch store already populated; leaving user patches alone.\"
    fi
'"

echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/schwung/modules/sound_generators/organelle"
ssh ableton@move.local "chmod -R a+rw /data/UserData/schwung/organelle-patches"

echo ""
echo "=== Install Complete ==="
echo "Module: /data/UserData/schwung/modules/sound_generators/organelle/"
echo "Patches: /data/UserData/schwung/organelle-patches/"
echo ""
echo "Restart Schwung to load the new module."
