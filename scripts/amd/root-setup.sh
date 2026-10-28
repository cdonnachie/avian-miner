#!/bin/bash
# Run this once on the droplet as root. Everything after it I can do myself.
#
# Docker rather than installing ROCm on the host, and the reason is the host:
# this droplet is Ubuntu 26.04, and AMD's ROCm repositories publish for 24.04
# and 22.04. Installing against the wrong release is how an afternoon
# disappears into library version conflicts. A container brings its own
# matched ROCm userspace and only needs the kernel's amdgpu driver, which is
# already loaded here.
set -eu

echo "=== docker ==="
if ! command -v docker >/dev/null; then
    apt-get update -qq
    apt-get install -y -qq docker.io
fi
systemctl enable --now docker

echo "=== let claude reach the GPU and run containers ==="
# render and video own /dev/kfd and /dev/dri; without them the device nodes
# are unreadable and ROCm reports no agents rather than a permission error,
# which is a confusing way to spend an hour.
usermod -aG docker,render,video claude

echo "=== pull the ROCm development image ==="
# Matched userspace for the host's driver, with hipcc and the runtime in it.
docker pull rocm/dev-ubuntu-24.04:latest

echo
echo "=== check ==="
docker run --rm --device=/dev/kfd --device=/dev/dri --group-add video \
    rocm/dev-ubuntu-24.04:latest rocminfo 2>&1 | grep -E "Name:|gfx|Marketing" | head -10
echo
echo "Done. Tell Claude; the new group membership applies to its next login."
