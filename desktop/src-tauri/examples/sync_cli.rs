//! Headless sync with the same engine the app uses. Handy against
//! tools/mock-device or a real device in sync mode:
//!
//!   cargo run --example sync_cli -- 127.0.0.1:8080 ./cli-library
//!
//! Keeps its database in `<library>/cli.sqlite`, so running it twice shows
//! an incremental sync.

use std::{path::PathBuf, sync::Mutex};

use cprec_desktop_lib::{
    db::Db,
    device::{Device, HttpDevice},
    sync::{self, SyncEvent, SyncTarget},
};

#[tokio::main]
async fn main() {
    let mut args = std::env::args().skip(1);
    let (Some(address), Some(library)) = (args.next(), args.next()) else {
        eprintln!("usage: sync_cli <address> <library dir>");
        std::process::exit(2);
    };
    let library = std::path::absolute(PathBuf::from(library)).expect("library path");
    std::fs::create_dir_all(&library).expect("create library");

    let db = Db::open(&library.join("cli.sqlite")).expect("open db");
    let user_id = match db.find_user("cli").expect("query") {
        Some((user, _)) => user.id,
        None => db.create_user("cli", "-", &library.to_string_lossy(), sync::now_ms()).expect("user").id,
    };
    let db = Mutex::new(db);

    let device = HttpDevice::new(&address).expect("address");
    let target = SyncTarget {
        db: &db,
        user_id,
        library: library.clone(),
        address: Some(device.base().to_string()),
        expected_device_id: None,
        now_ms: sync::now_ms,
        tz_offset_min: sync::local_tz_offset_min(),
    };
    let on_event = |event: SyncEvent| match &event {
        SyncEvent::Downloading { done, total, .. } if done != total => {}
        other => println!("{other:?}"),
    };
    match sync::sync_device(&device, &target, &on_event).await {
        Ok(report) => println!("{report:#?}"),
        Err(e) => {
            eprintln!("sync failed: {}", e.user_message());
            std::process::exit(1);
        }
    }
    if let Ok(info) = device.info().await {
        println!("device now reports {} unsynced", info.unsynced);
    }
}
