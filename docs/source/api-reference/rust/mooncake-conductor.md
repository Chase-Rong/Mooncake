# Mooncake Conductor Rust API

This page documents the Rust crate `mooncake_conductor` (located at `mooncake-conductor/rust`).
It is a **safe wrapper** around the Mooncake Conductor client C API (`conductor_c.h`) and
mirrors the Python client in `mooncake.conductor`.

For the conductor's concepts (contexts, tenants, storage tiers, hash profiles), see the
[Conductor Indexer API](../http/conductor-indexer.md).

## Build & runtime prerequisites

- **Build**:
  - Via CMake: `-DWITH_CONDUCTOR=ON -DWITH_CONDUCTOR_RUST=ON`, then build the
    `build_mooncake_conductor_rust` (library) or `build_mooncake_conductor_rust_tests` target.
  - Or with Cargo against an existing CMake build. `build.rs` statically links
    `libconductor_cpp_core.a` from `MOONCAKE_CONDUCTOR_LIB_DIR` (default
    `build/mooncake-conductor`) and finds its shared dependencies (libzmq, OpenSSL, glog,
    jsoncpp) through the CMake cache in `MOONCAKE_BUILD_DIR`. `MOONCAKE_CONDUCTOR_INCLUDE_DIR`
    overrides the header location.
  - bindgen needs libclang (set `LIBCLANG_PATH` if it is not on the default search path).
- **Runtime**:
  - The library search directories are embedded as rpath entries, so binaries run on the
    build machine without setting `LD_LIBRARY_PATH`.
  - A running `mooncake_conductor`. It serves RPC on port 13334 (HTTP on 13333) unless the
    config file at `CONDUCTOR_CONFIG_PATH` (default `~/.mooncake/conductor_config.json`)
    overrides them.

## Quick start (copy-paste)

```rust
use mooncake_conductor::{
    ClientConfig, ConductorClient, ConductorError, HashProfileConfig, QueryRequest, ServiceConfig,
};

fn main() -> Result<(), ConductorError> {
    let client = ConductorClient::new()?;
    client.setup(&ClientConfig::new("127.0.0.1:13334"))?;

    let profile = HashProfileConfig {
        strategy: "vllm_v1".to_string(),
        algorithm: "sha256_cbor".to_string(),
        python_hash_seed: "0".to_string(),
        index_projection: "low64_be".to_string(),
    };
    let service = ServiceConfig::new("instance-0", "tcp://127.0.0.1:5557", "my-model", 16, profile);
    client.register(&service)?;

    let tokens: Vec<i32> = (1..=64).collect();
    let result = client.query(&QueryRequest::new("my-model", 16, &tokens))?;
    for (instance_id, hit) in &result.hits {
        println!("{instance_id}: {} tokens matched", hit.longest_matched);
    }

    client.unregister("instance-0", "", 0)?;
    Ok(())
}
```

A runnable version is `mooncake-conductor/rust/examples/basic_usage.rs`
(`cargo run --example basic_usage`).

## API reference

### `ConductorClient`

Every method is a blocking RPC. The connection is closed and the handle released on drop.

#### `new() -> Result<ConductorClient, ConductorError>`

Create an unconnected client. Operations fail with `Unavailable` until `setup()` succeeds.

#### `setup(config) -> Result<(), ConductorError>`

Connect to `config.conductor_addr` (`"host:port"`).

- `InvalidParams`: the address has no port.
- `RpcFail`: the conductor could not be reached within `config.connect_timeout`.

May be called again after `close()` to reconnect.

#### `close()`

Close the connection. Idempotent. Later operations fail with `Unavailable`.

#### `health_check() -> HealthStatus`

- `NotInitialized` until a `setup()` has succeeded.
- `Healthy` if a `list_services` round trip succeeds.
- `Unreachable` otherwise, including after `close()`.

#### `query(request) -> Result<QueryResult, ConductorError>`

Query prefix-cache hits for `request.token_ids` in the context identified by model, LoRA,
block size and tenant.

- The result has one entry per instance registered for that context (or only
  `request.instance_filter`). An instance without matching blocks reports zeros.
- An unknown context or filter yields an empty result.
- `InvalidParams`: empty `model_name` or non-positive `block_size`.

#### `register(config) -> Result<RegisterResult, ConductorError>`

Register a KV-event publisher. The conductor subscribes to `config.endpoint`.

- `is_new` is `false` when an identical service was already registered.
- `InvalidParams`: invalid hash profile or config, or a conflicting registration of the same
  instance.

#### `unregister(instance_id, tenant_id, dp_rank) -> Result<(), ConductorError>`

Unregister a service. An empty `tenant_id` means `"default"`. Returns `ServiceNotFound` when
no matching service is registered.

