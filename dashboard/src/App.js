import React, { useState, useEffect, useRef } from "react";
import {
  db,
  auth,
  ref,
  onValue,
  set,
  update,
  signInWithEmailAndPassword,
  signOut,
  onAuthStateChanged
} from "./firebase";
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  ReferenceLine,
  ResponsiveContainer,
  Legend
} from "recharts";
import "./App.css";

// Defaults - the live values come from Firebase /settings
const DEFAULT_SETTINGS = {
  fanOnTemp: 27,
  fanOffTemp: 26,
  darkThreshold: 1500,
  motionHoldSec: 10
};

// The ESP32 writes /status every 5 s. If nothing arrives for 20 s it is offline.
const ONLINE_TIMEOUT_MS = 20000;

const MODES = [
  { id: "auto", label: "Auto", hint: "Rule-based: thresholds, hysteresis and motion timeout." },
  { id: "ai", label: "AI", hint: "Decisions from the trained Decision Tree running on the ESP32." },
  { id: "manual", label: "Manual", hint: "You control the fan and light. Your choices are logged as training data." },
  { id: "off", label: "Off", hint: "Both appliances are forced off." }
];


// Demo test evidence. Each test checks the LIVE values the ESP32 reports.
// A test only passes when the data check passes AND the student confirms
// that the real fan / bulb did the same thing.
const TESTS = [
  { id: "T01", name: "Auto: bright room, no motion", setup: "Auto mode. Room lit, nobody in front of the PIR for longer than the hold time.", expect: "Light OFF",
    check: x => x.mode === "auto" && !x.dark && !x.motionRecent && x.light === false },
  { id: "T02", name: "Auto: dark + motion", setup: "Auto mode. Cover the LDR, then wave at the PIR.", expect: "Light ON",
    check: x => x.mode === "auto" && x.dark && x.motionRecent && x.light === true },
  { id: "T03", name: "Auto: dark, motion stops", setup: "Keep the LDR covered and stay out of the PIR's view for longer than the hold time.", expect: "Light OFF after the hold time",
    check: x => x.mode === "auto" && !x.motionRecent && x.light === false },
  { id: "T04", name: "Auto: warm (fan on)", setup: "Auto mode. Hold the warm-water cup near the DHT22 until it reaches the fan-on temperature.", expect: "Fan ON",
    check: (x, st) => x.mode === "auto" && x.temperature >= st.fanOnTemp && x.fan === true },
  { id: "T05", name: "Auto: cooled (fan off)", setup: "Remove the cup and wait until the temperature is at or below the fan-off temperature.", expect: "Fan OFF",
    check: (x, st) => x.mode === "auto" && x.temperature <= st.fanOffTemp && x.fan === false },
  { id: "T06", name: "Manual: light ON", setup: "Manual mode. Switch the light ON.", expect: "Bulb ON",
    check: x => x.mode === "manual" && x.reqLight === true && x.light === true },
  { id: "T07", name: "Manual: light OFF", setup: "Manual mode. Switch the light OFF.", expect: "Bulb OFF",
    check: x => x.mode === "manual" && x.reqLight === false && x.light === false },
  { id: "T08", name: "Manual: fan ON", setup: "Manual mode. Switch the fan ON.", expect: "Fan ON",
    check: x => x.mode === "manual" && x.reqFan === true && x.fan === true },
  { id: "T09", name: "Manual: fan OFF", setup: "Manual mode. Switch the fan OFF.", expect: "Fan OFF",
    check: x => x.mode === "manual" && x.reqFan === false && x.fan === false },
  { id: "T10", name: "Off mode", setup: "Click Off.", expect: "Fan OFF and bulb OFF",
    check: x => x.mode === "off" && x.light === false && x.fan === false },
  { id: "T11", name: "Wi-Fi lost, rules keep working", setup: "Auto mode. Turn the Wi-Fi/hotspot off for 1 minute and trigger the light (cover LDR + motion). Turn Wi-Fi back on, then record.", expect: "Bulb still switched while offline; green LED blinked",
    check: x => x.online && x.mode === "auto" },
  { id: "T12", name: "AI mode", setup: "Only after the model is trained and uploaded. Click AI.", expect: "Outputs follow the AI decision",
    check: x => x.mode === "ai" && x.modelTrained && (x.source === "ai" || x.source === "ai+rules") && x.light === x.aiLight }
];

