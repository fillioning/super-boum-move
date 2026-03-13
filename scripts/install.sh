#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

if [ ! -d "dist/superboum" ]; then
    echo "Error: dist/superboum not found. Run ./scripts/build.sh first."
    exit 1
fi

ssh root@move.local "mkdir -p /data/UserData/move-anything/modules/audio_fx/superboum"
scp -r dist/superboum/* root@move.local:/data/UserData/move-anything/modules/audio_fx/superboum/
ssh root@move.local "chmod -R a+rw /data/UserData/move-anything/modules/audio_fx/superboum"

echo "Installed to Move. Restart MoveOriginal to load the module."
