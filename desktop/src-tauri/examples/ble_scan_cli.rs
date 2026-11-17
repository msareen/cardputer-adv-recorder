//! Debugs pairing discovery: shows what this machine sees over Bluetooth LE.
//! Usage: cargo run --example ble_scan_cli -- [seconds] [--all]
//! `--all` lists every advertiser, not only ones that look like a recorder.
//! Run it in a fresh process: the OS caches advertisement data per device.
use std::time::Duration;

use btleplug::api::{Central, Manager as _, Peripheral as _, ScanFilter};
use btleplug::platform::Manager;
use cprec_desktop_lib::{
    ble,
    pairing::{MFG_COMPANY, SERVICE_UUID},
};

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let all = args.iter().any(|a| a == "--all");
    let seconds = args.iter().find_map(|a| a.parse().ok()).unwrap_or(8);

    let manager = Manager::new().await.expect("Bluetooth manager");
    let Some(central) = manager.adapters().await.expect("adapters").into_iter().next() else {
        println!("no Bluetooth adapter");
        return;
    };
    println!("adapter: {:?}", central.adapter_info().await);

    central.start_scan(ScanFilter::default()).await.expect("start scan");
    tokio::time::sleep(Duration::from_secs(seconds)).await;
    let _ = central.stop_scan().await;
    let peripherals = central.peripherals().await.expect("peripherals");
    println!("{} advertisers seen", peripherals.len());
    for p in peripherals {
        let Ok(Some(props)) = p.properties().await else { continue };
        let service = props.services.iter().any(|u| u.to_string() == SERVICE_UUID);
        let mfg = props.manufacturer_data.get(&MFG_COMPANY);
        if all || service || mfg.is_some() {
            println!(
                "  {} name={:?} rssi={:?} recorder_service={} recorder_mfg={:02x?} mfg_ids={:?}",
                props.address,
                props.local_name,
                props.rssi,
                service,
                mfg,
                props.manufacturer_data.keys().collect::<Vec<_>>(),
            );
        }
    }

    match ble::scan(Duration::from_secs(seconds)).await {
        Ok((found, _)) => println!("pairing scan finds: {found:?}"),
        Err(e) => println!("pairing scan error: {e}"),
    }
}
