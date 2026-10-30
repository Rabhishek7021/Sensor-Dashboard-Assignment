/**
 * @file api.js
 * @brief Thin client for the C++ REST API.
 *
 * Every call throws an Error carrying the server's {"error": "..."} message on
 * non-2xx responses.
 */

/**
 * Sends a JSON request to the API.
 * Parameters:
 * - path: API path, e.g. "/api/sensors". (type: string)
 * - options: fetch() options (method, body, ...). (type: object)
 * @returns {Promise<any>}  Parsed JSON body, or null for 204 No Content.
 * @throws {Error} With the server's error message on a non-2xx status.
 */
async function request(path, options = {}) {
  const res = await fetch(path, {
    headers: { 'Content-Type': 'application/json' },
    ...options,
  });
  if (res.status === 204) return null;
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(body.error || `HTTP ${res.status}`);
  return body;
}

/**
 * API endpoints used by the dashboard. Each returns a Promise of the parsed JSON.
 * - health(): GET /api/health
 * - sensors(): GET /api/sensors
 * - readings(minutes, maxPoints = 600): GET /api/readings (bucketed history)
 * - alarms(): GET /api/alarms
 * - events(limit = 30): GET /api/alarm-events
 * - createAlarm(body): POST /api/alarms
 * - updateAlarm(id, patch): PATCH /api/alarms/{id}
 * - deleteAlarm(id): DELETE /api/alarms/{id}
 */
export const api = {
  health: () => request('/api/health'),
  sensors: () => request('/api/sensors'),
  readings: (minutes, maxPoints = 600) =>
    request(`/api/readings?minutes=${minutes}&max_points=${maxPoints}`),
  alarms: () => request('/api/alarms'),
  events: (limit = 30) => request(`/api/alarm-events?limit=${limit}`),
  createAlarm: (body) => request('/api/alarms', { method: 'POST', body: JSON.stringify(body) }),
  updateAlarm: (id, patch) =>
    request(`/api/alarms/${id}`, { method: 'PATCH', body: JSON.stringify(patch) }),
  deleteAlarm: (id) => request(`/api/alarms/${id}`, { method: 'DELETE' }),
};
