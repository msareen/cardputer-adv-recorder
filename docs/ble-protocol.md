# BLE pairing protocol (milestone 4) and sync fallback (milestone 6)

Detailed wire format for the Bluetooth part of the
[architecture doc](architecture/Cardputer-ADV-Recorder-Sync-Architecture.pdf).
Implemented in `firmware/src/app/pairing_flow.cpp` (device) and
`desktop/src-tauri/src/pairing.rs` (desktop). Both test suites check the same
vectors (`test_pairing` and `shared_vectors_with_firmware`).

## GATT service "Recorder Sync"

| | UUID | Properties |
| --- | --- | --- |
| Service | `6f1c0001-5b2e-4c8f-9a4d-3e7b1f2a9c00` | advertised |
| Info | `6f1c0002-…` | read, plain CBOR `{v, id, fw, paired}` |
| Control | `6f1c0003-…` | write (with response) + notify |

The device advertises in pairing mode (Settings > Sync > Pair with
Desktop, for at most two minutes) and in the sync fallback (below). The
scan response carries manufacturer data for company `0xFFFF`: `"CP"`, a
flags byte (bit 0 pairing mode, bit 1 already paired, bit 2 sync fallback),
and the 8-byte device id. The main advertisement holds only the service
UUID, so a desktop must scan without a service filter: Windows applies the
filter to each packet and would drop the scan response.

Not implemented: the Status characteristic, and advertising during WiFi
sync. The device turns Bluetooth off in WiFi sync mode to leave heap for
WiFi and TLS, and desktops find it by mDNS (`_cprec._tcp.local`, TXT `id`)
instead. There is no separate Data characteristic either: the sync fallback
carries file bytes in encrypted Control messages (below).

## Framing

Each write or notification is one fragment: byte 0 is `1` on the last
fragment, `0` otherwise; the rest is payload. Fragments reassemble into a
message of at most 2048 bytes. Byte 0 of a message selects its form:

- `0x00` plain: CBOR follows.
- `0x01` encrypted: `counter` (8 bytes, big endian), then AES-256-GCM
  ciphertext and 16-byte tag. The 9-byte header is the AAD. Nonce =
  direction (`'H'` host to device, `'D'` device to host), three zero bytes,
  counter. Counters start at 1 and must increase, so replays are dropped.

Messages are CBOR maps with text keys; `t` is the message type.

## Pairing (numeric comparison with a commitment)

```
host                                   device
 hello {pk_h, c}          ------->     c = SHA-256("cprec v1 commit" | pk_h | n_h)
                          <-------     hello {pk_d, n_d, known}
 reveal {n_h}             ------->     checks c
```

Plain swapping of nonces would let a man-in-the-middle try nonces until the
six-digit codes match. Committing to `n_h` first (as Bluetooth LE Secure
Connections does) fixes each side's code before the attacker sees the other
nonce.

Both sides then compute:

- shared = ECDH(P-256) x coordinate
- session key = HKDF-SHA256(salt = `n_h | n_d`, ikm = shared,
  info = `"cprec v1 session" | pk_h | pk_d`), 32 bytes
- code = first 4 bytes of SHA-256(`"cprec v1 code" | pk_h | pk_d | n_h | n_d`)
  as a big-endian integer, mod 1 000 000, six digits
- ids = first 16 hex chars of SHA-256(uncompressed public key)

Keys are 65-byte uncompressed P-256 points; nonces are 16 random bytes.

### New desktop (`known` = false)

1. Both screens show the code. The user clicks Codes match on the desktop
   and presses Enter on the Cardputer. Without the Enter press the device
   ignores `pair`, so a new owner always needs physical access.
2. device: `{t:"confirmed"}`, then `{t:"networks", list:[ssid…]}` (scanned
   when pairing mode opened).
3. host: `{t:"pair", name, cert, ssid, pass}`. `cert` is the host's
   self-signed X.509 certificate (DER); the device checks that it carries
   `pk_h`, stores peer and WiFi in NVS, replacing any previous peer and
   clearing its acks.
4. device tries the network once (15 s): `{t:"wifi", ok, ip}`.

Bluetooth only: the host sends `ssid` and `pass` as empty strings. The
device stores no network, skips the join and answers `{t:"wifi", ok:false,
ip:""}` at once; its `S` key then starts the BLE sync fallback directly.
The desktop records the device as `ble_only` and syncs it over BLE only.

### Known desktop (`known` = true): change WiFi

1. device: `{t:"session"}` (proves it holds its key and still pairs with
   this host), then `networks`.
2. host: `{t:"set_wifi", ssid, pass}`; device answers with `wifi` as above.
   An empty `ssid` erases the stored network (switch to Bluetooth only).

### Errors

The device may send plain `{t:"error", e}` at any point: `cancelled` (Esc),
`timeout` (two minutes), `commitment`, `bad hello`, `bad reveal`, `bad key`,
`bad certificate`, `bad wifi`, `storage`.

## After pairing

Sync uses the `/v1` protocol over HTTPS on port 8443 with mutual TLS. The
device serves a self-signed certificate for its key and accepts only the
stored host certificate; the desktop accepts only the device key it paired
with. Neither side checks certificate dates (the device clock may be unset).

Forget Pairing on the device wipes the peer, the WiFi credentials and the
acks. Removing a device in the desktop app forgets it on the desktop only.

## Sync fallback (milestone 6)

For when the device cannot reach WiFi. The device offers it when joining
WiFi fails or no network is set (Enter on the sync screen), and from
Settings > Sync > Sync over Bluetooth. It advertises with flags bit 2 and
accepts only its paired desktop; WiFi stays off. On the desktop: Devices >
Sync over Bluetooth.

The desktop opens the known-host session exactly as for changing WiFi
(hello / hello / reveal, then `{t:"session"}`; the device sends no
`networks`). A desktop that is not the peer gets a plain
`{t:"error", e:"not_paired"}` in place of the device's hello.

It then sends the `/v1` requests it would send over HTTPS, one at a time,
as encrypted Control messages:

```
host    {t:"req", m, p, since?, range?, body?}
device  {t:"res", s, ct, cr}           status, content type, Content-Range
device  {t:"body", d} ...              up to 1536 bytes each
device  {t:"end"}
```

`m` is the method, `p` the path (`/v1/manifest`), and `since`, `range` and
`body` carry the query value, the Range header and the JSON body as text.
Every response, errors included, ends with `end`; an error's `{code,
message}` JSON arrives in `body` messages. Bodies are batched to 1536 bytes,
so each message stays under the 2048-byte limit after CBOR and AES-GCM.

There is no app-level flow control: the device waits for free notification
buffers before each fragment (BLE delivers notifications in order), and the
desktop handles one request at a time. A broken link ends the sync; a
partial download resumes from its length (`range`) on the next attempt,
like over WiFi. Both ends share the request handling with the HTTPS server
(`SyncHandler` on the device, the `Device` trait on the desktop), so the
endpoints behave the same over both transports. Expect roughly 30 KB/s; the
desktop shows an estimate from the manifest before transferring.
