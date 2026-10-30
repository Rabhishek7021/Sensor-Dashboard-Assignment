/**
 * @file live.js
 * @brief Live data for the dashboard (React hook useLiveData).
 *
 * One history request per window change, then everything else arrives over a
 * single Server-Sent Events stream (/api/stream): samples every 500 ms, alarm
 * states every second, and notifications when alarms or sensors change.
 * Nothing is polled.
 *
 * Samples are folded into the same time buckets the server used for the
 * history (window / 600 points), so memory and chart size stay constant
 * however long the page is open. React state is updated on timers, not per
 * message: current values twice per second, charts at most once per second.
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import { api } from './api.js';

/** Refresh period of the current values (ms). */
const VALUES_MS = 500;
/** Refresh period of the charts (ms). */
const CHART_MS = 1000;
/** Live ticks kept to re-apply after a history reload (ms). */
const RECENT_MS = 15000;

/**
 * Merges n samples (sum, min, max) into the bucket `key` of a sensor's bucket map.
 * Parameters:
 * - map: Buckets by start time. (type: Map<number, {n: number, sum: number, min: number, max: number}>)
 * - key: Bucket start (epoch ms). (type: number)
 * - n: Number of samples. (type: number)
 * - sum: Sum of the samples. (type: number)
 * - min: Smallest sample. (type: number)
 * - max: Largest sample. (type: number)
 */
function addToBucket(map, key, n, sum, min, max) {
  const b = map.get(key);
  if (b) {
    b.n += n; b.sum += sum;
    if (min < b.min) b.min = min;
    if (max > b.max) b.max = max;
  } else {
    map.set(key, { n, sum, min, max });
  }
}

/**
 * React hook providing everything the dashboard shows, kept live over /api/stream.
 * Parameters:
 * - minutes: Selected history window in minutes. (type: number)
 * @returns {{sensors: Array, alarms: Array, events: Array, chart: object, latest: object,
 *            status: object, loadAlarms: Function, setError: Function}}
 *   - sensors: sensor list from /api/sensors
 *   - alarms / events: alarm list with live state, and recent fired/cleared events
 *   - chart: {series, stats, from, to, bucketMs} for the selected window
 *   - latest: newest value per sensor id
 *   - status: {connected, lastTs, error, source} of the live stream
 *   - loadAlarms(): reload alarms and events; setError(msg): show / clear the error banner
 */