#### `get_global_view() -> Result<GlobalView, ConductorError>`

Snapshot of every indexed context, its prefix count, hash profile, and the instances (with DP
ranks) serving it.

#### `list_services() -> Result<Vec<ServiceInfo>, ConductorError>`

All registered services, including the `root_digest` the server derived for each hash profile.

### Request types

#### `ClientConfig`

`ClientConfig::new(conductor_addr)` uses the Python client's timeouts:
`connect_timeout` 1 s and `request_timeout` 3 s.

#### `QueryRequest<'a>`

Borrows its strings and `token_ids`, so token arrays are not copied before the FFI call.
`QueryRequest::new(model_name, block_size, &token_ids)` defaults to no LoRA, tenant
`"default"`, no cache salt and no instance filter. Override them with `with_lora_name`,
`with_tenant_id`, `with_cache_salt` and `with_instance_filter`.

#### `ServiceConfig`

`ServiceConfig::new(instance_id, endpoint, model_name, block_size, hash_profile)` sets the
required fields. The others default to empty or zero:

- `replay_endpoint`: ZMQ replay endpoint used to recover missed events.
- `publisher_kind`: `PublisherKind::Vllm` (default), `Mooncake` or `Sglang`.
- `lora_name`, `tenant_id` (empty means `"default"`), `dp_rank`.
- `cache_group`: `None` keeps the server default. The conductor currently accepts only
  `Some(0)`.

`HashProfileConfig` holds `strategy`, `algorithm`, `python_hash_seed` and `index_projection`,
which must match how the engine hashes its blocks. The conductor accepts:

| `strategy` | `algorithm` | `index_projection` |
|------------|-------------|--------------------|
| `vllm_v1` | `sha256` or `sha256_cbor` | `low64_be` |
| `sglang`, `sglang_bigram` | `sha256_raw` | `first64_be` |

### Result types

Results are owned Rust values. The C-allocated output is copied and freed before the method
returns.

- `QueryResult.hits`: `BTreeMap<instance_id, CacheHit>`.
- `CacheHit`: `longest_matched` (tokens), `dp` (DP rank → tokens matched on the device tier),
  `rank_matches` (DP rank → `RankMatch`), and the per-tier maxima `device`, `cpu_local`,
  `cpu_share`, `disk`. `device` counts prefixes in inference-engine device memory (GPU, NPU,
  or another accelerator), matching the `device` field of the HTTP/msgpack response.
- `RegisterResult.is_new`.
- `ServiceInfo`: the registered `ServiceConfig` fields as normalized by the server, with the
  resolved `HashProfile` (including `root_digest`).
- `GlobalView.contexts`: `ContextView` with `model_name`, `lora_name`, `block_size`,
  `tenant_id`, `prefix_count`, `hash_profile` and `instances` (instance id → DP ranks).
- `HealthStatus`: `Healthy`, `NotInitialized` or `Unreachable`.

### `ConductorError`

Errors map one-to-one from the C status codes, which equal the C++ `ErrorCode` values the
Python module re-exports. `code()` returns the C code (if any) and `from_code()` maps a code
back. The enum is `#[non_exhaustive]`.

| Variant | C code | Meaning |
|---------|--------|---------|
| `NullHandle` | — | The client handle could not be allocated. |
| `InvalidString` | — | A string argument contains an interior `\0`. |
| `Internal` | -1 | Unexpected failure inside the client or conductor. |
| `InvalidParams` | -600 | The request was rejected as malformed. |
| `RpcFail` | -900 | The RPC transport failed. |
| `RpcTimeout` | -901 | The RPC did not complete within `request_timeout`. |
| `Unavailable` | -2000 | The client is not connected; call `setup()` first. |
| `ServiceNotFound` | -2001 | `unregister()` found no matching service. |
| `Unknown(i32)` | other | A code this crate does not know about. |

## Safety & thread-safety

- The public API has no `unsafe` methods.
- `ConductorClient` is `Send + Sync`. The C++ client serializes calls on an internal mutex, so
  calls on one shared client do not run in parallel; use one client per thread for parallel
  RPCs.

## Examples & tests

- `examples/basic_usage.rs`: register, list, global view, query and unregister. It exits 0
  with `[SKIP]` when no conductor is running. `MC_CONDUCTOR_ADDR` overrides the address.
- `cargo test`: unit tests and server-free integration tests. Set
  `MC_RUST_CONDUCTOR_RUN_INTEGRATION=1` (and optionally `MC_RUST_CONDUCTOR_ADDR`, default
  `127.0.0.1:13334`) to also run a round trip against a live conductor.
- `scripts/ci/run_conductor_rust_smoke.sh` starts a conductor and runs both against it.
