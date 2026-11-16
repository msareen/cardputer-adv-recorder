//! Finds devices in sync mode on the LAN: they announce `_cprec._tcp.local`
//! with TXT `id=<device id>` while their sync server runs (milestone 5).
//! [`Presence`] decides when a present device is due for a sync; the worker
//! in lib.rs acts on it.

use std::{
    collections::HashMap,
    net::Ipv4Addr,
    time::{Duration, Instant},
};

use mdns_sd::{IfKind, ServiceDaemon, ServiceEvent};
use tokio::sync::mpsc;

pub const SERVICE_TYPE: &str = "_cprec._tcp.local.";
/// A failed sync with a device that is still announcing is retried after
/// this long.
pub const RETRY_AFTER: Duration = Duration::from_secs(60);

#[derive(Debug, Clone, PartialEq)]
pub struct Found {
    pub device_id: String,
    pub fullname: String,
    /// `ip:port`, without a scheme: paired devices get https, dev ones http.
    pub address: String,
}

#[derive(Debug, Clone, PartialEq)]
pub enum DiscoveryEvent {
    Found(Found),
    Removed { fullname: String },
}

fn is_device_id(id: &str) -> bool {
    id.len() == 16 && id.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// Builds a [`Found`] from a resolved service. Prefers a LAN address;
/// loopback only when nothing else is offered (tools/mock-device).
pub fn from_resolved(
    fullname: &str,
    addresses: impl IntoIterator<Item = Ipv4Addr>,
    port: u16,
    id: Option<&str>,
) -> Option<Found> {
    let id = id.filter(|id| is_device_id(id))?;
    let mut addresses: Vec<Ipv4Addr> = addresses.into_iter().filter(|a| !a.is_unspecified()).collect();
    addresses.sort_by_key(|a| (a.is_loopback(), a.is_link_local(), *a));
    let ip = addresses.first()?;
    Some(Found { device_id: id.to_string(), fullname: fullname.to_string(), address: format!("{ip}:{port}") })
}

/// Starts browsing. Events arrive on the returned channel; keep the daemon
/// alive for as long as discovery should run.
pub fn start() -> Result<(ServiceDaemon, mpsc::UnboundedReceiver<DiscoveryEvent>), String> {
    let daemon = ServiceDaemon::new().map_err(|e| e.to_string())?;
    // So a mock device on this machine is found too.
    let _ = daemon.enable_interface(IfKind::LoopbackV4);
    let receiver = daemon.browse(SERVICE_TYPE).map_err(|e| e.to_string())?;
    let (tx, rx) = mpsc::unbounded_channel();
    std::thread::Builder::new()
        .name("mdns-browse".into())
        .spawn(move || {
            while let Ok(event) = receiver.recv() {
                let out = match event {
                    ServiceEvent::ServiceResolved(info) => from_resolved(
                        info.get_fullname(),
                        info.get_addresses_v4(),
                        info.get_port(),
                        info.get_property_val_str("id"),
                    )
                    .map(DiscoveryEvent::Found),
                    ServiceEvent::ServiceRemoved(_, fullname) => Some(DiscoveryEvent::Removed { fullname }),
                    _ => None,
                };
                if let Some(out) = out {
                    if tx.send(out).is_err() {
                        break;
                    }
                }
            }
        })
        .map_err(|e| e.to_string())?;
    Ok((daemon, rx))
}

#[derive(Debug)]
struct Seen {
    address: String,
    fullname: String,
    /// Last sync attempt while present: when, and whether it succeeded.
    attempt: Option<(Instant, bool)>,
}

/// Devices currently announcing, and what the worker last did with them.
#[derive(Debug, Default)]
pub struct Presence {
    devices: HashMap<String, Seen>,
}

impl Presence {
    /// Records an announcement. Returns true if the device is new or moved.
    pub fn found(&mut self, found: Found) -> bool {
        match self.devices.get_mut(&found.device_id) {
            Some(seen) if seen.address == found.address => {
                seen.fullname = found.fullname;
                false
            }
            Some(seen) => {
                seen.address = found.address;
                seen.fullname = found.fullname;
                true
            }
            None => {
                self.devices.insert(
                    found.device_id,
                    Seen { address: found.address, fullname: found.fullname, attempt: None },
                );
                true
            }
        }
    }

    /// The device stopped announcing (left sync mode). Its next
    /// announcement starts fresh.
    pub fn removed(&mut self, fullname: &str) -> Option<String> {
        let id = self.devices.iter().find(|(_, s)| s.fullname == fullname).map(|(id, _)| id.clone())?;
        self.devices.remove(&id);
        Some(id)
    }

    /// Devices to sync now: never tried while present, or failed and
    /// waited long enough. A success is not repeated; the device leaves
    /// sync mode after it and announces again next time.
    pub fn due(&self, now: Instant) -> Vec<String> {
        let mut ids: Vec<String> = self
            .devices
            .iter()
            .filter(|(_, s)| match s.attempt {
                None => true,
                Some((at, ok)) => !ok && now.duration_since(at) >= RETRY_AFTER,
            })
            .map(|(id, _)| id.clone())
            .collect();
        ids.sort();
        ids
    }

    pub fn record(&mut self, device_id: &str, ok: bool, now: Instant) {
        if let Some(seen) = self.devices.get_mut(device_id) {
            seen.attempt = Some((now, ok));
        }
    }

    /// "Sync now" from the tray: try every present device again.
    pub fn retry_all(&mut self) {
        for seen in self.devices.values_mut() {
            seen.attempt = None;
        }
    }

    pub fn address(&self, device_id: &str) -> Option<&str> {
        self.devices.get(device_id).map(|s| s.address.as_str())
    }

    pub fn present_ids(&self) -> Vec<String> {
        let mut ids: Vec<String> = self.devices.keys().cloned().collect();
        ids.sort();
        ids
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const ID: &str = "0123456789abcdef";

    fn found(address: &str) -> Found {
        Found { device_id: ID.into(), fullname: "cprec-x._cprec._tcp.local.".into(), address: address.into() }
    }

    #[test]
    fn resolves_lan_address_and_validates_id() {
        let addrs = [Ipv4Addr::new(127, 0, 0, 1), Ipv4Addr::new(169, 254, 1, 2), Ipv4Addr::new(192, 168, 1, 20)];
        let f = from_resolved("n", addrs, 8443, Some(ID)).unwrap();
        assert_eq!(f.address, "192.168.1.20:8443");
        assert_eq!(from_resolved("n", [Ipv4Addr::LOCALHOST], 8080, Some(ID)).unwrap().address, "127.0.0.1:8080");
        assert!(from_resolved("n", addrs, 8443, Some("not-an-id")).is_none());
        assert!(from_resolved("n", addrs, 8443, None).is_none());
        assert!(from_resolved("n", [], 8443, Some(ID)).is_none());
    }

    #[test]
    fn syncs_once_per_appearance_and_retries_failures() {
        let mut p = Presence::default();
        let t0 = Instant::now();
        assert!(p.found(found("10.0.0.2:8443")));
        assert!(!p.found(found("10.0.0.2:8443")), "repeat announcement is not news");
        assert_eq!(p.due(t0), vec![ID]);

        p.record(ID, false, t0);
        assert!(p.due(t0 + Duration::from_secs(10)).is_empty());
        assert_eq!(p.due(t0 + RETRY_AFTER), vec![ID]);

        p.record(ID, true, t0 + RETRY_AFTER);
        assert!(p.due(t0 + RETRY_AFTER * 10).is_empty(), "success is not repeated");

        p.retry_all();
        assert_eq!(p.due(t0), vec![ID]);

        assert_eq!(p.removed("cprec-x._cprec._tcp.local.").as_deref(), Some(ID));
        assert!(p.present_ids().is_empty());
        assert!(p.found(found("10.0.0.2:8443")), "a new appearance syncs again");
        assert!(p.found(found("10.0.0.9:8443")), "moved");
        assert_eq!(p.address(ID), Some("10.0.0.9:8443"));
    }
}
