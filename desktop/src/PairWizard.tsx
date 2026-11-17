import { useEffect, useState } from "react";
import { api, errorText, type NearbyDevice, type PairOutcome } from "./api";

export type PairMode = { kind: "new" } | { kind: "wifi"; deviceId: string; name: string };

type Step =
  | { name: "intro" }
  | { name: "scanning" }
  | { name: "pick"; devices: NearbyDevice[] }
  | { name: "connecting" }
  | { name: "compare"; code: string }
  | { name: "waitDevice" }
  | { name: "wifi"; networks: string[] }
  | { name: "sending" }
  | { name: "done"; outcome: PairOutcome }
  | { name: "error"; message: string };

interface Props {
  mode: PairMode;
  onClose: () => void;
  onPaired: () => void;
}

/** Add device (scan, pick, compare code, choose WiFi, done) and Change WiFi. */
export function PairWizard({ mode, onClose, onPaired }: Props) {
  const [step, setStep] = useState<Step>({ name: "intro" });
  const [ssid, setSsid] = useState("");
  const [otherSsid, setOtherSsid] = useState("");
  const [password, setPassword] = useState("");
  const changingWifi = mode.kind === "wifi";

  // Leave no BLE connection open if the wizard closes mid-way.
  useEffect(() => {
    return () => {
      api.pairCancel();
    };
  }, []);

  function fail(e: unknown) {
    api.pairCancel();
    setStep({ name: "error", message: errorText(e) });
  }

  async function scan() {
    setStep({ name: "scanning" });
    try {
      // A Cardputer in Bluetooth sync mode only talks to its paired desktop.
      let devices = (await api.bleScan()).filter((d) => d.pairing);
      if (mode.kind === "wifi") devices = devices.filter((d) => d.deviceId === mode.deviceId);
      setStep({ name: "pick", devices });
    } catch (e) {
      fail(e);
    }
  }

  async function connect(device: NearbyDevice) {
    setStep({ name: "connecting" });
    try {
      const start = await api.pairStart(device.deviceId);
      if (start.known) {
        await waitForDevice();
      } else if (changingWifi) {
        fail("This device is paired with another desktop. Pair it again instead.");
      } else {
        setStep({ name: "compare", code: start.code ?? "" });
      }
    } catch (e) {
      fail(e);
    }
  }

  async function waitForDevice() {
    setStep({ name: "waitDevice" });
    try {
      const networks = await api.pairReady();
      setSsid(networks[0] ?? "");
      setStep({ name: "wifi", networks });
    } catch (e) {
      fail(e);
    }
  }

  async function finish(e: React.FormEvent) {
    e.preventDefault();
    const chosen = ssid === "__other" ? otherSsid.trim() : ssid;
    setStep({ name: "sending" });
    try {
      const outcome = await api.pairFinish(chosen, password);
      setPassword("");
      setStep({ name: "done", outcome });
      onPaired();
    } catch (err) {
      fail(err);
    }
  }

  function cancel() {
    api.pairCancel();
    onClose();
  }

  const title = changingWifi ? `Change WiFi for ${mode.name}` : "Pair a Cardputer";

  return (
    <div className="wizard">
      <div className="wizard-head">
        <h3>{title}</h3>
        <button className="link" onClick={cancel}>
          Close
        </button>
      </div>

      {step.name === "intro" && (
        <>
          <ol className="steps">
            <li>
              On the Cardputer: hold <b>G0</b>, open <b>Sync</b>, choose <b>Pair with Desktop</b>.
            </li>
            <li>Keep it near this computer; Bluetooth must be on here.</li>
          </ol>
          <button className="primary" onClick={scan}>
            Search
          </button>
        </>
      )}

      {step.name === "scanning" && <p className="muted">Searching for Cardputers in pairing mode…</p>}

      {step.name === "pick" && (
        <>
          {step.devices.length === 0 ? (
            <p className="muted">None found. Check the Cardputer shows “Pair with desktop”, then search again.</p>
          ) : (
            <div className="nearby">
              {step.devices.map((d) => (
                <button key={d.deviceId} className="nearby-item" onClick={() => connect(d)}>
                  <span>
                    <b>{d.name}</b> <span className="mono muted">{d.deviceId}</span>
                  </span>
                  <span className="muted small">
                    {d.hasPeer ? "paired before" : "new"}
                    {d.rssi != null && ` · ${d.rssi} dBm`}
                  </span>
                </button>
              ))}
            </div>
          )}
          <button onClick={scan}>Search again</button>
        </>
      )}

      {step.name === "connecting" && <p className="muted">Connecting over Bluetooth…</p>}

      {step.name === "compare" && (
        <>
          <p>Does the Cardputer show this code?</p>
          <div className="code">
            {step.code.slice(0, 3)} {step.code.slice(3)}
          </div>
          <p className="muted small">
            If the codes differ, someone may be interfering. Cancel and try again somewhere else.
          </p>
          <div className="row">
            <button className="primary" onClick={waitForDevice}>
              Codes match
            </button>
            <button onClick={cancel}>They differ</button>
          </div>
        </>
      )}

      {step.name === "waitDevice" && (
        <p>{changingWifi ? "Connected. Loading networks…" : "Now press Enter on the Cardputer to confirm."}</p>
      )}

      {step.name === "wifi" && (
        <form className="wifi-form" onSubmit={finish}>
          <p className="muted">The Cardputer will join this network to sync. It is sent encrypted over Bluetooth.</p>
          <label>
            Network
            <select value={ssid} onChange={(e) => setSsid(e.target.value)}>
              {step.networks.map((n) => (
                <option key={n} value={n}>
                  {n}
                </option>
              ))}
              <option value="__other">Other…</option>
            </select>
          </label>
          {(ssid === "__other" || step.networks.length === 0) && (
            <label>
              Network name
              <input
                value={otherSsid}
                onChange={(e) => {
                  setOtherSsid(e.target.value);
                  setSsid("__other");
                }}
              />
            </label>
          )}
          <label>
            Password
            <input type="password" value={password} onChange={(e) => setPassword(e.target.value)} autoFocus />
          </label>
          <button className="primary" disabled={ssid === "__other" ? !otherSsid.trim() : !ssid}>
            {changingWifi ? "Save WiFi" : "Finish pairing"}
          </button>
        </form>
      )}

      {step.name === "sending" && <p className="muted">The Cardputer is testing the network (up to 15 s)…</p>}

      {step.name === "done" && (
        <>
          <p className="note">{changingWifi ? "WiFi updated." : `Paired with ${step.outcome.device.name}.`}</p>
          <p className={step.outcome.wifi.ok ? "muted" : "error"}>
            {step.outcome.wifi.ok
              ? `It joined the network at ${step.outcome.wifi.ip}. Press S on the Cardputer, then Sync now.`
              : "It could not join that network. The pairing is kept; use Change WiFi to try again."}
          </p>
          <button className="primary" onClick={onClose}>
            Done
          </button>
        </>
      )}

      {step.name === "error" && (
        <>
          <div className="error">{step.message}</div>
          <div className="row">
            <button className="primary" onClick={() => setStep({ name: "intro" })}>
              Try again
            </button>
            <button onClick={onClose}>Close</button>
          </div>
        </>
      )}
    </div>
  );
}
