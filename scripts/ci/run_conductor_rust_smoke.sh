#!/usr/bin/env bash

set -e -o pipefail

# shellcheck source=scripts/ci/services.sh
source "$(dirname "${BASH_SOURCE[0]}")/services.sh"
: "${RUNNER_TEMP:=${TMPDIR:-/tmp}}"
trap 'ci_cleanup_services "$?"' EXIT

: "${GITHUB_WORKSPACE:?GITHUB_WORKSPACE must be set}"

# ASan-enabled CI libraries require the Rust package to link libasan first.
# Release-style nightly builds intentionally leave the ASan runtime unlinked.
case "${MOONCAKE_CONDUCTOR_RUST_LINK_ASAN:-0}" in
    0) unset MOONCAKE_LINK_ASAN ;;
    1) export MOONCAKE_LINK_ASAN=1 ;;
    *)
        echo "MOONCAKE_CONDUCTOR_RUST_LINK_ASAN must be 0 or 1" >&2
        exit 2
        ;;
esac

: "${MOONCAKE_BUILD_DIR:=$GITHUB_WORKSPACE/build}"
: "${MOONCAKE_CONDUCTOR_LIB_DIR:=$MOONCAKE_BUILD_DIR/mooncake-conductor}"
export MOONCAKE_BUILD_DIR MOONCAKE_CONDUCTOR_LIB_DIR
export MOONCAKE_CONDUCTOR_INCLUDE_DIR="$GITHUB_WORKSPACE/mooncake-conductor/include"

# A missing config file keeps the default ports (HTTP 13333, RPC 13334)
# instead of picking up ~/.mooncake/conductor_config.json from the runner.
ci_start_service conductor "$RUNNER_TEMP/mooncake-conductor.log" \
    env CONDUCTOR_CONFIG_PATH="$RUNNER_TEMP/mooncake-conductor-unset.json" \
    "$MOONCAKE_CONDUCTOR_LIB_DIR/mooncake_conductor"
ci_wait_service conductor 13333 13334

cd "$GITHUB_WORKSPACE/mooncake-conductor/rust"
export MC_RUST_CONDUCTOR_RUN_INTEGRATION=true
export MC_RUST_CONDUCTOR_ADDR=127.0.0.1:13334
export MC_CONDUCTOR_ADDR=127.0.0.1:13334

cargo test --lib
cargo test --test minimal_smoke -- --nocapture
cargo run --example basic_usage