const timestamp = () =>
  new Date().toLocaleTimeString("en-ZA", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit"
  });

const onOff = v => (v ? "ON" : "OFF");

function SensorCard({ icon, label, value, unit, state }) {
  return (
    <div className={`sensor-card ${state || ""}`}>
      <span className="s-icon" aria-hidden="true">{icon}</span>
      <p className="s-label">{label}</p>
      <p className="s-value">{value}</p>
      <p className="s-unit">{unit}</p>
    </div>
  );
}

function ApplianceRow({ icon, name, trigger, requested, actual, onChange, disabled, waiting }) {
  return (
    <div className="appliance-row">
      <span className="app-icon" aria-hidden="true">{icon}</span>

      <div className="app-info">
        <p className="app-name">{name}</p>
        <p className="app-trigger">{trigger}</p>
        {waiting && <p className="app-wait">Waiting for ESP32 to confirm…</p>}
      </div>

      <span className={`app-status ${actual ? "on" : ""}`} title="Actual relay state reported by the ESP32">
        {actual == null ? "—" : onOff(actual)}
      </span>

      <label className="toggle" aria-label={`Switch ${name}`}>
        <input
          type="checkbox"
          checked={!!requested}
          onChange={e => onChange(e.target.checked)}
          disabled={disabled}
        />
        <span className="slider" />
      </label>
    </div>
  );
}

function DecisionRow({ name, rule, ai, prob, actual, trained }) {
  return (
    <div className="decision-row">
      <span className="d-name">{name}</span>
      <span className={`pill ${rule ? "pill-on" : ""}`}>Rules: {onOff(rule)}</span>
      <span className={`pill ${trained && prob >= 0 ? (ai ? "pill-on" : "") : "pill-na"}`}>
        AI: {trained && prob >= 0 ? `${onOff(ai)} (${Math.round(prob * 100)}%)` : "n/a"}
      </span>
      <span className={`pill ${actual ? "pill-on" : ""}`}>Actual: {actual == null ? "—" : onOff(actual)}</span>
    </div>
  );
}

function LogEntry({ color, message, time }) {
  return (
    <div className="log-entry">
      <span className="log-dot" style={{ background: color }} />
      <span className="log-msg">{message}</span>
      <span className="log-time">{time}</span>
    </div>
  );
}

