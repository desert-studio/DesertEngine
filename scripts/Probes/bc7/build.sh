#!/usr/bin/env bash
# Builds the BC7 upload+sample probe. See bc7upload.c for what it measures and why it is committed.
#
# Needs glslc and the Vulkan loader. On macOS both come from Homebrew's vulkan-tools / vulkan-loader;
# on Linux they come from the Vulkan SDK or the distribution packages.
set -euo pipefail
cd "$(dirname "$0")"

BREW_PREFIX="${HOMEBREW_PREFIX:-$(brew --prefix 2>/dev/null || echo /opt/homebrew)}"
INC="${VULKAN_SDK:+$VULKAN_SDK/include}"
LIB="${VULKAN_SDK:+$VULKAN_SDK/lib}"
INC="${INC:-$BREW_PREFIX/include}"
LIB="${LIB:-$BREW_PREFIX/lib}"

glslc -fshader-stage=compute bc7fetch.comp -o bc7fetch.spv -mfmt=c
cc -O1 -Wall -I"$INC" bc7upload.c -L"$LIB" -lvulkan -o bc7upload

echo "built ./bc7upload"
echo "run:  ./bc7upload            (feature enabled -- the thing under test)"
echo "      ./bc7upload --disabled (negative control)"
