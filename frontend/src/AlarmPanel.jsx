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
import { WINDOW_LIMITS, categoryInfo, formatValue, thresholdLimits, unitInfo } from './units.js';
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
/** Unit text used in the form's labels and messages, e.g. "°C", "RPM", "%". */
const unitText = (unit) => (unit === 'rpm' ? 'RPM' : unitInfo(unit).axis || unit || '');

/**
 * Characters the window field accepts while typing: whole minutes, at most 3 digits
 * (no sign, decimal point or exponent). Anything else is not entered at all.
 */
const WINDOW_TYPING = /^\d{0,3}$/;

/**
 * Pattern for what the threshold field accepts while typing: digits with an optional
 * decimal part of up to 2 places, and a leading minus only when the unit allows
 * negative values. Letters, exponents ("1e3") and other symbols are not entered at all.
 * Parameters:
 * - limits: Range of the unit, or null for any number. (type: ?Object)
 * @returns {RegExp} Pattern for a partial entry.
 */
const thresholdTyping = (limits) => (limits && limits.min >= 0 ? /^\d{0,4}(\.\d{0,2})?$/ : /^-?\d{0,4}(\.\d{0,2})?$/);

/**
 * Checks the window X entry.
 * Parameters:
 * - text: Field content. (type: string)
 * @returns {?string} Error message, or null when valid.
 */
function windowError(text) {
  const { min, max } = WINDOW_LIMITS;
  if (text === '') return `Enter window X: ${min} to ${max} minutes.`;
  const n = Number(text);
  if (!/^\d+$/.test(text) || n < min || n > max) return `Window X must be ${min} to ${max} minutes.`;
  return null;
}

/**
 * Checks the threshold Y entry against the range of the sensor's unit.
 * Parameters:
 * - text: Field content. (type: string)
 * - unit: Unit of the target sensors. (type: string)
 * @returns {?string} Error message, or null when valid.
 */
function thresholdError(text, unit) {
  const limits = thresholdLimits(unit);
  const range = limits ? `${limits.min} to ${limits.max} ${unitText(unit)}` : 'a number';
  if (text === '') return `Enter threshold Y: ${range}.`;
  const n = Number(text);
  if (!/^-?\d+(\.\d+)?$/.test(text) || !Number.isFinite(n)) return `Threshold Y must be ${range}.`;
  if (limits && (n < limits.min || n > limits.max)) return `Threshold Y must be ${range}.`;
  return null;
}

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
 *
 * Inputs are validated as they are typed: characters that cannot form a valid entry
 * are not accepted, an out-of-range value shows an error and disables "Add alarm".
 * The limits are the same as the API's (window X 1 to 100 min; threshold Y -50 to
 * 100 °C, 0 to 5000 RPM, 0 to 100 %).
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
  const [windowMin, setWindowMin] = useState('10');
  const [threshold, setThreshold] = useState(String(suggestion.threshold));
  const [op, setOp] = useState(suggestion.op);
  const [busy, setBusy] = useState(false);

  // "all" = every sensor of the family; otherwise one sensor id.
  const targets = target === 'all' ? sensors : sensors.filter((s) => String(s.id) === target);
  const unit = targets[0]?.unit ?? sensors[0]?.unit;
  const limits = thresholdLimits(unit);
  const errors = [windowError(windowMin), thresholdError(threshold, unit)];
  const invalid = errors.some(Boolean);

  /** Creates the alarm(s) for the chosen target(s); does nothing while an entry is invalid. */
  const submit = async (e) => {
    e.preventDefault();
    if (invalid) return;
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
    <form className="alarm-form" onSubmit={submit} noValidate>
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
        Window X (min, {WINDOW_LIMITS.min} to {WINDOW_LIMITS.max})
        <input type="text" inputMode="numeric" required value={windowMin}
          className={errors[0] ? 'invalid' : undefined} aria-invalid={Boolean(errors[0])}
          aria-describedby="alarm-form-status"
          onChange={(e) => WINDOW_TYPING.test(e.target.value) && setWindowMin(e.target.value)} />
      </label>
      <label>
        Fires when
        <select value={op} onChange={(e) => setOp(e.target.value)}>
          {OPS.map((o) => <option key={o.op} value={o.op}>{o.label}</option>)}
        </select>
      </label>
      <label>
        Threshold Y ({[unitText(unit), limits && `${limits.min} to ${limits.max}`].filter(Boolean).join(', ')})
        <input type="text" inputMode={limits && limits.min >= 0 ? 'decimal' : 'text'} required value={threshold}
          className={errors[1] ? 'invalid' : undefined} aria-invalid={Boolean(errors[1])}
          aria-describedby="alarm-form-status"
          onChange={(e) => thresholdTyping(limits).test(e.target.value) && setThreshold(e.target.value)} />
      </label>
      <button type="submit" disabled={busy || invalid || !targets.length}>Add alarm</button>
      {invalid ? (
        <p className="hint form-error" id="alarm-form-status" role="alert">{errors.filter(Boolean).join(' ')}</p>
      ) : (
        <p className="hint" id="alarm-form-status">Fires when {func}(value, last {windowMin} min) {op} {threshold}</p>
      )}
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
