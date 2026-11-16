import { useEffect, useState } from "react";
import { revealItemInDir } from "@tauri-apps/plugin-opener";
import { api, errorText, type AppSettings, type User } from "./api";

export function Settings({ user, onUser }: { user: User; onUser: (u: User) => void }) {
  const [path, setPath] = useState(user.libraryPath);
  const [message, setMessage] = useState("");
  const [error, setError] = useState("");
  const [settings, setSettings] = useState<AppSettings | null>(null);

  useEffect(() => {
    api.appSettings().then(setSettings).catch((e) => setError(errorText(e)));
  }, []);

  async function toggle(key: keyof AppSettings, value: boolean) {
    if (!settings) return;
    setError("");
    try {
      if (key === "autoSync") {
        await api.setAutoSync(value);
        onUser({ ...user, autoSync: value });
      } else if (key === "stayLoggedIn") {
        await api.setStayLoggedIn(value);
      } else {
        await api.setStartAtLogin(value);
      }
      setSettings({ ...settings, [key]: value });
    } catch (e) {
      setError(errorText(e));
    }
  }

  async function save(e: React.FormEvent) {
    e.preventDefault();
    setError("");
    setMessage("");
    try {
      onUser(await api.setLibraryPath(path));
      setMessage("Saved. New syncs go to this folder; existing files stay where they are.");
    } catch (err) {
      setError(errorText(err));
    }
  }

  return (
    <section className="page">
      <header className="page-head">
        <div>
          <h2>Settings</h2>
          <p className="muted">Signed in as {user.username}</p>
        </div>
      </header>
      <form className="settings-form" onSubmit={save}>
        <label>
          Library folder
          <input value={path} onChange={(e) => setPath(e.target.value)} />
        </label>
        <div className="row">
          <button className="primary" disabled={path.trim() === user.libraryPath}>
            Save
          </button>
          <button type="button" onClick={() => revealItemInDir(user.libraryPath).catch((e) => setError(errorText(e)))}>
            Open folder
          </button>
        </div>
        {message && <div className="note">{message}</div>}
        {error && <div className="error">{error}</div>}
      </form>
      {settings && (
        <div className="toggles">
          <label className="toggle">
            <input type="checkbox" checked={settings.autoSync} onChange={(e) => toggle("autoSync", e.target.checked)} />
            <span>
              <b>Auto-sync</b>
              <span className="muted small">
                Pull new recordings whenever one of your devices is in sync mode on this network. While on, closing
                the window keeps the app in the system tray.
              </span>
            </span>
          </label>
          <label className="toggle">
            <input
              type="checkbox"
              checked={settings.stayLoggedIn}
              onChange={(e) => toggle("stayLoggedIn", e.target.checked)}
            />
            <span>
              <b>Stay logged in</b>
              <span className="muted small">
                Open straight into this account. Only for a computer nobody else uses: anyone who opens the app
                sees your recordings.
              </span>
            </span>
          </label>
          <label className="toggle">
            <input
              type="checkbox"
              checked={settings.startAtLogin}
              onChange={(e) => toggle("startAtLogin", e.target.checked)}
            />
            <span>
              <b>Start at login</b>
              <span className="muted small">
                Start in the tray when you sign in to this computer. Auto-sync then needs Stay logged in.
              </span>
            </span>
          </label>
        </div>
      )}
    </section>
  );
}
