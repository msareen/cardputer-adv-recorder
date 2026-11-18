// Typed wrappers around the Rust commands in src-tauri/src/lib.rs.
import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";

export interface User {
  id: number;
  username: string;
  libraryPath: string;
  autoSync: boolean;
}

export interface AppSettings {
  autoSync: boolean;
  stayLoggedIn: boolean;
  startAtLogin: boolean;
}

export interface SyncStarted {
  deviceId: string;
  automatic: boolean;
}

export interface SyncFinished {
  deviceId: string;
  automatic: boolean;
  report: SyncReport | null;
  error: string | null;
}

export interface Device {
  id: string;
  name: string;
  address: string | null;
  lastCursor: number;
  lastSeenMs: number | null;
  firmware: string | null;
  batteryPct: number | null;
  unsynced: number | null;
  freeBytes: number | null;
  /** Paired over Bluetooth: synced over HTTPS pinned to its key. */
  paired: boolean;
  /** Paired without WiFi: every sync runs over Bluetooth, no auto-sync. */
  bleOnly: boolean;
}

export interface NearbyDevice {
  deviceId: string;
  name: string;
  pairing: boolean;
  hasPeer: boolean;
  syncMode: boolean;
  rssi: number | null;
}

export interface PairStart {
  deviceId: string;
  code: string | null;
  known: boolean;
}

export interface PairOutcome {
  device: Device;
  wifi: { ok: boolean; ip: string | null };
}

export interface Recording {
  id: string;
  deviceId: string;
  deviceName: string;
  name: string;
  displayName: string | null;
  size: number;
  sha256: string;
  createdMs: number;
  durationMs: number;
  path: string;
  syncedAtMs: number;
  acked: boolean;
  onDevice: boolean;
}

export interface SyncReport {
  deviceId: string;
  downloaded: number;
  renamed: number;
  reacked: number;
  deletedOnDevice: number;
  failed: string[];
  cursor: number;
}

export type SyncEvent =
  | { phase: "connected"; deviceId: string; deviceName: string; firmware: string }
  | { phase: "listed"; count: number }
  | { phase: "downloading"; index: number; count: number; name: string; done: number; total: number }
  | { phase: "verifying"; index: number; count: number; name: string }
  | { phase: "saved"; index: number; count: number; name: string }
  | { phase: "fileFailed"; name: string; message: string };

export interface SyncProgress {
  deviceId: string;
  event: SyncEvent;
}

export interface AgentToken {
  id: number;
  userId: number;
  name: string;
  canWrite: boolean;
  createdMs: number;
  lastUsedMs: number | null;
}

export interface Guidelines {
  general: string;
  recording: string;
  day: string;
}

export interface AgentSettings {
  enabled: boolean;
  port: number;
  /** The MCP endpoint while the server runs. */
  url: string | null;
  error: string | null;
  tokens: AgentToken[];
  guidelines: Guidelines;
}

/** Text an agent attached to a recording ("<deviceId>/<id>") or a day ("YYYY-MM-DD"). */
export interface Attribute {
  scope: "recording" | "day";
  target: string;
  kind: string;
  mime: string;
  path: string;
  size: number;
  sha256: string;
  author: string;
  updatedMs: number;
}

