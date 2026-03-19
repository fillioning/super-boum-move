#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

if [ ! -d "dist/superboom" ]; then
    echo "Error: dist/superboom not found. Run ./scripts/build.sh first."
    exit 1
fi

ssh root@move.local "mkdir -p /data/UserData/move-anything/modules/audio_fx/superboom"
scp -r dist/superboom/* root@move.local:/data/UserData/move-anything/modules/audio_fx/superboom/
ssh root@move.local "chmod -R a+rw /data/UserData/move-anything/modules/audio_fx/superboom"

echo "Installed to Move. Restart MoveOriginal to load the module."
