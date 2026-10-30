/**
 * @file units.js
 * @brief Presentation metadata for sensor categories and units.
 *
 * The dashboard is driven entirely by the `category` and `unit` fields the API
 * returns, so a new sensor family shows up automatically; adding an entry here
 * only gives it a nicer title and value format.
 */

/** Display format per unit: value suffix, axis label and decimal places. */
const UNITS = {
  celsius: { suffix: ' °C', axis: '°C', digits: 1 },
  rpm: { suffix: ' RPM', axis: '', digits: 0 },
  percent: { suffix: ' %', axis: '%', digits: 0 },
  volt: { suffix: ' V', axis: 'V', digits: 2 },
  watt: { suffix: ' W', axis: 'W', digits: 1 },
};

/**
 * Accepted alarm threshold Y per unit, bounds included. Must match
 * sd::alarm::threshold_range() in src/common/alarm_logic.hpp (the API rejects
 * anything else). Units without an entry accept any number.
 */
const THRESHOLD_LIMITS = {
  celsius: { min: -50, max: 100 },
  rpm: { min: 0, max: 5000 },
  percent: { min: 0, max: 100 },
};

/** Accepted alarm window X in whole minutes (sd::alarm::kMinWindowMin / kMaxWindowMin). */
export const WINDOW_LIMITS = { min: 1, max: 100 };

/**
 * Accepted threshold range of a unit.
 * Parameters:
 * - unit: Unit from the API, e.g. "rpm". (type: string)
 * @returns {?{min: number, max: number}} The range, or null if any number is accepted.
 */
export function thresholdLimits(unit) {
  return THRESHOLD_LIMITS[unit] || null;
}

/** Titles per sensor category: tab title, singular item name and plural. */
const CATEGORIES = {
  temperature: { title: 'CPU temperatures', item: 'Core', items: 'cores' },
  fan: { title: 'Fans', item: 'Fan', items: 'fans' },
  battery: { title: 'Battery', item: 'Battery', items: 'batteries' },
};

/** Upper-cases the first letter of a string. */
const titleCase = (s) => s.charAt(0).toUpperCase() + s.slice(1);

/**
 * Display format of a unit.
 * Parameters:
 * - unit: Unit from the API, e.g. "celsius". (type: string)
 * @returns {{suffix: string, axis: string, digits: number}} Known format, or a generic one.
 */
export function unitInfo(unit) {
  return UNITS[unit] || { suffix: unit ? ` ${unit}` : '', axis: unit || '', digits: 1 };
}

/**
 * Formats a value with its unit, e.g. 41.5 °C or 2600 RPM.
 * Parameters:
 * - value: Value (null / NaN shows a dash). (type: ?number)
 * - unit: Unit from the API. (type: string)
 * - digits: Decimal places (default: the unit's). (type: number, optional)
 * @returns {string} Formatted text.
 */
export function formatValue(value, unit, digits) {
  if (value == null || Number.isNaN(value)) return '—';
  const u = unitInfo(unit);
  return `${Number(value).toFixed(digits ?? u.digits)}${u.suffix}`;
}

/**
 * Titles for a sensor category.
 * Parameters:
 * - category: Category from the API, e.g. "temperature". (type: string)
 * - sensors: Sensors of that category (used to detect CPUs without per-core sensors). (type: Array)
 * @returns {{title: string, item: string, items: string}} Tab title and item names.
 */
export function categoryInfo(category, sensors = []) {
  const known = CATEGORIES[category];
  if (category === 'temperature' && sensors.length && !sensors.every((s) => s.kind === 'core')) {
    // CPUs without per-core sensors (AMD per-die, ARM thermal zones).
    return { title: 'CPU temperatures', item: 'Sensor', items: 'CPU sensors' };
  }
  return known || { title: titleCase(category), item: 'Sensor', items: `${category} sensors` };
}

/**
 * Categories in the order the API lists sensors (the provider order).
 * Parameters:
 * - sensors: Sensors from /api/sensors. (type: Array)
 * @returns {string[]} Distinct categories, in first-seen order.
 */
export function categoriesOf(sensors) {
  const seen = [];
  for (const s of sensors) if (!seen.includes(s.category)) seen.push(s.category);
  return seen;
}