export const api = {
  agentSettings: () => invoke<AgentSettings>("agent_settings"),
  setAgentAccess: (enabled: boolean, port: number) => invoke<AgentSettings>("set_agent_access", { enabled, port }),
  createAgentToken: (name: string, canWrite: boolean) =>
    invoke<{ token: string; row: AgentToken }>("create_agent_token", { name, canWrite }),
  revokeAgentToken: (id: number) => invoke<void>("revoke_agent_token", { id }),
  setAgentGuidelines: (guidelines: Guidelines) => invoke<void>("set_agent_guidelines", { guidelines }),
  listAttributes: () => invoke<Attribute[]>("list_attributes"),
  readAttribute: (scope: string, target: string, kind: string) =>
    invoke<string>("read_attribute", { scope, target, kind }),
  deleteAttribute: (scope: string, target: string, kind: string) =>
    invoke<void>("delete_attribute", { scope, target, kind }),

  session: () => invoke<User | null>("session"),
  createAccount: (username: string, password: string) =>
    invoke<User>("create_account", { username, password }),
  login: (username: string, password: string) => invoke<User>("login", { username, password }),
  logout: () => invoke<void>("logout"),
  setLibraryPath: (path: string) => invoke<User>("set_library_path", { path }),
  appSettings: () => invoke<AppSettings>("app_settings"),
  setAutoSync: (enabled: boolean) => invoke<void>("set_auto_sync", { enabled }),
  setStayLoggedIn: (enabled: boolean) => invoke<void>("set_stay_logged_in", { enabled }),
  setStartAtLogin: (enabled: boolean) => invoke<void>("set_start_at_login", { enabled }),
  presentDevices: () => invoke<string[]>("present_devices"),

  listDevices: () => invoke<Device[]>("list_devices"),
  addDevice: (address: string) => invoke<Device>("add_device", { address }),
  renameDevice: (deviceId: string, name: string) => invoke<void>("rename_device", { deviceId, name }),
  setDeviceAddress: (deviceId: string, address: string) =>
    invoke<void>("set_device_address", { deviceId, address }),
  removeDevice: (deviceId: string) => invoke<void>("remove_device", { deviceId }),
  syncDevice: (deviceId: string) => invoke<SyncReport>("sync_device", { deviceId }),
  /** BLE fallback: the Cardputer must be in Sync over Bluetooth. */
  syncDeviceBle: (deviceId: string) => invoke<SyncReport>("sync_device_ble", { deviceId }),

  bleScan: () => invoke<NearbyDevice[]>("ble_scan"),
  pairStart: (deviceId: string) => invoke<PairStart>("pair_start", { deviceId }),
  pairReady: () => invoke<string[]>("pair_ready"),
  pairFinish: (ssid: string, password: string, bluetoothOnly = false) =>
    invoke<PairOutcome>("pair_finish", { ssid, password, bluetoothOnly }),
  pairCancel: () => invoke<void>("pair_cancel"),

  listRecordings: () => invoke<Recording[]>("list_recordings"),
  renameRecording: (deviceId: string, id: string, name: string) =>
    invoke<void>("rename_recording", { deviceId, id, name }),
  deleteFromDevice: (deviceId: string, id: string) =>
    invoke<void>("delete_from_device", { deviceId, id }),
};

export function onSyncProgress(handler: (p: SyncProgress) => void): Promise<UnlistenFn> {
  return listen<SyncProgress>("sync-progress", (e) => handler(e.payload));
}

export function onSyncStarted(handler: (e: SyncStarted) => void): Promise<UnlistenFn> {
  return listen<SyncStarted>("sync-started", (e) => handler(e.payload));
}

export function onSyncFinished(handler: (e: SyncFinished) => void): Promise<UnlistenFn> {
  return listen<SyncFinished>("sync-finished", (e) => handler(e.payload));
}

/** Device ids announcing sync mode on the LAN. */
export function onPresenceChanged(handler: (ids: string[]) => void): Promise<UnlistenFn> {
  return listen<string[]>("presence-changed", (e) => handler(e.payload));
}

export interface BleEstimate {
  deviceId: string;
  bytes: number;
  secs: number;
}

/** Sent before a Bluetooth sync transfers anything. */
export function onBleEstimate(handler: (e: BleEstimate) => void): Promise<UnlistenFn> {
  return listen<BleEstimate>("ble-sync-estimate", (e) => handler(e.payload));
}

/** An agent wrote or deleted an attribute. */
export function onAttributesChanged(handler: () => void): Promise<UnlistenFn> {
  return listen("attributes-changed", () => handler());
}

export function errorText(e: unknown): string {
  return typeof e === "string" ? e : e instanceof Error ? e.message : JSON.stringify(e);
}
