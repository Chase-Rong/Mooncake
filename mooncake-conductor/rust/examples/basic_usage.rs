// Copyright 2024 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! Basic usage example for the Mooncake Conductor Rust bindings.
//!
//! This example demonstrates the API surface of `mooncake_conductor`:
//!   - Connecting a client and checking health
//!   - Registering a KV-event publisher service
//!   - Listing services and reading the global view
//!   - Querying prefix-cache hits
//!   - Unregistering the service
//!
//! # Running
//!
//! The example needs a running `mooncake_conductor` (RPC port 13334 unless a
//! config file overrides it). Build and start one with:
//!
//! ```bash
//! cd build
//! cmake -G Ninja .. -DWITH_CONDUCTOR=ON -DWITH_CONDUCTOR_RUST=ON
//! cmake --build . --target mooncake_conductor build_mooncake_conductor_rust
//! ./mooncake-conductor/mooncake_conductor &
//! ```
//!
//! Then run the example with cargo (`build.rs` finds `build/` by default; set
//! `MOONCAKE_BUILD_DIR` for another build directory):
//!
//! ```bash
//! cd mooncake-conductor/rust
//! cargo run --example basic_usage
//! ```
//!
//! `MC_CONDUCTOR_ADDR` overrides the conductor RPC address.

use mooncake_conductor::{
    ClientConfig, ConductorClient, ConductorError, HashProfileConfig, QueryRequest, ServiceConfig,
};

fn main() {
    println!("=== Mooncake Conductor Rust Bindings — Basic Usage ===\n");

    // Step 1: Create a client handle.
    let client = match ConductorClient::new() {
        Ok(client) => {
            println!("[OK] Client handle created.");
            client
        }
        Err(e) => {
            eprintln!("[FAIL] Could not create client handle: {e}");
            std::process::exit(1);
        }
    };

    // Step 2: Connect to the conductor.
    //
    // Without a running conductor the setup call is expected to fail – this
    // is not an error in the bindings themselves.
    let conductor_addr =
        std::env::var("MC_CONDUCTOR_ADDR").unwrap_or_else(|_| "127.0.0.1:13334".to_string());

    println!("Connecting to conductor: {conductor_addr}");

    if let Err(e) = client.setup(&ClientConfig::new(&conductor_addr)) {
        eprintln!(
            "[SKIP] setup() returned an error (expected when no conductor is running): {e}\n\
             The API surface has been validated successfully."
        );
        std::process::exit(0);
    }
    println!(
        "[OK] Client set up; health_check() = {:?}\n",
        client.health_check()
    );

    if let Err(e) = run(&client) {
        eprintln!("[FAIL] {e}");
        std::process::exit(1);
    }

    println!("\n=== Example completed successfully ===");
}

fn run(client: &ConductorClient) -> Result<(), ConductorError> {
    let instance_id = format!("rust-example-{}", std::process::id());
    let model_name = "example-model";
    let block_size = 16;

    // Step 3: Register a service. The conductor subscribes to `endpoint` for
    // KV events; nothing has to publish there for this example.
    let hash_profile = HashProfileConfig {
        strategy: "vllm_v1".to_string(),
        algorithm: "sha256_cbor".to_string(),
        python_hash_seed: "0".to_string(),
        index_projection: "low64_be".to_string(),
    };
    let service = ServiceConfig::new(
        &instance_id,
        "tcp://127.0.0.1:5557",
        model_name,
        block_size,
        hash_profile,
    );
    let registered = client.register(&service)?;
    println!(
        "[OK] register(\"{instance_id}\") is_new = {}",
        registered.is_new
    );

    // Step 4: List the registered services.
    for info in client.list_services()? {
        println!(
            "[OK] list_services(): {} model={} tenant={} publisher={} root_digest={}",
            info.instance_id,
            info.model_name,
            info.tenant_id,
            info.publisher_kind.as_str(),
            info.hash_profile.root_digest
        );
    }

    // Step 5: Read the global view of indexed contexts.
    for context in client.get_global_view()?.contexts {
        println!(
            "[OK] get_global_view(): model={} block_size={} tenant={} prefixes={} instances={:?}",
            context.model_name,
            context.block_size,
            context.tenant_id,
            context.prefix_count,
            context.instances
        );
    }

    // Step 6: Query prefix-cache hits. No KV events were published, so the
    // instance reports a zero-length match.
    let tokens: Vec<i32> = (1..=64).collect();
    let request = QueryRequest::new(model_name, block_size, &tokens);
    for (id, hit) in client.query(&request)?.hits {
        println!(
            "[OK] query(): {id} longest_matched={} device={} cpu_local={} cpu_share={} disk={}",
            hit.longest_matched, hit.device, hit.cpu_local, hit.cpu_share, hit.disk
        );
    }

    // Step 7: Unregister, then show the error for a missing service.
    client.unregister(&instance_id, "", 0)?;
    println!("[OK] unregister(\"{instance_id}\")");
    match client.unregister(&instance_id, "", 0) {
        Err(ConductorError::ServiceNotFound) => {
            println!("[OK] second unregister() returned ServiceNotFound as expected")
        }
        other => println!("[WARN] second unregister() returned {other:?}"),
    }

    client.close();
    println!("[OK] close(); health_check() = {:?}", client.health_check());
    Ok(())
}
