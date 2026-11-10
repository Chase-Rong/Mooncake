# Mooncake Conductor

Mooncake Conductor is the KV-cache indexer used by cache-aware routers. It
subscribes to KV cache events from vLLM, SGLang, and the Mooncake Store master,
maintains a global prefix cache table across device, host, and disk tiers, and
exposes HTTP and RPC APIs for dynamic service registration and cache-hit
queries.

Start with the [end-to-end Conductor Quick Start](../../getting_started/conductor.md)
for a vLLM and Router deployment, or see the
[Rust client API](../../api-reference/rust/conductor.md).

:::{toctree}
:maxdepth: 1

conductor-architecture-design
publisher-design
subscriber-guide
:::
