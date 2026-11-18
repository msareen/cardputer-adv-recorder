import { useEffect, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { api, errorText, onModelDownloadProgress, type TranscribeSettings } from "./api";

/** Settings > local speech-to-text (sherpa-onnx Whisper). */
export function Transcription() {
  const [settings, setSettings] = useState<TranscribeSettings | null>(null);
  const [modelDir, setModelDir] = useState("");
  const [language, setLanguage] = useState("en");
  const [message, setMessage] = useState("");
  const [error, setError] = useState("");
  const [download, setDownload] = useState<{ bytes: number; total: number | null } | null>(null);

  useEffect(() => {
    api
      .transcribeSettings()
      .then((s) => {
        setSettings(s);
        setModelDir(s.modelDir);
        setLanguage(s.language);
      })
      .catch((e) => setError(errorText(e)));
  }, []);

  async function save(auto: boolean, dir = modelDir) {
    setError("");
    setMessage("");
    try {
      const s = await api.setTranscribeSettings(dir, language, auto);
      setSettings(s);
      setMessage(s.ready ? "Saved." : "Saved. Choose a model folder to start transcribing.");
    } catch (e) {
      setError(errorText(e));
    }
  }

  async function choose() {
    const picked = await open({ directory: true, title: "Choose a sherpa-onnx Whisper model folder" });
    if (typeof picked === "string") {
      setModelDir(picked);
      await save(settings?.auto ?? true, picked);
    }
  }

  async function downloadModel() {
    setError("");
    setMessage("");
    setDownload({ bytes: 0, total: null });
    const unlisten = onModelDownloadProgress((bytes, total) => setDownload({ bytes, total }));
    try {
      const s = await api.downloadDefaultModel();
      setSettings(s);
      setModelDir(s.modelDir);
      setMessage("Model ready. New recordings will be transcribed after each sync.");
    } catch (e) {
      setError(errorText(e));
    } finally {
      (await unlisten)();
      setDownload(null);
    }
  }

  async function transcribeAll() {
    setError("");
    setMessage("");
    try {
      const n = await api.transcribeMissing();
      setMessage(n === 0 ? "Every recording already has a transcript." : `Transcribing ${n} recording${n === 1 ? "" : "s"}.`);
    } catch (e) {
      setError(errorText(e));
    }
  }

  if (!settings) return error ? <div className="error">{error}</div> : null;
  return (
    <div className="settings-form">
      <h3>Transcription</h3>
      <p className="muted small">
        Speech-to-text runs on this computer with a Whisper model (sherpa-onnx); audio never leaves it. Download a
        model such as <code>sherpa-onnx-whisper-base.en</code> from the sherpa-onnx "asr-models" releases, unpack it,
        and choose its folder.
      </p>
      {!settings.ready && (
        <div className="row">
          <button className="primary" type="button" disabled={download !== null} onClick={downloadModel}>
            Download the default model (Whisper tiny.en, about 110 MB)
          </button>
          {download && (
            <span className="muted small">
              {download.total
                ? `${Math.round((download.bytes / download.total) * 100)}%`
                : `${Math.round(download.bytes / 1e6)} MB`}
            </span>
          )}
        </div>
      )}
      <label>
        Model folder
        <div className="row">
          <input value={modelDir} onChange={(e) => setModelDir(e.target.value)} placeholder="Not set" />
          <button type="button" onClick={choose}>
            Choose…
          </button>
        </div>
      </label>
      <label>
        Language
        <input value={language} onChange={(e) => setLanguage(e.target.value)} placeholder="en" size={6} />
        <span className="muted small">Whisper language code (en, hi, es…). English-only models ignore it.</span>
      </label>
      <label className="toggle">
        <input type="checkbox" checked={settings.auto} onChange={(e) => save(e.target.checked)} />
        <span>
          <b>Transcribe new recordings automatically</b>
          <span className="muted small">As soon as a sync brings a recording in, it gets a transcript.</span>
        </span>
      </label>
      <div className="row">
        <button className="primary" type="button" onClick={() => save(settings.auto)}>
          Save
        </button>
        <button type="button" disabled={!settings.ready} onClick={transcribeAll}>
          Transcribe all without a transcript
        </button>
      </div>
      {message && <div className="note">{message}</div>}
      {error && <div className="error">{error}</div>}
    </div>
  );
}
