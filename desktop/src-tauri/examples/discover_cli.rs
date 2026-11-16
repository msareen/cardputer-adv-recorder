//! Prints devices announcing sync mode on the network, using the same
//! discovery code as the app's worker:
//!
//!   cargo run --example discover_cli -- 10      # listen for 10 s

use std::time::Duration;

use cprec_desktop_lib::discovery::{self, DiscoveryEvent};

#[tokio::main]
async fn main() {
    let seconds: u64 = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(10);
    let (_daemon, mut events) = discovery::start().expect("mDNS");
    let deadline = tokio::time::Instant::now() + Duration::from_secs(seconds);
    while let Ok(Some(event)) = tokio::time::timeout_at(deadline, events.recv()).await {
        match event {
            DiscoveryEvent::Found(f) => println!("found {} at {} ({})", f.device_id, f.address, f.fullname),
            DiscoveryEvent::Removed { fullname } => println!("removed {fullname}"),
        }
    }
}
