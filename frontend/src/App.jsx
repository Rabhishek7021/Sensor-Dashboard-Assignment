/**
 * @file App.jsx
 * @brief Dashboard page: header, category tabs, charts, sensor table and alarm panel.
 */
import React, { useMemo, useState } from 'react';
import { CombinedChart, PerSensorGrid, colorFor } from './TempChart.jsx';
import { AlarmPanel, StateBadge } from './AlarmPanel.jsx';
import { categoriesOf, categoryInfo, formatValue } from './units.js';
import { useLiveData } from './live.js';

/** Selectable history windows in minutes (5 min ... 24 h). */
const WINDOWS = [5, 15, 60, 360, 1440];

/**
 * Root component of the dashboard.
 *
 * Shows one sensor family (category) at a time: a combined chart or one chart
 * per sensor, a table with current / min / avg / max and alarm badges, and the
 * alarm panel. All data comes from useLiveData().
 * @returns {JSX.Element} The page.
 */
export default function App() {
  const [minutes, setMinutes] = useState(15);
  const [view, setView] = useState('combined');
  const [category, setCategory] = useState(null);
  const [hidden, setHidden] = useState(new Set());
  // History once per window, then live over Server-Sent Events (see live.js).
  const { sensors, alarms, events, chart, latest, status, loadAlarms, setError } = useLiveData(minutes);
  const error = status.error;

  // Sensor families present on this machine (temperature, fan, battery, ...).
  const categories = useMemo(() => categoriesOf(sensors), [sensors]);
  const active = categories.includes(category) ? category : categories[0];
  const inCategory = useMemo(() => sensors.filter((s) => s.category === active), [sensors, active]);
  const info = categoryInfo(active || 'temperature', inCategory);
  const unit = inCategory[0]?.unit;

  // Stable colour per sensor within its family, shared by table and charts.
  const colorIndex = useMemo(() => {
    const idx = {};
    for (const c of categories) sensors.filter((s) => s.category === c).forEach((s, i) => { idx[s.id] = i; });
    return idx;
  }, [sensors, categories]);
  const visible = useMemo(
    () => new Set(inCategory.map((s) => s.id).filter((id) => !hidden.has(id))), [inCategory, hidden],
  );
  /** Shows / hides one sensor in the charts. */
  const toggle = (id) => setHidden((h) => {
    const n = new Set(h);
    n.has(id) ? n.delete(id) : n.add(id);
    return n;
  });
  /** Shows (true) or hides (false) every sensor of the current family. */
  const setAll = (show) => setHidden((h) => {
    const n = new Set(h);
    for (const s of inCategory) (show ? n.delete(s.id) : n.add(s.id));
    return n;
  });

  const series = useMemo(
    () => chart.series.filter((s) => (s.category || 'temperature') === active), [chart, active],
  );
  // Number of alarms currently firing (header badge).
  const firingCount = alarms.filter((a) => a.state === 'firing').length;

  // Header summary, e.g. "10 cores · 2 fans · 1 battery".
  const counts = categories.map((c) => {
    const list = sensors.filter((s) => s.category === c);
    const ci = categoryInfo(c, list);
    return `${list.length} ${list.length === 1 ? ci.item.toLowerCase() : ci.items}`;
  });

  // Layout: header (counts, live marker, firing badge), error banner, category tabs,
  // chart section (window / view toolbar), sensor table, alarm panel.
  return (
    <div className="page">
      <header>
        <h1>System Sensors</h1>
        <div className="meta">
          {sensors.length ? counts.join(' · ') : 'no sensors detected yet'}
          {' · '}
          <span className={`live ${status.connected ? 'live-on' : 'live-off'}`}>
            {status.connected ? 'live' : 'reconnecting…'}
          </span>
          {status.lastTs ? ` ${new Date(status.lastTs).toLocaleTimeString()}` : ''}
          {firingCount > 0 && <span className="badge badge-firing">{firingCount} alarm(s) firing</span>}
        </div>
      </header>

      {error && <div className="error" onClick={() => setError(null)}>{error} (click to dismiss)</div>}

      {categories.length > 1 && (
        <nav className="tabs">
          {categories.map((c) => {
            const list = sensors.filter((s) => s.category === c);
            // Firing alarms of this family, so they stay visible while another tab is open.
            const ids = new Set(list.map((s) => s.id));
            const firing = alarms.filter((a) => a.state === 'firing' && ids.has(a.sensor_id)).length;
            return (
              <button key={c} className={c === active ? 'active' : ''} onClick={() => setCategory(c)}>
                {categoryInfo(c, list).title} ({list.length})
                {firing > 0 && <span className="badge badge-firing">{firing} firing</span>}
              </button>
            );
          })}
        </nav>
      )}

      <section>
        <h2>{info.title}</h2>
        <div className="toolbar">
          <span>Window:</span>
          {WINDOWS.map((w) => (
            <button key={w} className={w === minutes ? 'active' : ''} onClick={() => setMinutes(w)}>
              {w < 60 ? `${w} min` : `${w / 60} h`}
            </button>
          ))}
          <span className="sep" />
          <span>View:</span>
          <button className={view === 'combined' ? 'active' : ''} onClick={() => setView('combined')}>Combined</button>
          <button className={view === 'grid' ? 'active' : ''} onClick={() => setView('grid')}>
            Per {info.item.toLowerCase()}
          </button>
          {chart.bucketMs > 0 && (
            <span className="hint">
              {chart.bucketMs < 1000 ? `${chart.bucketMs} ms` : `${Math.round(chart.bucketMs / 1000)} s`} per point
            </span>
          )}
        </div>
        {view === 'combined' ? (
          <CombinedChart series={series} colorIndex={colorIndex} visible={visible} unit={unit}
            from={chart.from} to={chart.to} />
        ) : (
          <PerSensorGrid series={series} colorIndex={colorIndex} visible={visible} unit={unit}
            alarms={alarms} from={chart.from} to={chart.to} />
        )}
      </section>

      <section>
        <div className="toolbar">
          <button onClick={() => setAll(true)}>Show all</button>
          <button onClick={() => setAll(false)}>Hide all</button>
        </div>
        <table>
          <thead>
            <tr>
              <th>Show</th><th>{info.item}</th><th>Current</th>
              <th>Min ({minutes}m)</th><th>Avg</th><th>Max</th><th>Alarms</th>
            </tr>
          </thead>
          <tbody>
            {inCategory.map((s) => {
              const st = chart.stats[s.id];
              const own = alarms.filter((a) => a.sensor_id === s.id);
              const firing = own.some((a) => a.state === 'firing');
              return (
                <tr key={s.id} className={firing ? 'row-firing' : ''}>
                  <td><input type="checkbox" checked={visible.has(s.id)} onChange={() => toggle(s.id)} /></td>
                  <td><span className="swatch" style={{ background: colorFor(colorIndex[s.id]) }} />{s.label}</td>
                  <td><strong>{formatValue(latest[s.id]?.value, s.unit)}</strong></td>
                  <td>{formatValue(st?.min, s.unit)}</td>
                  <td>{formatValue(st?.avg, s.unit)}</td>
                  <td>{formatValue(st?.max, s.unit)}</td>
                  <td>
                    {own.length === 0 ? <span className="muted">none</span>
                      : own.map((a) => (
                        <span key={a.id} title={a.condition}><StateBadge state={a.state} /></span>
                      ))}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </section>

      {/* Alarms of the family selected in the tabs above only. */}
      <AlarmPanel category={active} sensors={sensors} alarms={alarms} events={events}
        refresh={loadAlarms} onError={setError} />
    </div>
  );
}
