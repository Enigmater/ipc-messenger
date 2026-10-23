#!/bin/sh
# Run as root after creating the ipcm group and adding messenger users to it.
set -eu
if [ "$(id -u)" -ne 0 ]; then
    echo 'Run this launcher as root (or with sudo).' >&2
    exit 1
fi
if ! getent group ipcm >/dev/null; then
    echo 'Create the group first: groupadd --system ipcm' >&2
    exit 1
fi
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec runuser -u root -g ipcm -- "$project_dir/build/ipcm-server" "$@"
