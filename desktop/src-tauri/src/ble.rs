//! Bluetooth LE transport for pairing: scan for Cardputers in pairing mode
//! and move Control frames over GATT (btleplug). Protocol logic lives in
//! pairing.rs; this file only finds devices and carries bytes.

use std::{collections::HashMap, pin::Pin, str::FromStr, time::Duration};

use btleplug::{
    api::{Central, Characteristic, Manager as _, Peripheral as _, ScanFilter, ValueNotification, WriteType},
    platform::{Adapter, Manager, Peripheral},
};
use futures::{Stream, StreamExt};
use serde::Serialize;
use uuid::Uuid;

use crate::pairing::{
    CONTROL_UUID, ControlChannel, INFO_UUID, MFG_COMPANY, MFG_FLAG_PAIRED, MFG_FLAG_PAIRING, PairError,
    PairResult, Reassembler, SERVICE_UUID, fragment,
};

/// Writes use "write with response", which the OS splits into long writes
/// when needed; keep fragments well under the 512-byte attribute limit.
const WRITE_FRAGMENT: usize = 180;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NearbyDevice {
    pub device_id: String,
    pub name: String,
    pub pairing: bool,
    pub has_peer: bool,
    pub rssi: Option<i16>,
}

fn transport<E: std::fmt::Display>(e: E) -> PairError {
    PairError::Transport(e.to_string())
}

fn uuid(text: &str) -> Uuid {
    Uuid::from_str(text).expect("constant UUID")
}

/// Parses our manufacturer data: "CP", flags, 8-byte device id.
fn parse_manufacturer(data: &[u8]) -> Option<(String, u8)> {
    if data.len() < 11 || &data[..2] != b"CP" {
        return None;
    }
    let id = data[3..11].iter().map(|b| format!("{b:02x}")).collect();
    Some((id, data[2]))
}

async fn adapter() -> PairResult<Adapter> {
    let manager = Manager::new().await.map_err(transport)?;
    manager
        .adapters()
        .await
        .map_err(transport)?
        .into_iter()
        .next()
        .ok_or_else(|| PairError::Transport("no Bluetooth adapter found".into()))
}

/// Scans for devices advertising the Recorder Sync service. Returns what
/// was seen plus the peripherals, keyed by device id, for connecting.
pub async fn scan(duration: Duration) -> PairResult<(Vec<NearbyDevice>, HashMap<String, Peripheral>)> {
    let central = adapter().await?;
    central.start_scan(ScanFilter { services: vec![uuid(SERVICE_UUID)] }).await.map_err(transport)?;
    tokio::time::sleep(duration).await;
    let peripherals = central.peripherals().await.map_err(transport)?;
    let _ = central.stop_scan().await;

    let mut found = Vec::new();
    let mut by_id = HashMap::new();
    for peripheral in peripherals {
        let Ok(Some(props)) = peripheral.properties().await else { continue };
        let Some((device_id, flags)) = props.manufacturer_data.get(&MFG_COMPANY).and_then(|d| parse_manufacturer(d))
        else {
            continue;
        };
        found.push(NearbyDevice {
            device_id: device_id.clone(),
            name: props.local_name.unwrap_or_else(|| "Cardputer".into()),
            pairing: flags & MFG_FLAG_PAIRING != 0,
            has_peer: flags & MFG_FLAG_PAIRED != 0,
            rssi: props.rssi,
        });
        by_id.insert(device_id, peripheral);
    }
    found.sort_by_key(|d| std::cmp::Reverse(d.rssi.unwrap_or(i16::MIN)));
    Ok((found, by_id))
}

type Notifications = Pin<Box<dyn Stream<Item = ValueNotification> + Send>>;

pub struct BleLink {
    peripheral: Peripheral,
    control: Characteristic,
    notifications: Notifications,
    reassembler: Reassembler,
}

impl BleLink {
    pub async fn connect(peripheral: Peripheral) -> PairResult<Self> {
        if !peripheral.is_connected().await.unwrap_or(false) {
            tokio::time::timeout(Duration::from_secs(15), peripheral.connect())
                .await
                .map_err(|_| PairError::Timeout)?
                .map_err(transport)?;
        }
        peripheral.discover_services().await.map_err(transport)?;
        let characteristics = peripheral.characteristics();
        let find = |id: &str| characteristics.iter().find(|c| c.uuid == uuid(id)).cloned();
        let control = find(CONTROL_UUID).ok_or_else(|| PairError::Protocol("no Control characteristic".into()))?;
        if find(INFO_UUID).is_none() {
            return Err(PairError::Protocol("no Info characteristic".into()));
        }
        let notifications = peripheral.notifications().await.map_err(transport)?;
        peripheral.subscribe(&control).await.map_err(transport)?;
        Ok(Self { peripheral, control, notifications, reassembler: Reassembler::default() })
    }

    pub async fn disconnect(self) {
        let _ = self.peripheral.unsubscribe(&self.control).await;
        let _ = self.peripheral.disconnect().await;
    }
}

impl ControlChannel for BleLink {
    async fn send(&mut self, frame: Vec<u8>) -> PairResult<()> {
        for piece in fragment(&frame, WRITE_FRAGMENT) {
            self.peripheral.write(&self.control, &piece, WriteType::WithResponse).await.map_err(transport)?;
        }
        Ok(())
    }

    async fn recv(&mut self, timeout: Duration) -> PairResult<Vec<u8>> {
        let deadline = tokio::time::Instant::now() + timeout;
        loop {
            let next = tokio::time::timeout_at(deadline, self.notifications.next())
                .await
                .map_err(|_| PairError::Timeout)?
                .ok_or_else(|| PairError::Transport("the device disconnected".into()))?;
            if next.uuid != self.control.uuid {
                continue;
            }
            if let Some(message) = self.reassembler.push(&next.value)? {
                return Ok(message);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_advertised_identity() {
        let mut data = b"CP".to_vec();
        data.push(MFG_FLAG_PAIRING | MFG_FLAG_PAIRED);
        data.extend_from_slice(&[0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef]);
        let (id, flags) = parse_manufacturer(&data).unwrap();
        assert_eq!(id, "0123456789abcdef");
        assert_eq!(flags, 3);
        assert!(parse_manufacturer(b"XX").is_none());
    }
}
