/**
 * @file AlarmPanel.jsx
 * @brief Alarm management for the sensor family selected on the dashboard: create form,
 *        alarm list with live state, and the fired/cleared event log.
 *
 * Everything in this section is limited to the family of the selected tab (CPU
 * temperatures, fans, battery, ...): the form only offers that family's sensors and
 * its suggested comparison and threshold, and only that family's alarms and events
 * are listed.
 */
import React, { useState } from 'react';
import { api } from './api.js';
import { categoryInfo, formatValue, unitInfo } from './units.js';
import { fmtTime } from './TempChart.jsx';

/** Aggregate functions an alarm can use. */
const FUNCS = ['avg', 'max', 'min', 'stddev'];
/** Comparisons an alarm can use: fire above or below the threshold. */
const OPS = [{ op: '>', label: 'above (>)' }, { op: '<', label: 'below (<)' }];
/**
 * Suggested comparison and threshold for each sensor family, used when the form
 * opens for a family (batteries alarm when charge drops, temperatures and fans when
 * they rise). Families without an entry start with "above" and an empty threshold.
 */
const SUGGEST = {
  temperature: { op: '>', threshold: 75 },
  fan: { op: '>', threshold: 4500 },
  battery: { op: '<', threshold: 20 },
};
/** Local 24-hour time of an epoch-ms timestamp (as on the charts), or a dash. */
const fmtTs = (ts) => (ts ? fmtTime(ts) : '—');
/** Local date and 24-hour time of an epoch-ms timestamp (event log). */
const fmtDateTime = (ts) => new Date(ts).toLocaleString([], { hour12: false });
/** Formats an alarm's aggregate; stddev is a spread in the sensor's unit, shown with more precision. */
const fmtVal = (v, func, unit) => formatValue(v, unit, func === 'stddev' ? 2 : undefined);

/**
 * Coloured badge for an alarm state.
 * Props:
 * - state: ok | firing | no_data | disabled
 */
export function StateBadge({ state }) {
  return <span className={`badge badge-${state}`}>{state.replace('_', ' ')}</span>;
}

/**
 * Create form for one sensor family. Target is one sensor of the family or every
 * sensor of it (e.g. every core); the backend creates one independent alarm per
 * target sensor. The parent remounts the form (React key) when the family changes,
 * so the target and the suggested comparison / threshold start fresh.
 * Props:
 * - category: The selected sensor family, e.g. "temperature". (type: string)
 * - sensors: Sensors of that family only. (type: Array)
 * - onCreated: Called after a successful create. (type: Function)
 * - onError: Called with an error message. (type: Function)
 */
