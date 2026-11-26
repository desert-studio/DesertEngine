#!/usr/bin/env bash
# schema_lock.sh — rewrite Desert/Schema/ComponentSchema.lock to the code's COMPONENT blocks and print the diff.
# The one command that changes the lock: the build only compares it and fails on a difference (DesertHeaderTool
# without --write-schema-lock). Runs the DesertSchemaLock target of the host preset's build tree (configured
# before: dev_configure); commit the lock with the change that moved a component field.
set -u
source "$(dirname "$0")/_common.sh"
[ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ] && { dev_help "$0"; exit 0; }
[ -f "$DEV_BUILD/build.ninja" ] || { echo "schema_lock: no configured build tree at $DEV_BUILD (run a build first)"; exit 2; }
cmake --build "$DEV_BUILD" --config "$DEV_CONFIG" --target DesertSchemaLock || exit 1
git -C "$DEV_ROOT" --no-pager diff --stat -- Desert/Schema/ComponentSchema.lock
