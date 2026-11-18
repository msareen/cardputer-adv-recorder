//! SQLite store: users, devices, recordings (data model from the
//! architecture doc). Every query is scoped by user id.

use std::path::Path;

use rusqlite::{Connection, OptionalExtension, params};
use serde::Serialize;

use crate::protocol::DeviceInfo;

pub type DbResult<T> = Result<T, rusqlite::Error>;

pub struct Db {
    conn: Connection,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct User {
    pub id: i64,
    pub username: String,
    pub library_path: String,
    /// Sync devices automatically when they appear on the network.
    pub auto_sync: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct DeviceRow {
    pub id: String,
    pub name: String,
    pub address: Option<String>,
    pub last_cursor: u64,
    pub last_seen_ms: Option<i64>,
    pub firmware: Option<String>,
    pub battery_pct: Option<i64>,
    pub unsynced: Option<i64>,
    pub free_bytes: Option<i64>,
    /// Paired over BLE: synced with mutual TLS against the stored key.
    /// Unpaired rows are dev devices reached over plain HTTP.
    pub paired: bool,
    /// Paired without WiFi: the device has no network to join, so every
    /// sync runs over the BLE fallback and auto-sync never applies.
    pub ble_only: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct RecordingRow {
    pub id: String,
    pub device_id: String,
    pub device_name: String,
    /// Name on the device, kept current by sync.
    pub name: String,
    /// Desktop-only rename; shown instead of `name` when set.
    pub display_name: Option<String>,
    pub size: u64,
    pub sha256: String,
    pub created_ms: i64,
    pub duration_ms: u64,
    pub path: String,
    pub synced_at_ms: i64,
    pub acked: bool,
    pub on_device: bool,
}

const SCHEMA: &str = r#"
PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS users (
    id            INTEGER PRIMARY KEY,
    username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
    password_hash TEXT NOT NULL,
    library_path  TEXT NOT NULL,
    created_ms    INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS devices (
    user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    id          TEXT NOT NULL,
    name        TEXT NOT NULL,
    address     TEXT,
    public_key  BLOB,
    last_cursor INTEGER NOT NULL DEFAULT 0,
    last_seen   INTEGER,
    firmware    TEXT,
    battery_pct INTEGER,
    unsynced    INTEGER,
    free_bytes  INTEGER,
    ble_only    INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (user_id, id)
);
-- Recordings outlive their device row: removing a device forgets it on the
-- desktop only, and synced files stay in the library.
CREATE TABLE IF NOT EXISTS recordings (
    user_id      INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_id    TEXT NOT NULL,
    id           TEXT NOT NULL,
    device_name  TEXT NOT NULL,
    name         TEXT NOT NULL,
    display_name TEXT,
    size         INTEGER NOT NULL,
    sha256       TEXT NOT NULL,
    created_ms   INTEGER NOT NULL,
    duration_ms  INTEGER NOT NULL,
    path         TEXT NOT NULL,
    synced_at    INTEGER NOT NULL,
    acked        INTEGER NOT NULL DEFAULT 0,
    on_device    INTEGER NOT NULL DEFAULT 1,
    PRIMARY KEY (user_id, device_id, id)
);
"#;

fn device_from_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<DeviceRow> {
    Ok(DeviceRow {
        id: row.get("id")?,
        name: row.get("name")?,
        address: row.get("address")?,
        last_cursor: row.get::<_, i64>("last_cursor")? as u64,
        last_seen_ms: row.get("last_seen")?,
        firmware: row.get("firmware")?,
        battery_pct: row.get("battery_pct")?,
        unsynced: row.get("unsynced")?,
        free_bytes: row.get("free_bytes")?,
        paired: row.get::<_, Option<Vec<u8>>>("public_key")?.is_some(),
        ble_only: row.get::<_, i64>("ble_only")? != 0,
    })
}

fn recording_from_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<RecordingRow> {
    Ok(RecordingRow {
        id: row.get("id")?,
        device_id: row.get("device_id")?,
        device_name: row.get("device_name")?,
        name: row.get("name")?,
        display_name: row.get("display_name")?,
        size: row.get::<_, i64>("size")? as u64,
        sha256: row.get("sha256")?,
        created_ms: row.get("created_ms")?,
        duration_ms: row.get::<_, i64>("duration_ms")? as u64,
        path: row.get("path")?,
        synced_at_ms: row.get("synced_at")?,
        acked: row.get::<_, i64>("acked")? != 0,
        on_device: row.get::<_, i64>("on_device")? != 0,
    })
}

impl Db {
    pub fn open(path: &Path) -> DbResult<Self> {
        Self::init(Connection::open(path)?)
    }

    pub fn open_in_memory() -> DbResult<Self> {
        Self::init(Connection::open_in_memory()?)
    }

    fn init(conn: Connection) -> DbResult<Self> {
        conn.execute_batch(SCHEMA)?;
        let db = Self { conn };
        db.migrate()?;
        Ok(db)
    }

    /// Upgrades databases created by earlier versions (PRAGMA user_version).
    fn migrate(&self) -> DbResult<()> {
        let version: i64 = self.conn.query_row("PRAGMA user_version", [], |r| r.get(0))?;
        if version < 1 {
            // Milestone 5: per-user auto-sync and app-wide settings.
            let has_auto_sync = self
                .conn
                .prepare("SELECT 1 FROM pragma_table_info('users') WHERE name = 'auto_sync'")?
                .exists([])?;
            if !has_auto_sync {
                self.conn.execute_batch("ALTER TABLE users ADD COLUMN auto_sync INTEGER NOT NULL DEFAULT 1;")?;
            }
            self.conn.execute_batch(
                "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
                 PRAGMA user_version = 1;",
            )?;
        }
        if version < 2 {
            // Bluetooth-only devices (paired without WiFi).
            let has_ble_only = self
                .conn
                .prepare("SELECT 1 FROM pragma_table_info('devices') WHERE name = 'ble_only'")?
                .exists([])?;
            if !has_ble_only {
                self.conn.execute_batch("ALTER TABLE devices ADD COLUMN ble_only INTEGER NOT NULL DEFAULT 0;")?;
            }
            self.conn.execute_batch("PRAGMA user_version = 2;")?;
        }
        Ok(())
    }

    // --- app settings --------------------------------------------------

    pub fn setting(&self, key: &str) -> DbResult<Option<String>> {
        self.conn
            .query_row("SELECT value FROM settings WHERE key = ?1", params![key], |r| r.get(0))
            .optional()
    }

    pub fn set_setting(&self, key: &str, value: Option<&str>) -> DbResult<()> {
        match value {
            Some(v) => self.conn.execute(
                "INSERT INTO settings (key, value) VALUES (?1, ?2)
                 ON CONFLICT (key) DO UPDATE SET value = excluded.value",
                params![key, v],
            )?,
            None => self.conn.execute("DELETE FROM settings WHERE key = ?1", params![key])?,
        };
        Ok(())
    }

    // --- users ---------------------------------------------------------

    pub fn create_user(&self, username: &str, password_hash: &str, library: &str, now_ms: i64) -> DbResult<User> {
        self.conn.execute(
            "INSERT INTO users (username, password_hash, library_path, created_ms) VALUES (?1, ?2, ?3, ?4)",
            params![username, password_hash, library, now_ms],
        )?;
        Ok(User {
            id: self.conn.last_insert_rowid(),
            username: username.to_string(),
            library_path: library.to_string(),
            auto_sync: true,
        })
    }

    pub fn user_by_id(&self, user_id: i64) -> DbResult<Option<User>> {
        self.conn
            .query_row(
                "SELECT id, username, library_path, auto_sync FROM users WHERE id = ?1",
                params![user_id],
                |r| {
                    Ok(User {
                        id: r.get(0)?,
                        username: r.get(1)?,
                        library_path: r.get(2)?,
                        auto_sync: r.get::<_, i64>(3)? != 0,
                    })
                },
            )
            .optional()
    }

    pub fn set_auto_sync(&self, user_id: i64, enabled: bool) -> DbResult<()> {
        self.conn.execute(
            "UPDATE users SET auto_sync = ?2 WHERE id = ?1",
            params![user_id, enabled as i64],
        )?;
        Ok(())
    }

    /// Returns the user and their password hash.
    pub fn find_user(&self, username: &str) -> DbResult<Option<(User, String)>> {
        self.conn
            .query_row(
                "SELECT id, username, library_path, auto_sync, password_hash FROM users WHERE username = ?1",
                params![username],
                |r| {
                    Ok((
                        User {
                            id: r.get(0)?,
                            username: r.get(1)?,
                            library_path: r.get(2)?,
                            auto_sync: r.get::<_, i64>(3)? != 0,
                        },
                        r.get::<_, String>(4)?,
                    ))
                },
            )
            .optional()
    }

    pub fn set_library_path(&self, user_id: i64, library: &str) -> DbResult<()> {
        self.conn.execute(
            "UPDATE users SET library_path = ?2 WHERE id = ?1",
            params![user_id, library],
        )?;
        Ok(())
    }

    // --- devices -------------------------------------------------------

    pub fn get_device(&self, user_id: i64, device_id: &str) -> DbResult<Option<DeviceRow>> {
        self.conn
            .query_row(
                "SELECT * FROM devices WHERE user_id = ?1 AND id = ?2",
                params![user_id, device_id],
                device_from_row,
            )
            .optional()
    }

    pub fn list_devices(&self, user_id: i64) -> DbResult<Vec<DeviceRow>> {
        let mut stmt = self.conn.prepare("SELECT * FROM devices WHERE user_id = ?1 ORDER BY name")?;
        let rows = stmt.query_map(params![user_id], device_from_row)?;
        rows.collect()
    }

    /// Records that the device answered `/info` at `address`, creating the
    /// device row on first contact.
    pub fn upsert_device_seen(
        &self,
        user_id: i64,
        info: &DeviceInfo,
        address: Option<&str>,
        now_ms: i64,
    ) -> DbResult<DeviceRow> {
        let default_name = format!("Cardputer {}", &info.device_id[..info.device_id.len().min(4)]);
        self.conn.execute(
            "INSERT INTO devices (user_id, id, name, address, last_seen, firmware, battery_pct, unsynced, free_bytes)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)
             ON CONFLICT (user_id, id) DO UPDATE SET
                address = COALESCE(excluded.address, devices.address),
                last_seen = excluded.last_seen,
                firmware = excluded.firmware,
                battery_pct = excluded.battery_pct,
                unsynced = excluded.unsynced,
                free_bytes = excluded.free_bytes",
            params![
                user_id,
                info.device_id,
                default_name,
                address,
                now_ms,
                info.firmware,
                info.battery_pct,
                info.unsynced as i64,
                info.free_bytes as i64
            ],
        )?;
        Ok(self.get_device(user_id, &info.device_id)?.expect("row just written"))
    }

    pub fn set_device_unsynced(&self, user_id: i64, device_id: &str, unsynced: u64) -> DbResult<()> {
        self.conn.execute(
            "UPDATE devices SET unsynced = ?3 WHERE user_id = ?1 AND id = ?2",
            params![user_id, device_id, unsynced as i64],
        )?;
        Ok(())
    }

    pub fn set_device_cursor(&self, user_id: i64, device_id: &str, cursor: u64) -> DbResult<()> {
        self.conn.execute(
            "UPDATE devices SET last_cursor = ?3 WHERE user_id = ?1 AND id = ?2",
            params![user_id, device_id, cursor as i64],
        )?;
        Ok(())
    }

    /// Stores a device paired over BLE (new or re-paired). Keeps the name a
    /// user gave it before. `ble_only` records that the device was given no
    /// WiFi network (it also clears the old address, which no longer works).
    #[allow(clippy::too_many_arguments)]
    pub fn save_paired_device(
        &self,
        user_id: i64,
        device_id: &str,
        public_key: &[u8],
        address: Option<&str>,
        firmware: Option<&str>,
        ble_only: bool,
        now_ms: i64,
    ) -> DbResult<DeviceRow> {
        let default_name = format!("Cardputer {}", &device_id[..device_id.len().min(4)]);
        self.conn.execute(
            "INSERT INTO devices (user_id, id, name, address, public_key, last_seen, firmware, ble_only)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
             ON CONFLICT (user_id, id) DO UPDATE SET
                public_key = excluded.public_key,
                address = CASE WHEN excluded.ble_only THEN NULL
                               ELSE COALESCE(excluded.address, devices.address) END,
                last_seen = excluded.last_seen,
                firmware = COALESCE(excluded.firmware, devices.firmware),
                ble_only = excluded.ble_only",
            params![user_id, device_id, default_name, address, public_key, now_ms, firmware, ble_only],
        )?;
        Ok(self.get_device(user_id, device_id)?.expect("row just written"))
    }

    pub fn device_public_key(&self, user_id: i64, device_id: &str) -> DbResult<Option<Vec<u8>>> {
        Ok(self
            .conn
            .query_row(
                "SELECT public_key FROM devices WHERE user_id = ?1 AND id = ?2",
                params![user_id, device_id],
                |r| r.get::<_, Option<Vec<u8>>>(0),
            )
            .optional()?
            .flatten())
    }

    pub fn set_device_address(&self, user_id: i64, device_id: &str, address: &str) -> DbResult<()> {
        self.conn.execute(
            "UPDATE devices SET address = ?3 WHERE user_id = ?1 AND id = ?2",
            params![user_id, device_id, address],
        )?;
        Ok(())
    }

    pub fn rename_device(&self, user_id: i64, device_id: &str, name: &str) -> DbResult<()> {
        self.conn.execute(
            "UPDATE devices SET name = ?3 WHERE user_id = ?1 AND id = ?2",
            params![user_id, device_id, name],
        )?;
        Ok(())
    }

    pub fn remove_device(&self, user_id: i64, device_id: &str) -> DbResult<()> {
        self.conn.execute(
            "DELETE FROM devices WHERE user_id = ?1 AND id = ?2",
            params![user_id, device_id],
        )?;
        Ok(())
    }

    // --- recordings ----------------------------------------------------

    pub fn get_recording(&self, user_id: i64, device_id: &str, id: &str) -> DbResult<Option<RecordingRow>> {
        self.conn
            .query_row(
                "SELECT * FROM recordings WHERE user_id = ?1 AND device_id = ?2 AND id = ?3",
                params![user_id, device_id, id],
                recording_from_row,
            )
            .optional()
    }

    pub fn list_recordings(&self, user_id: i64) -> DbResult<Vec<RecordingRow>> {
        let mut stmt = self.conn.prepare(
            "SELECT * FROM recordings WHERE user_id = ?1
             ORDER BY CASE WHEN created_ms > 0 THEN created_ms ELSE synced_at END DESC",
        )?;
        let rows = stmt.query_map(params![user_id], recording_from_row)?;
        rows.collect()
    }

    pub fn insert_recording(&self, user_id: i64, r: &RecordingRow) -> DbResult<()> {
        self.conn.execute(
            "INSERT INTO recordings (user_id, device_id, id, device_name, name, display_name, size, sha256,
                                     created_ms, duration_ms, path, synced_at, acked, on_device)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)",
            params![
                user_id,
                r.device_id,
                r.id,
                r.device_name,
                r.name,
                r.display_name,
                r.size as i64,
                r.sha256,
                r.created_ms,
                r.duration_ms as i64,
                r.path,
                r.synced_at_ms,
                r.acked as i64,
                r.on_device as i64
            ],
        )?;
        Ok(())
    }

    pub fn set_recording_device_name(&self, user_id: i64, device_id: &str, id: &str, name: &str) -> DbResult<()> {
        self.conn.execute(
            "UPDATE recordings SET name = ?4 WHERE user_id = ?1 AND device_id = ?2 AND id = ?3",
            params![user_id, device_id, id, name],
        )?;
        Ok(())
    }

    pub fn set_recording_display_name(
        &self,
        user_id: i64,
        device_id: &str,
        id: &str,
        display_name: Option<&str>,
    ) -> DbResult<()> {
        self.conn.execute(
            "UPDATE recordings SET display_name = ?4 WHERE user_id = ?1 AND device_id = ?2 AND id = ?3",
            params![user_id, device_id, id, display_name],
        )?;
        Ok(())
    }

    pub fn set_recording_acked(&self, user_id: i64, device_id: &str, id: &str, on_device: bool) -> DbResult<()> {
        self.conn.execute(
            "UPDATE recordings SET acked = 1, on_device = ?4 WHERE user_id = ?1 AND device_id = ?2 AND id = ?3",
            params![user_id, device_id, id, on_device as i64],
        )?;
        Ok(())
    }

    pub fn set_recording_on_device(&self, user_id: i64, device_id: &str, id: &str, on_device: bool) -> DbResult<()> {
        self.conn.execute(
            "UPDATE recordings SET on_device = ?4 WHERE user_id = ?1 AND device_id = ?2 AND id = ?3",
            params![user_id, device_id, id, on_device as i64],
        )?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn info(id: &str) -> DeviceInfo {
        DeviceInfo {
            device_id: id.into(),
            firmware: "t".into(),
            battery_pct: Some(50),
            free_bytes: 10,
            unsynced: 1,
            clock_set: true,
        }
    }

    #[test]
    fn migrates_a_milestone_3_database() {
        let conn = Connection::open_in_memory().unwrap();
        // The users table as milestone 3 created it.
        conn.execute_batch(
            "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE COLLATE NOCASE,
                                 password_hash TEXT NOT NULL, library_path TEXT NOT NULL, created_ms INTEGER NOT NULL);
             INSERT INTO users (username, password_hash, library_path, created_ms) VALUES ('old', 'h', '/l', 0);",
        )
        .unwrap();
        let db = Db::init(conn).unwrap();
        let (user, _) = db.find_user("old").unwrap().unwrap();
        assert!(user.auto_sync, "existing users default to auto-sync on");
        db.set_auto_sync(user.id, false).unwrap();
        assert!(!db.user_by_id(user.id).unwrap().unwrap().auto_sync);
        // Running the migration again is harmless.
        db.migrate().unwrap();
    }

    #[test]
    fn settings_round_trip() {
        let db = Db::open_in_memory().unwrap();
        assert_eq!(db.setting("remembered_user").unwrap(), None);
        db.set_setting("remembered_user", Some("3")).unwrap();
        db.set_setting("remembered_user", Some("4")).unwrap();
        assert_eq!(db.setting("remembered_user").unwrap().as_deref(), Some("4"));
        db.set_setting("remembered_user", None).unwrap();
        assert_eq!(db.setting("remembered_user").unwrap(), None);
    }

    #[test]
    fn usernames_are_unique_ignoring_case() {
        let db = Db::open_in_memory().unwrap();
        db.create_user("Alice", "h", "/lib", 0).unwrap();
        assert!(db.create_user("alice", "h", "/lib", 0).is_err());
        assert_eq!(db.find_user("ALICE").unwrap().unwrap().0.username, "Alice");
    }

    #[test]
    fn devices_are_per_user_and_keep_name_and_cursor() {
        let db = Db::open_in_memory().unwrap();
        let a = db.create_user("a", "h", "/a", 0).unwrap();
        let b = db.create_user("b", "h", "/b", 0).unwrap();
        db.upsert_device_seen(a.id, &info("0123456789abcdef"), Some("http://x:8080"), 1).unwrap();
        db.rename_device(a.id, "0123456789abcdef", "Desk").unwrap();
        db.set_device_cursor(a.id, "0123456789abcdef", 7).unwrap();
        let again = db.upsert_device_seen(a.id, &info("0123456789abcdef"), None, 2).unwrap();
        assert_eq!(again.name, "Desk");
        assert_eq!(again.last_cursor, 7);
        assert_eq!(again.address.as_deref(), Some("http://x:8080"));
        assert!(db.list_devices(b.id).unwrap().is_empty());
        assert!(!again.paired);
    }

    #[test]
    fn pairing_stores_the_key_and_keeps_the_name() {
        let db = Db::open_in_memory().unwrap();
        let a = db.create_user("a", "h", "/a", 0).unwrap();
        db.upsert_device_seen(a.id, &info("0123456789abcdef"), Some("http://x:8080"), 1).unwrap();
        db.rename_device(a.id, "0123456789abcdef", "Desk").unwrap();
        let row = db
            .save_paired_device(a.id, "0123456789abcdef", &[4; 65], Some("https://10.0.0.2:8443"), None, false, 2)
            .unwrap();
        assert!(row.paired);
        assert!(!row.ble_only);
        assert_eq!(row.name, "Desk");
        assert_eq!(row.address.as_deref(), Some("https://10.0.0.2:8443"));
        assert_eq!(db.device_public_key(a.id, "0123456789abcdef").unwrap(), Some(vec![4; 65]));
        assert_eq!(db.device_public_key(a.id, "ffffffffffffffff").unwrap(), None);
    }

    #[test]
    fn bluetooth_only_clears_the_address_and_wifi_restores_it() {
        let db = Db::open_in_memory().unwrap();
        let a = db.create_user("a", "h", "/a", 0).unwrap();
        let id = "0123456789abcdef";
        db.save_paired_device(a.id, id, &[4; 65], Some("https://10.0.0.2:8443"), None, false, 1).unwrap();
        let row = db.save_paired_device(a.id, id, &[4; 65], None, None, true, 2).unwrap();
        assert!(row.ble_only);
        assert_eq!(row.address, None);
        let row = db.save_paired_device(a.id, id, &[4; 65], Some("https://10.0.0.3:8443"), None, false, 3).unwrap();
        assert!(!row.ble_only);
        assert_eq!(row.address.as_deref(), Some("https://10.0.0.3:8443"));
    }

    #[test]
    fn migrates_a_milestone_5_devices_table() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(
            "CREATE TABLE devices (user_id INTEGER NOT NULL, id TEXT NOT NULL, name TEXT NOT NULL, address TEXT,
                                   public_key BLOB, last_cursor INTEGER NOT NULL DEFAULT 0, last_seen INTEGER,
                                   firmware TEXT, battery_pct INTEGER, unsynced INTEGER, free_bytes INTEGER,
                                   PRIMARY KEY (user_id, id));
             INSERT INTO devices (user_id, id, name) VALUES (1, 'abcd', 'Old');
             PRAGMA user_version = 1;",
        )
        .unwrap();
        let db = Db::init(conn).unwrap();
        assert!(!db.get_device(1, "abcd").unwrap().unwrap().ble_only);
        db.migrate().unwrap();
    }
}