export function useLiveData(minutes) {
  const [sensors, setSensors] = useState([]);
  const [alarms, setAlarms] = useState([]);
  const [events, setEvents] = useState([]);
  const [chart, setChart] = useState({ series: [], stats: {}, from: 0, to: 0, bucketMs: 0 });
  const [latest, setLatest] = useState({});
  const [status, setStatus] = useState({ connected: false, lastTs: null, error: null, source: null });

  // Mutable store written by stream handlers, read by the render timers.
  const st = useRef({
    minutes, bucketMs: 1000, buckets: new Map(), historyTo: 0, latest: {}, lastTs: 0,
    recent: [], meta: {}, chartDirty: true, valuesDirty: true, historySeq: 0,
  });

  const alarmIds = useRef('');  // ids of the loaded alarm list, to detect creates/deletes
  /** Sets (or clears with null) the error shown in the banner. */
  const setError = useCallback((error) => setStatus((s) => ({ ...s, error })), []);

  /** Loads /api/sensors and merges metadata and latest values into the store. */
  const loadSensors = useCallback(async () => {
    try {
      const list = await api.sensors();
      const s = st.current;
      for (const x of list) {
        s.meta[x.id] = { label: x.label, unit: x.unit, category: x.category };
        if (x.latest && !(s.latest[x.id]?.ts >= x.latest.ts)) s.latest[x.id] = x.latest;
      }
      s.valuesDirty = true;
      s.chartDirty = true;
      setSensors(list);
    } catch (err) {
      setError(`Cannot reach API: ${err.message}`);
    }
  }, [setError]);

  /** Loads the alarm list and the 30 newest events. */
  const loadAlarms = useCallback(async () => {
    try {
      const [a, e] = await Promise.all([api.alarms(), api.events(30)]);
      alarmIds.current = a.map((x) => x.id).join(',');
      setAlarms(a);
      setEvents(e);
    } catch (err) {
      setError(`Cannot reach API: ${err.message}`);
    }
  }, [setError]);

  /** Applies one live tick {ts, v: {sensorId: value}}: latest values and chart buckets. */
  const applyTick = useCallback((tick) => {
    const s = st.current;
    if (tick.ts > s.lastTs) s.lastTs = tick.ts;
    const key = Math.floor(tick.ts / s.bucketMs) * s.bucketMs;
    for (const [id, v] of Object.entries(tick.v)) {
      const prev = s.latest[id];
      if (!prev || tick.ts >= prev.ts) s.latest[id] = { ts: tick.ts, value: v };
      if (tick.ts <= s.historyTo) continue;  // already contained in the loaded history
      let m = s.buckets.get(Number(id));
      if (!m) { m = new Map(); s.buckets.set(Number(id), m); }
      addToBucket(m, key, 1, v, v, v);
    }
    s.valuesDirty = true;
    s.chartDirty = true;
  }, []);

  /**
   * Loads bucketed history for `mins` minutes and re-applies newer live ticks.
   * Responses superseded by a newer request are ignored.
   */
  const loadHistory = useCallback(async (mins) => {
    const s = st.current;
    const seq = ++s.historySeq;
    try {
      const r = await api.readings(mins);
      if (seq !== s.historySeq) return;  // a newer request superseded this one
      const buckets = new Map();
      for (const series of r.series) {
        const m = new Map();
        for (const [ts, avg, min, max, n] of series.points) m.set(ts, { n, sum: avg * n, min, max });
        buckets.set(series.sensor_id, m);
        s.meta[series.sensor_id] = { label: series.label, unit: series.unit, category: series.category };
      }
      s.minutes = mins;
      s.bucketMs = r.bucket_ms;
      s.buckets = buckets;
      s.historyTo = r.to;  // the server includes every reading up to here
      if (r.to > s.lastTs) s.lastTs = r.to;
      // Live ticks that arrived after the history snapshot.
      for (const t of s.recent) if (t.ts > r.to) applyTick(t);
      s.chartDirty = true;
      setStatus((x) => ({ ...x, error: null }));
    } catch (err) {
      setError(`Cannot reach API: ${err.message}`);
    }
  }, [applyTick, setError]);

  // History for the selected window.
  useEffect(() => { st.current.minutes = minutes; loadHistory(minutes); }, [minutes, loadHistory]);

  // Live stream.
  useEffect(() => {
    loadSensors();
    loadAlarms();
    let first = true;
    const es = new EventSource('/api/stream');
    es.addEventListener('hello', (e) => {
      const h = JSON.parse(e.data);
      setStatus((x) => ({ ...x, connected: true, error: null, source: h.source }));
      if (!first) {
        // Reconnected: fill the gap and refresh everything that may have changed.
        loadSensors();
        loadAlarms();
        loadHistory(st.current.minutes);
      }
      first = false;
    });
    es.addEventListener('samples', (e) => {
      const s = st.current;
      for (const tick of JSON.parse(e.data)) {
        applyTick(tick);
        s.recent.push(tick);
      }
      const cutoff = s.lastTs - RECENT_MS;
      while (s.recent.length && s.recent[0].ts < cutoff) s.recent.shift();
    });
    es.addEventListener('alarms', (e) => {
      const list = JSON.parse(e.data);
      if (list.map((a) => a.id).join(',') !== alarmIds.current) {
        loadAlarms();  // created or deleted (possibly from another browser)
        return;
      }
      const live = new Map(list.map((a) => [a.id, a]));
      setAlarms((prev) => prev.map((a) => ({ ...a, ...live.get(a.id) })));
    });
    es.addEventListener('alarm', () => loadAlarms());     // a transition: refresh the event log
    es.addEventListener('sensors', () => loadSensors());  // sensor set changed
    es.onerror = () => setStatus((x) => ({ ...x, connected: false }));
    return () => es.close();
  }, [applyTick, loadAlarms, loadHistory, loadSensors]);

  // Render timers: publish current values every VALUES_MS and rebuild chart series
  // (dropping buckets that slid out of the window) every CHART_MS, only when dirty.
  useEffect(() => {
    const values = setInterval(() => {
      const s = st.current;
      if (!s.valuesDirty) return;
      s.valuesDirty = false;
      setLatest({ ...s.latest });
      setStatus((x) => (x.lastTs === s.lastTs ? x : { ...x, lastTs: s.lastTs }));
    }, VALUES_MS);
    const charts = setInterval(() => {
      const s = st.current;
      if (!s.chartDirty) return;
      s.chartDirty = false;
      const to = s.lastTs || Date.now();
      const from = to - s.minutes * 60000;
      const firstKey = Math.floor(from / s.bucketMs) * s.bucketMs;
      const series = [];
      const stats = {};
      for (const [id, m] of s.buckets) {
        const points = [];
        let n = 0, sum = 0, min = Infinity, max = -Infinity;
        for (const [ts, b] of m) {
          if (ts < firstKey) { m.delete(ts); continue; }  // slid out of the window
          points.push([ts, b.sum / b.n]);
          n += b.n; sum += b.sum;
          if (b.min < min) min = b.min;
          if (b.max > max) max = b.max;
        }
        const meta = s.meta[id] || {};
        series.push({ sensor_id: id, label: meta.label, unit: meta.unit, category: meta.category, points });
        if (n) stats[id] = { min, max, avg: sum / n };
      }
      setChart({ series, stats, from, to, bucketMs: s.bucketMs });
    }, CHART_MS);
    return () => { clearInterval(values); clearInterval(charts); };
  }, []);

  return { sensors, alarms, events, chart, latest, status, loadAlarms, setError };
}
