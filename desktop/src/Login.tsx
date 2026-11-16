import { useState } from "react";
import { api, errorText, type User } from "./api";

export function Login({ onLoggedIn }: { onLoggedIn: (u: User) => void }) {
  const [mode, setMode] = useState<"login" | "create">("login");
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [confirm, setConfirm] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);

  async function submit(e: React.FormEvent) {
    e.preventDefault();
    setError("");
    if (mode === "create" && password !== confirm) {
      setError("Passwords do not match.");
      return;
    }
    setBusy(true);
    try {
      const user =
        mode === "login" ? await api.login(username, password) : await api.createAccount(username, password);
      onLoggedIn(user);
    } catch (err) {
      setError(errorText(err));
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className="login">
      <form className="login-card" onSubmit={submit}>
        <div className="brand">
          <span className="brand-dot" />
          Cardputer Sync
        </div>
        <h1>{mode === "login" ? "Log in" : "Create account"}</h1>
        <p className="muted">
          {mode === "login"
            ? "Your recordings and devices belong to your local account."
            : "Accounts live on this computer only. Each one has its own library and devices."}
        </p>
        <label>
          Username
          <input autoFocus value={username} onChange={(e) => setUsername(e.target.value)} autoComplete="username" />
        </label>
        <label>
          Password
          <input
            type="password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            autoComplete={mode === "login" ? "current-password" : "new-password"}
          />
        </label>
        {mode === "create" && (
          <label>
            Confirm password
            <input type="password" value={confirm} onChange={(e) => setConfirm(e.target.value)} autoComplete="new-password" />
          </label>
        )}
        {error && <div className="error">{error}</div>}
        <button className="primary" disabled={busy || !username || !password}>
          {busy ? "…" : mode === "login" ? "Log in" : "Create account"}
        </button>
        <button
          type="button"
          className="link"
          onClick={() => {
            setMode(mode === "login" ? "create" : "login");
            setError("");
          }}
        >
          {mode === "login" ? "New here? Create an account" : "Have an account? Log in"}
        </button>
      </form>
    </div>
  );
}
