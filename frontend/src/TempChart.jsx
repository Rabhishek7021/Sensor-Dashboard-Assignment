/**
 * @file TempChart.jsx
 * @brief Recharts line charts: all sensors of a family in one chart, or one small chart per sensor.
 */
import React, { useMemo } from 'react';
import {
  CartesianGrid, Legend, Line, LineChart, ReferenceLine, ResponsiveContainer, Tooltip, XAxis, YAxis,
} from 'recharts';
import { formatValue, unitInfo } from './units.js';

/** Line colours; a sensor keeps the same colour in the table and every chart. */
const PALETTE = [
  '#1f77b4', '#ff7f0e', '#2ca02c', '#d62728', '#9467bd', '#8c564b',
  '#e377c2', '#7f7f7f', '#bcbd22', '#17becf', '#393b79', '#ad494a',
];
/**
 * Colour for a sensor's index within its family.
 * Parameters:
 * - index: Position of the sensor in its family. (type: number)
 * @returns {string} CSS colour.
 */
export const colorFor = (index) => PALETTE[index % PALETTE.length];

/** Formats an epoch-ms timestamp as a local HH:MM:SS axis label. */
const fmtTime = (ts) =>
  new Date(ts).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });

/**
 * Merges series into chart rows {ts, s<id>: value}.
 *
 * All sensors are sampled with the same timestamp per tick, so the series
 * align on ts and can be merged into one row per instant.
 * Parameters:
 * - series: Series from useLiveData(). (type: Array<{sensor_id: number, points: Array}>)
 * @returns {Array<object>} Rows sorted by time.
 */
function mergeSeries(series) {
  const rows = new Map();
  for (const s of series) {
    for (const [ts, v] of s.points) {
      if (!rows.has(ts)) rows.set(ts, { ts });
      rows.get(ts)[`s${s.sensor_id}`] = v;
    }
  }
  return [...rows.values()].sort((a, b) => a.ts - b.ts);
}

/**
 * Grid, time axis, value axis and tooltip shared by every chart.
 *
 * Returned as an array (not a component): Recharts only recognises axis
 * elements that are direct children of the chart.
 * Parameters:
 * - from: Window start (epoch ms). (type: number)
 * - to: Window end (epoch ms). (type: number)
 * - unit: Unit of the values (axis label and tooltip format). (type: string)
 * @returns {JSX.Element[]} Chart children.
 */
function axes(from, to, unit) {
  return [
    <CartesianGrid key="g" strokeDasharray="3 3" stroke="#ddd" />,
    <XAxis
      key="x" dataKey="ts" type="number" scale="time" domain={[from, to]}
      tickFormatter={fmtTime} minTickGap={40} fontSize={11}
    />,
    <YAxis key="y" unit={unitInfo(unit).axis} domain={['auto', 'auto']} width={58} fontSize={11}
      allowDecimals={false} />,
    <Tooltip
      key="t"
      labelFormatter={(ts) => new Date(ts).toLocaleString()}
      formatter={(v, name) => [formatValue(v, unit), name]}
    />,
  ];
}

/**
 * One chart, one line per visible sensor (all sensors share one unit,
 * because the dashboard shows one category at a time).
 *
 * Memoised: it only re-renders when the chart data (once per second) or the
 * visible set changes, not on every live value update of the page.
 * Props:
 * - series: Series of the current family. (type: Array)
 * - colorIndex: Sensor id -> colour index. (type: object)
 * - visible: Ids of the sensors to draw. (type: Set)
 * - unit: Unit of the family. (type: string)
 * - from: Window start (epoch ms). (type: number)
 * - to: Window end (epoch ms). (type: number)
 */
export const CombinedChart = React.memo(function CombinedChart({ series, colorIndex, visible, unit, from, to }) {
  const shown = useMemo(() => series.filter((s) => visible.has(s.sensor_id)), [series, visible]);
  const data = useMemo(() => mergeSeries(shown), [shown]);
  if (!data.length) return <div className="empty">No readings in this window yet.</div>;
  return (
    <ResponsiveContainer width="100%" height={380}>
      <LineChart data={data} margin={{ top: 8, right: 16, bottom: 4, left: 0 }}>
        {axes(from, to, unit)}
        <Legend wrapperStyle={{ fontSize: 12 }} />
        {shown.map((s) => (
          <Line
            key={s.sensor_id} dataKey={`s${s.sensor_id}`} name={s.label}
            stroke={colorFor(colorIndex[s.sensor_id])}
            strokeWidth={1.4}
            dot={false} isAnimationActive={false} connectNulls
          />
        ))}
      </LineChart>
    </ResponsiveContainer>
  );
});

/**
 * Small multiples: one chart per sensor, with that sensor's own alarm
 * thresholds drawn as reference lines (avg/max/min only; stddev is a spread,
 * not a value on the axis). A sensor with a firing alarm is outlined in red.
 * Props: the same as CombinedChart, plus `alarms` (alarm list with live state).
 */
export const PerSensorGrid = React.memo(function PerSensorGrid({ series, colorIndex, visible, unit, alarms, from, to }) {
  const shown = useMemo(() => series.filter((s) => visible.has(s.sensor_id)), [series, visible]);
  return (
    <div className="grid">
      {shown.map((s) => {
        const own = alarms.filter((a) => a.sensor_id === s.sensor_id && a.enabled && a.func !== 'stddev');
        const firing = alarms.some((a) => a.sensor_id === s.sensor_id && a.state === 'firing');
        const data = s.points.map(([ts, v]) => ({ ts, v }));
        const last = data.length ? data[data.length - 1].v : null;
        return (
          <div key={s.sensor_id} className={`cell ${firing ? 'cell-firing' : ''}`}>
            <div className="cell-head">
              <strong>{s.label}</strong>
              <span>{formatValue(last, s.unit || unit)}</span>
            </div>
            <ResponsiveContainer width="100%" height={150}>
              <LineChart data={data} margin={{ top: 4, right: 8, bottom: 0, left: -8 }}>
                {axes(from, to, s.unit || unit)}
                <Line
                  dataKey="v" name={s.label} stroke={colorFor(colorIndex[s.sensor_id])}
                  dot={false} isAnimationActive={false} strokeWidth={1.5}
                />
                {own.map((a) => (
                  <ReferenceLine
                    key={a.id} y={a.threshold} stroke={a.state === 'firing' ? '#c00' : '#999'}
                    strokeDasharray="4 3" ifOverflow="extendDomain"
                    label={{ value: `${a.func}${a.op || '>'}${a.threshold}`, fontSize: 10, position: 'insideTopRight' }}
                  />
                ))}
              </LineChart>
            </ResponsiveContainer>
          </div>
        );
      })}
    </div>
  );
});