function AlarmForm({ category, sensors, onCreated, onError }) {
  const suggestion = SUGGEST[category] || { op: '>', threshold: '' };
  const ci = categoryInfo(category, sensors);
  const [target, setTarget] = useState('all');
  const [func, setFunc] = useState('avg');
  const [windowMin, setWindowMin] = useState(10);
  const [threshold, setThreshold] = useState(suggestion.threshold);
  const [op, setOp] = useState(suggestion.op);
  const [busy, setBusy] = useState(false);

  // "all" = every sensor of the family; otherwise one sensor id.
  const targets = target === 'all' ? sensors : sensors.filter((s) => String(s.id) === target);
  const unit = targets[0]?.unit ?? sensors[0]?.unit;

  /** Creates the alarm(s) for the chosen target(s). */
  const submit = async (e) => {
    e.preventDefault();
    setBusy(true);
    try {
      await api.createAlarm({
        sensor_ids: targets.map((s) => s.id), func, window_min: Number(windowMin), threshold: Number(threshold), op,
      });
      onCreated();
    } catch (err) {
      onError(err.message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <form className="alarm-form" onSubmit={submit}>
      <label>
        Sensor
        <select value={target} onChange={(e) => setTarget(e.target.value)}>
          <option value="all">Every {ci.item.toLowerCase()} (one alarm each)</option>
          {sensors.map((s) => <option key={s.id} value={String(s.id)}>{s.label}</option>)}
        </select>
      </label>
      <label>
        Function
        <select value={func} onChange={(e) => setFunc(e.target.value)}>
          {FUNCS.map((f) => <option key={f}>{f}</option>)}
        </select>
      </label>
      <label>
        Window X (min)
        <input type="number" min="1" step="1" required value={windowMin}
          onChange={(e) => setWindowMin(e.target.value)} />
      </label>
      <label>
        Fires when
        <select value={op} onChange={(e) => setOp(e.target.value)}>
          {OPS.map((o) => <option key={o.op} value={o.op}>{o.label}</option>)}
        </select>
      </label>
      <label>
        Threshold Y {unitInfo(unit).axis ? `(${unitInfo(unit).axis})` : unit ? `(${unit})` : ''}
        <input type="number" step="0.1" required value={threshold}
          onChange={(e) => setThreshold(e.target.value)} />
      </label>
      <button type="submit" disabled={busy || !targets.length}>Add alarm</button>
      <p className="hint">Fires when {func}(value, last {windowMin || 'X'} min) {op} {threshold === '' ? 'Y' : threshold}</p>
    </form>
  );
}

/**
 * Alarms section of the dashboard, limited to the sensor family of the selected tab.
 * Props:
 * - category: The selected sensor family, e.g. "temperature", "fan", "battery". (type: string)
 * - sensors: All sensors; the panel keeps those of `category`. (type: Array)
 * - alarms: All alarms with live state; the panel keeps those of `category`. (type: Array)
 * - events: Recent fired / cleared events; the panel keeps those of `category`. (type: Array)
 * - refresh: Reloads alarms and events. (type: Function)
 * - onError: Shows an error message. (type: Function)
 */
export function AlarmPanel({ category, sensors, alarms, events, refresh, onError }) {
  const family = sensors.filter((s) => s.category === category);
  const ids = new Set(family.map((s) => s.id));
  const ci = categoryInfo(category || 'temperature', family);
  const shownAlarms = alarms.filter((a) => ids.has(a.sensor_id));
  const shownEvents = events.filter((e) => ids.has(e.sensor_id));
  const elsewhere = alarms.length - shownAlarms.length;  // alarms of other families
  /** Unit of a sensor id. */
  const unitOf = (id) => sensors.find((s) => s.id === id)?.unit;
  /** Runs an API action, then refreshes; errors go to onError. */
  const act = async (fn) => {
    try { await fn(); refresh(); } catch (err) { onError(err.message); }
  };

  return (
    <section>
      <h2>Alarms: {ci.title}</h2>
      {family.length > 0 && (
        <AlarmForm key={category} category={category} sensors={family} onCreated={refresh} onError={onError} />
      )}

      {elsewhere > 0 && (
        <p className="hint">
          {elsewhere} more alarm(s) on other sensor families; select their tab above to see them.
        </p>
      )}
      {shownAlarms.length === 0 ? (
        <div className="empty">No alarms defined for {ci.items}.</div>
      ) : (
        <table>
          <thead>
            <tr>
              <th>#</th><th>Sensor</th><th>Condition</th><th>State</th><th>Current aggregate</th>
              <th>Last change</th><th />
            </tr>
          </thead>
          <tbody>
            {shownAlarms.map((a) => (
              <tr key={a.id} className={a.state === 'firing' ? 'row-firing' : ''}>
                <td>{a.id}</td>
                <td>{a.sensor_label}</td>
                <td><code>{a.condition}</code></td>
                <td><StateBadge state={a.state} /></td>
                <td>{fmtVal(a.last_value, a.func, unitOf(a.sensor_id))}</td>
                <td>{fmtTs(a.last_change_ts)}</td>
                <td className="actions">
                  <button type="button" onClick={() => act(() => api.updateAlarm(a.id, { enabled: !a.enabled }))}>
                    {a.enabled ? 'Disable' : 'Enable'}
                  </button>
                  <button type="button" onClick={() => act(() => api.deleteAlarm(a.id))}>Delete</button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      <h3>Recent alarm events</h3>
      {shownEvents.length === 0 ? (
        <div className="empty">No alarm for {ci.items} has fired yet.</div>
      ) : (
        <table className="events">
          <thead><tr><th>Time</th><th>Alarm</th><th>Sensor</th><th>Event</th><th>Value</th><th>Threshold</th></tr></thead>
          <tbody>
            {shownEvents.map((e) => (
              <tr key={e.id}>
                <td>{fmtDateTime(e.ts)}</td>
                <td>#{e.alarm_id}</td>
                <td>{e.sensor_label}</td>
                <td><span className={`badge badge-${e.type === 'fired' ? 'firing' : 'ok'}`}>{e.type}</span></td>
                <td>{formatValue(e.value, unitOf(e.sensor_id), 2)}</td>
                <td>{e.op || '>'} {e.threshold}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </section>
  );
}