export default function App() {
  const [user, setUser] = useState(null);
  const [authChecked, setAuthChecked] = useState(false);
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [loginError, setLoginError] = useState("");
  const [loginLoading, setLoginLoading] = useState(false);

  const [sensors, setSensors] = useState({});
  const [status, setStatus] = useState({});
  const [control, setControl] = useState({ mode: "auto", fan: false, light: false });
  const [settings, setSettings] = useState(DEFAULT_SETTINGS);
  const [settingsDraft, setSettingsDraft] = useState(DEFAULT_SETTINGS);
  const [serverOffset, setServerOffset] = useState(0);
  const [nowTick, setNowTick] = useState(Date.now());

  const [history, setHistory] = useState([]);
  const [log, setLog] = useState([]);
  const [tests, setTests] = useState({});
  const [seen, setSeen] = useState({});
  const previousStatus = useRef(null);

  function addLog(color, message) {
    setLog(previous => [{ color, message, time: timestamp() }, ...previous].slice(0, 12));
  }

  // ---- authentication ----
  useEffect(() => {
    const unsubscribe = onAuthStateChanged(auth, currentUser => {
      setUser(currentUser);
      setAuthChecked(true);
    });
    return unsubscribe;
  }, []);

  async function handleLogin(event) {
    event.preventDefault();
    setLoginError("");
    setLoginLoading(true);
    try {
      await signInWithEmailAndPassword(auth, email.trim(), password);
      setPassword("");
    } catch (error) {
      setLoginError(
        error.code === "auth/invalid-credential"
          ? "Email or password is incorrect."
          : error.message
      );
    } finally {
      setLoginLoading(false);
    }
  }

  async function handleLogout() {
    try {
      await signOut(auth);
      setHistory([]);
      setLog([]);
    } catch (error) {
      setLoginError(`Sign-out failed: ${error.message}`);
    }
  }

  // ---- clock tick for the "ESP32 online" check ----
  useEffect(() => {
    const id = setInterval(() => setNowTick(Date.now()), 2000);
    return () => clearInterval(id);
  }, []);

  // ---- Firebase listeners ----
  useEffect(() => {
    if (!user) return;

    const fail = what => error => addLog("#e34948", `${what} read failed: ${error.message}`);

    const unsubs = [
      onValue(ref(db, "sensors"), snap => setSensors(snap.val() || {}), fail("Sensor")),
      onValue(ref(db, "status"), snap => setStatus(snap.val() || {}), fail("Status")),
      onValue(ref(db, "control"), snap => {
        const data = snap.val() || {};
        setControl(prev => ({
          mode: typeof data.mode === "string" ? data.mode : prev.mode,
          fan: typeof data.fan === "boolean" ? data.fan : false,
          light: typeof data.light === "boolean" ? data.light : false
        }));
      }, fail("Control")),
      onValue(ref(db, "settings"), snap => {
        const merged = { ...DEFAULT_SETTINGS, ...(snap.val() || {}) };
        setSettings(merged);
        setSettingsDraft(merged);
      }, fail("Settings")),
      onValue(ref(db, ".info/serverTimeOffset"), snap => setServerOffset(snap.val() || 0)),
      onValue(ref(db, "tests"), snap => setTests(snap.val() || {}), fail("Tests"))
    ];

    return () => unsubs.forEach(u => u());
  }, [user]);

  // ---- chart history ----
  useEffect(() => {
    if (sensors.temperature == null || sensors.humidity == null) return;
    setHistory(previous => [
      ...previous,
      {
        time: timestamp(),
        temp: Number(Number(sensors.temperature).toFixed(1)),
        hum: Number(Number(sensors.humidity).toFixed(1))
      }
    ].slice(-40));
  }, [sensors.temperature, sensors.humidity]);

  // ---- event log: report what the ESP32 actually did ----
  useEffect(() => {
    const prev = previousStatus.current;
    if (prev && Object.keys(prev).length && status && Object.keys(status).length) {
      if (prev.light !== status.light && status.light != null) {
        addLog(status.light ? "#f5a524" : "#7a7974",
          `ESP32: light ${onOff(status.light)} (${status.source || "?"})`);
      }
      if (prev.fan !== status.fan && status.fan != null) {
        addLog(status.fan ? "#2a78d6" : "#7a7974",
          `ESP32: fan ${onOff(status.fan)} (${status.source || "?"})`);
      }
      if (!prev.failsafe && status.failsafe) {
        addLog("#e34948", "ESP32 lost contact in Manual mode - switched to rules (failsafe)");
      }
      if (!prev.gasAlarm && status.gasAlarm) {
        addLog("#e34948", "GAS ALARM - buzzer active");
      }
    }
    previousStatus.current = status;
  }, [status]);

  // ---- actions (each writes ONLY the fields it owns) ----
  async function changeMode(newMode) {
    if (!user) return;
    try {
      if (newMode === "off") {
        await update(ref(db, "control"), { mode: "off", fan: false, light: false });
      } else if (newMode === "manual") {
        // Start Manual from what is physically on now, so nothing jumps.
        await update(ref(db, "control"), {
          mode: "manual",
          fan: status.fan === true,
          light: status.light === true
        });
      } else {
        await set(ref(db, "control/mode"), newMode);
      }
      addLog("#378ADD", `Mode changed to ${newMode}`);
    } catch (error) {
      addLog("#e34948", `Could not change mode: ${error.message}`);
    }
  }

  async function manualToggle(device, value) {
    try {
      await set(ref(db, `control/${device}`), value);
      addLog("#378ADD", `Manual: ${device} ${onOff(value)}`);
    } catch (error) {
      addLog("#e34948", `Could not update ${device}: ${error.message}`);
    }
  }


  async function recordTest(test, snapshot) {
    const physical = seen[test.id];
    if (physical !== "yes" && physical !== "no") {
      addLog("#e34948", `${test.id}: choose whether you saw the real fan/bulb respond first.`);
      return;
    }
    const dataPass = !!test.check(snapshot, settings);
    const row = {
      id: test.id, name: test.name, expect: test.expect,
      recordedAt: Date.now() + serverOffset,
      seq: snapshot.seq ?? null,
      mode: snapshot.mode ?? null, source: snapshot.source ?? null,
      temperature: snapshot.temperature ?? null, humidity: snapshot.humidity ?? null,
      lightLevel: snapshot.lightLevel ?? null, ambientLight: snapshot.ambientLight ?? null,
      dark: !!snapshot.dark, motionRecent: !!snapshot.motionRecent,
      light: !!snapshot.light, fan: !!snapshot.fan,
      dataPass, physical,
      result: dataPass && physical === "yes" ? "PASS" : "FAIL"
    };
    try {
      await set(ref(db, `tests/${test.id}`), row);
      addLog(row.result === "PASS" ? "#0ca30c" : "#e34948", `${test.id} recorded: ${row.result} (reading #${row.seq})`);
    } catch (error) {
      addLog("#e34948", `Could not record ${test.id}: ${error.message}`);
    }
  }

  async function saveSettings(event) {
    event.preventDefault();
    const s = {
      fanOnTemp: Number(settingsDraft.fanOnTemp),
      fanOffTemp: Number(settingsDraft.fanOffTemp),
      darkThreshold: Math.round(Number(settingsDraft.darkThreshold)),
      motionHoldSec: Math.round(Number(settingsDraft.motionHoldSec))
    };
    if (!(s.fanOnTemp > s.fanOffTemp)) {
      addLog("#e34948", "Fan ON temperature must be higher than fan OFF temperature.");
      return;
    }
    if (!(s.darkThreshold > 0 && s.darkThreshold < 4095) || !(s.motionHoldSec >= 2 && s.motionHoldSec <= 3600)) {
      addLog("#e34948", "Dark threshold must be 1-4094 and motion hold 2-3600 s.");
      return;
    }
    try {
      await update(ref(db, "settings"), s);
      addLog("#378ADD", "Settings saved - ESP32 picks them up within 30 s");
    } catch (error) {
      addLog("#e34948", `Could not save settings: ${error.message}`);
    }
  }

  // ---- screens ----
  if (!authChecked) {
    return <div className="app"><p className="center-msg">Checking sign-in…</p></div>;
  }

  if (!user) {
    return (
      <div className="app">
        <div className="login-card">
          <div className="logo-badge">DES371S</div>
          <h1>AI Smart Home Automation</h1>
          <p className="login-sub">Sign in to monitor and control your smart home.</p>

          <form onSubmit={handleLogin} className="login-form">
            <label htmlFor="email">Email address</label>
            <input id="email" type="email" value={email}
              onChange={e => setEmail(e.target.value)} autoComplete="username" required />

            <label htmlFor="password">Password</label>
            <input id="password" type="password" value={password}
              onChange={e => setPassword(e.target.value)} autoComplete="current-password" required />

            {loginError && <p className="login-error" role="alert">{loginError}</p>}

            <button type="submit" className="btn-primary" disabled={loginLoading}>
              {loginLoading ? "Signing in…" : "Sign in"}
            </button>
          </form>
        </div>
      </div>
    );
  }

  const lastSeen = Number(status.lastSeen) || 0;
  const espOnline = lastSeen > 0 && nowTick + serverOffset - lastSeen < ONLINE_TIMEOUT_MS;
  const secondsAgo = lastSeen ? Math.max(0, Math.round((nowTick + serverOffset - lastSeen) / 1000)) : null;

  const mode = control.mode;
  const isManual = mode === "manual";
  const temp = sensors.temperature == null ? null : Number(sensors.temperature);
  const hum = sensors.humidity == null ? null : Number(sensors.humidity);
  const modelTrained = status.modelTrained === true;

  const tempState = temp == null ? "" : temp >= settings.fanOnTemp ? "alert" : "";
  const dark = status.dark === true;

  const snapshot = {
    seq: status.seq, mode: status.mode, source: status.source,
    temperature: temp, humidity: hum, lightLevel: sensors.light, ambientLight: sensors.ambientLight,
    dark, motionRecent: sensors.motionRecent === true,
    light: status.light === true, fan: status.fan === true,
    aiLight: status.aiLight === true, aiFan: status.aiFan === true,
    reqLight: control.light === true, reqFan: control.fan === true,
    modelTrained, online: espOnline
  };

  return (
    <div className="app">
      <header className="header">
        <div className="header-left">
          <div className="logo-badge">DES371S</div>
          <div>
            <h1>AI Smart Home Automation</h1>
            <p className="header-sub">ESP32 · Firebase · Decision Tree</p>
          </div>
        </div>

        <div className="conn-badge" role="status">
          <span className={`conn-dot ${espOnline ? "live" : ""}`} />
          <span>
            {espOnline
              ? `ESP32 online · reading #${status.seq ?? "—"}`
              : secondsAgo == null
              ? "Waiting for ESP32…"
              : `ESP32 offline (last seen ${secondsAgo}s ago)`}
          </span>
        </div>
      </header>

      <main className="main">
        {status.gasAlarm && (
          <div className="banner banner-alert" role="alert">⚠️ Gas / smoke detected - buzzer active</div>
        )}
        {status.failsafe && (
          <div className="banner" role="alert">
            ESP32 lost contact with the dashboard in Manual mode and is using the rules (failsafe).
          </div>
        )}

        <section className="section">
          <p className="section-label">Operating mode</p>
          <div className="mode-bar">
            {MODES.map(m => (
              <button key={m.id}
                className={`mode-btn ${mode === m.id ? "active" : ""}`}
                onClick={() => changeMode(m.id)}
                aria-pressed={mode === m.id}>
                {m.label}
              </button>
            ))}
          </div>
          <p className="mode-hint">
            {MODES.find(m => m.id === mode)?.hint}
            {mode === "ai" && !modelTrained && " No model is trained yet, so the ESP32 is using the rules."}
          </p>
        </section>

        <section className="section">
          <p className="section-label">Live sensor readings</p>
          <div className="sensor-grid">
            <SensorCard icon="🌡" label="Temperature"
              value={temp == null ? "—" : temp.toFixed(1)}
              unit={`°C · fan on ≥ ${settings.fanOnTemp}°C, off ≤ ${settings.fanOffTemp}°C`}
              state={tempState} />
            <SensorCard icon="💧" label="Humidity"
              value={hum == null ? "—" : hum.toFixed(1)}
              unit="% relative humidity" />
            <SensorCard icon={dark ? "🌙" : "☀️"} label="Light level"
              value={sensors.light == null ? "—" : sensors.light}
              unit={`ADC · ambient ${sensors.ambientLight ?? "—"} · ${dark ? "dark" : "bright"} (dark < ${settings.darkThreshold})`} />
            <SensorCard icon="🚶" label="Motion"
              value={sensors.motion == null ? "—" : sensors.motion ? "Detected" : "None"}
              unit={sensors.motionRecent ? `Room occupied (hold ${settings.motionHoldSec}s)` : "Room empty"}
              state={sensors.motion ? "ok" : ""} />
            {sensors.gas != null && (
              <SensorCard icon="🔥" label="Gas (MQ-2)" value={sensors.gas}
                unit="raw ADC" state={status.gasAlarm ? "alert" : ""} />
            )}
          </div>
        </section>

        <section className="section">
          <p className="section-label">Appliances</p>
          <div className="appliance-card">
            <ApplianceRow icon="🌀" name="Fan (12 V)"
              trigger={isManual ? "Manual control" : `Controlled by ${mode === "off" ? "Off mode" : mode === "ai" ? "AI" : "rules"}`}
              requested={isManual ? control.fan : status.fan}
              actual={status.fan}
              waiting={isManual && espOnline && status.fan !== control.fan}
              disabled={!isManual}
              onChange={v => manualToggle("fan", v)} />
            <ApplianceRow icon="💡" name="Light (12 V bulb)"
              trigger={isManual ? "Manual control" : `Controlled by ${mode === "off" ? "Off mode" : mode === "ai" ? "AI" : "rules (motion + dark)"}`}
              requested={isManual ? control.light : status.light}
              actual={status.light}
              waiting={isManual && espOnline && status.light !== control.light}
              disabled={!isManual}
              onChange={v => manualToggle("light", v)} />
          </div>
        </section>

        <section className="section">
          <p className="section-label">AI vs rules</p>
          <div className="ai-card">
            <div className="ai-head">
              <span className={`ai-badge ${modelTrained ? "ready" : ""}`}>
                {modelTrained ? "Model loaded" : "Model not trained yet"}
              </span>
              <span className="ai-version">
                {modelTrained ? `Version ${status.modelVersion}` : "Collect data in Manual mode, then run train_model.py"}
              </span>
            </div>
            <DecisionRow name="Light" rule={status.ruleLight} ai={status.aiLight}
              prob={status.aiLightProb} actual={status.light} trained={modelTrained} />
            <DecisionRow name="Fan" rule={status.ruleFan} ai={status.aiFan}
              prob={status.aiFanProb} actual={status.fan} trained={modelTrained} />
            <p className="ai-note">
              Decision source now: <strong>{status.source || "—"}</strong>.
              Both methods are calculated on the ESP32 all the time so they can be compared.
            </p>
          </div>
        </section>

        <section className="section">
          <p className="section-label">Demo test evidence</p>
          <div className="test-card">
            <p className="test-intro">
              Set up each test, check the real fan or bulb, then press Record. Results are saved in
              Firebase under <code>tests</code> with the reading number, which also appears in the
              Serial Monitor as <code>SENSOR DATA #{status.seq ?? "N"}</code>.
            </p>
            <p className="test-summary">
              Recorded: {Object.keys(tests).length} / {TESTS.length} ·
              PASS: {Object.values(tests).filter(t => t.result === "PASS").length} ·
              FAIL: {Object.values(tests).filter(t => t.result === "FAIL").length}
            </p>
            {TESTS.map(test => {
              const live = !!test.check(snapshot, settings);
              const rec = tests[test.id];
              return (
                <div className="test-row" key={test.id}>
                  <div className="test-main">
                    <p className="test-name"><span className="test-id">{test.id}</span>{test.name}</p>
                    <p className="test-setup">{test.setup} <b>Expected: {test.expect}.</b></p>
                    {rec && (
                      <p className={`test-rec ${rec.result === "PASS" ? "pass" : "fail"}`}>
                        Recorded {rec.result} · reading #{rec.seq} · {new Date(rec.recordedAt).toLocaleString("en-ZA")} ·
                        {" "}{rec.temperature}°C · light {rec.lightLevel} · light {rec.light ? "ON" : "OFF"} · fan {rec.fan ? "ON" : "OFF"}
                      </p>
                    )}
                  </div>
                  <div className="test-actions">
                    <span className={`pill ${live ? "pill-on" : ""}`}>Data now: {live ? "PASS" : "not yet"}</span>
                    <select
                      aria-label={`Did the real device respond for ${test.id}?`}
                      value={seen[test.id] || ""}
                      onChange={e => setSeen({ ...seen, [test.id]: e.target.value })}>
                      <option value="">Saw it physically?</option>
                      <option value="yes">Yes, it responded</option>
                      <option value="no">No</option>
                    </select>
                    <button type="button" className="btn-primary btn-small"
                      disabled={!espOnline}
                      onClick={() => recordTest(test, snapshot)}>Record</button>
                  </div>
                </div>
              );
            })}
          </div>
        </section>

        <section className="section">
          <p className="section-label">Temperature &amp; humidity history</p>
          <div className="chart-card">
            <ResponsiveContainer width="100%" height={220}>
              <LineChart data={history} margin={{ top: 8, right: 8, left: -12, bottom: 0 }}>
                <CartesianGrid strokeDasharray="3 3" stroke="#e8e7e1" />
                <XAxis dataKey="time" tick={{ fontSize: 10, fill: "#898781" }} interval="preserveStartEnd" />
                <YAxis yAxisId="t" domain={["auto", "auto"]} tick={{ fontSize: 10, fill: "#898781" }}
                  tickFormatter={v => `${v}°`} />
                <YAxis yAxisId="h" orientation="right" domain={[0, 100]} tick={{ fontSize: 10, fill: "#898781" }}
                  tickFormatter={v => `${v}%`} />
                <Tooltip contentStyle={{ fontSize: 12, borderRadius: 8, border: "0.5px solid #e0dfd8" }}
                  formatter={(v, n) => [n === "temp" ? `${v}°C` : `${v}%`, n === "temp" ? "Temperature" : "Humidity"]} />
                <ReferenceLine yAxisId="t" y={settings.fanOnTemp} stroke="#e34948" strokeDasharray="4 3"
                  label={{ value: `fan on ${settings.fanOnTemp}°C`, fill: "#e34948", fontSize: 10, position: "insideTopLeft" }} />
                <Line yAxisId="t" type="monotone" dataKey="temp" stroke="#2a78d6" strokeWidth={2} dot={false} name="temp" isAnimationActive={false} />
                <Line yAxisId="h" type="monotone" dataKey="hum" stroke="#1baf7a" strokeWidth={2} dot={false} name="hum" isAnimationActive={false} />
                <Legend wrapperStyle={{ fontSize: 11, paddingTop: 8 }}
                  formatter={v => (v === "temp" ? "Temperature °C" : "Humidity %")} />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </section>

        <section className="section">
          <details className="settings-card">
            <summary>Automation settings</summary>
            <form onSubmit={saveSettings} className="settings-form">
              <label>Fan ON at (°C)
                <input type="number" step="0.1" value={settingsDraft.fanOnTemp}
                  onChange={e => setSettingsDraft({ ...settingsDraft, fanOnTemp: e.target.value })} />
              </label>
              <label>Fan OFF at (°C)
                <input type="number" step="0.1" value={settingsDraft.fanOffTemp}
                  onChange={e => setSettingsDraft({ ...settingsDraft, fanOffTemp: e.target.value })} />
              </label>
              <label>Dark below (LDR ADC)
                <input type="number" step="1" value={settingsDraft.darkThreshold}
                  onChange={e => setSettingsDraft({ ...settingsDraft, darkThreshold: e.target.value })} />
              </label>
              <label>Light hold after motion (s)
                <input type="number" step="1" value={settingsDraft.motionHoldSec}
                  onChange={e => setSettingsDraft({ ...settingsDraft, motionHoldSec: e.target.value })} />
              </label>
              <button type="submit" className="btn-primary">Save settings</button>
            </form>
          </details>
        </section>

        <section className="section">
          <p className="section-label">Event log</p>
          <div className="log-card">
            {log.length === 0 ? (
              <p className="log-empty">Waiting for events…</p>
            ) : (
              log.map((entry, index) => <LogEntry key={`${entry.time}-${index}`} {...entry} />)
            )}
          </div>
        </section>

        <button type="button" className="btn-secondary" onClick={handleLogout}>Sign out</button>
      </main>
    </div>
  );
}
