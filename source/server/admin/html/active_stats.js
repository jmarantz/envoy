/**
 * This file contains JavaScript functionality to periodically fetch JSON stats from
 * the Envoy server, and display the top 50 (by default) stats in order of how often
 * they've changed since the page was brought up. This can be useful to find potentially
 * problematic listeners or clusters or other high-cardinality subsystems in Envoy whose
 * activity can be quickly examined for potential problems. The more active a stat, the
 * more likely it is to reflect behavior of note.
 */

// Follow-ups:
//   * top-n algorithm to limit compute overhead with high cardinality stats with user control of N.
//   * alternate sorting criteria, reverse-sort controls, etc.
//   * detect when user is not looking at page and stop or slow down pinging the server
//   * hierarchical display
//   * json flavor to send hierarchical names to save serialization/deserialization costs
//   * pause auto-refresh for at least 5 seconds when editng fields.
//   * don't auto-refresh when there is error -- provide a button to re-retry.
//   * consider removing histogram mode during active display, and overlay summary graphics
//   * rename bucket mode "none" to "summary"
//   * improve graphics
//   * integrate interval view.
//   * sort histograms by change-count
//   * incremental histogram update
//   * stretchable height -- resize histograms

/**
 * Maps a stat name to a record containing name, value, and a use-count. This
 * map is rebuilt every 5 seconds.
 */
const nameStatsMap = new Map();

/**
 * The first time this script loads, it will write PRE element at the end of body.
 * This is hooked to the `DOMContentLoaded` event.
 */
let activeStatsPreElement = null;

/**
 * A div into which we render histograms.
 */
let activeStatsHistogramsDiv = null;

/**
 * A small div for displaying status and error messages.
 */
let statusDiv = null;

/**
 * This magic string is derived from C++ in StatsHtmlRender::urlHandler to uniquely
 * name each parameter. In the stats page there is only one parameter, so it's
 * always param-1. The reason params are numbered is that on the home page, "/",
 * all the admin endpoints have uniquely numbered parameters.
 */
const paramIdPrefix = 'param-1-stats-';

let postRenderTestHook = null;
let currentValues = null;
let initialValues = null;
let reloadTimer = null;
let controls = null;

/**
 * To make testing easier, provide a hook for tests to set, to enable tests
 * to block on rendering.
 *
 * @param {!function()} hook
 */
function setRenderTestHook(hook) { // eslint-disable-line no-unused-vars
  if (postRenderTestHook != null) {
    throw new Exception('setRenderTestHook called with hook already pending');
  }
  postRenderTestHook = hook;
}

function getParamValues() {
  let values = {};
  for (key of Object.keys(controls)) {
    values[key] = controls[key].value;
  }
  return values;
/*
  return {
    filter: paramValue('filter'),
    type: paramValue('type'),
    max_display_count: loadSettingOrUseDefault('active-max-display-count', 50),
    update_interval_sec: loadSettingOrUseDefault('active-update-interval', 5)
  };
*/
}

/**
 * Hook that's run on DOMContentLoaded to create the HTML elements (just one
 * PRE right now) and kick off the periodic JSON updates.
 */
function initHook() {
  statusDiv = document.createElement('div');
  statusDiv.className = 'error-status-line';
  activeStatsPreElement = document.createElement('pre');
  activeStatsPreElement.id = 'active-content-pre';
  const table = document.createElement('table');
  table.classList.add('histogram-body', 'histogram-column');
  activeStatsHistogramsDiv = document.createElement('div');
  //activeStatsHistogramsTable.id = 'active-content-histograms-table';
  document.body.appendChild(statusDiv);
  document.body.appendChild(activeStatsPreElement);
  document.body.appendChild(activeStatsHistogramsDiv);
  controls = {
    filter: document.getElementById(paramIdPrefix + 'filter'),
    type: document.getElementById(paramIdPrefix + 'type'),
    max_display_count: document.getElementById('active-max-display-count'),
    update_interval_sec: document.getElementById('active-update-interval')
  };
  initialValues = currentValues = {
    filter: '',
    type: 'All',
    max_display_count: '50',
    update_interval_sec: '5',
  };
  setControls();

  // The type widget activates immediately on any change, recording history.
  controls['type'].addEventListener('change', onSubmit);

  // The three text controls reset the auto-refresh timer when there's a change,
  // and auto-submit on form submit, which gets run when user hits Return when
  // focus in a text widget. They also auto-submit when the focus shifts out
  // of them.
  for (name of ['filter', 'max_display_count', 'update_interval_sec']) {
    const control = controls[name];
    control.addEventListener('change', updateParams);
    control.addEventListener('blur', onSubmit);
  }
  loadStats();
}

// Called when anyone edits a type-in field. This doesn't re-execute the stats
// query from the incremental edit immediately, but it resets the timer. If
// the timer expires before the user hits return then it auto-confirms the
// new setting.
function updateParams() {
  clearTimer();
  setTimer();
}

function allValuesEqual(a, b) {
  const a_keys = Object.keys(a);
  const b_keys = Object.keys(b);
  if (a_keys.length != b_keys.length) {
    return false;
  }
  for (key of a_keys) {
    if (!b.hasOwnProperty(key)) {
      return false;
    }
    if (a[key] != b[key]) {
      return false;
    }
  }
  return true;
}

// Called when user hits return in any type-in field, or when someone changes the Type field.
function onSubmit() {
  saveState();
  loadStats();
  return false;
}

function saveState() {
  const newValues = getParamValues();
  if (allValuesEqual(newValues, currentValues)) {
    return;
  }
  currentValues = newValues;
  history.pushState(currentValues, null,
                    'html-active?filter=' + encodeURIComponent(currentValues.filter) +
                    '&type=' + encodeURIComponent(currentValues.type));
}

function setControls() {
  // Set the widgets to the new values.
  for (key of Object.keys(controls)) {
    console.log('setting control ' + key + ' to ' + currentValues[key]);
    controls[key].value = currentValues[key];
  }
}

// Called when someone presses the back/forward button.
function navigateHook(event) {
  if (event.state) {
    currentValues = event.state;
  } else {
    currentValues = initialValues;
  }

  setControls();
  loadStats();
}

function clearTimer() {
  if (reloadTimer != null) {
    window.clearTimeout(reloadTimer);
    reloadTimer = null;
  }
}

function setTimer() {
  reloadTimer = window.setTimeout(() => {
    reloadTimer = null;
    loadStats();
  }, 1000*loadSettingOrUseDefault('active-update-interval', 5));
}

/**
 * Initiates an Ajax request for the stats JSON based on the stats parameters.
 */
async function loadStats() {
  clearTimer();

  const makeQueryParam = (name) => name + '=' + encodeURIComponent(currentValues[name]);
  const params = ['filter', 'type'];
  const url = '/stats?format=json&usedonly&histogram_buckets=detailed&' +
       params.map(makeQueryParam).join('&');
  try {
    const response = await fetch(url);
    const data = await response.json();
    renderStats(activeStatsHistogramsDiv, data);
  } catch (e) {
    statusDiv.textContent = 'Error fetching ' + url + ': ' + e;
  }

  // Update stats with a minimum of 5 second interval between completing the
  // previous update and the start of the new one. So if it takes 1 second
  // to refresh the stats we'll do it every 6 seconds, by default.
  setTimer();
}

/**
 * Function used to sort stat records. The highest priority is update frequency,
 * then value (higher values are likely more interesting), and finally alphabetic,
 * in forward order.
 *
 * @param {!Object} a
 * @param {!Object} b
 * @return {number}
 */
function compareStatRecords(a, b) {
  // Sort higher change-counts first.
  if (a.change_count != b.change_count) {
    return b.change_count - a.change_count;
  }

  // Secondarily put higher values first -- they are often more interesting.
  if (b.value != a.value) {
    return b.value - a.value;
  }

  // Fall back to forward alphabetic sort.
  if (a.name < b.name) {
    return -1;
  }
  if (a.name > b.name) {
    return 1;
  }
  return 0;
}

/**
 * The active display has additional settings for tweaking it -- this helper extracts numeric
 * values from text widgets
 *
 * @param {string} id
 * @param {number} defaultValue
 * @return {number}
 */
function loadSettingOrUseDefault(id, defaultValue) {
  const elt = document.getElementById(id);
  const value = parseInt(elt.value);
  if (Number.isNaN(value) || value <= 0) {
    console.log('Invalid ' + id + ': invalid positive number');
    return defaultValue;
  }
  return value;
}

/**
 * Rendering function which interprets the Json response from the server, updates
 * the most-frequently-used map, reverse-sorts by use-count, and serializes the
 * top ordered stats into the PRE element created in initHook.
 *
 * @param {!Element} histogramDiv
 * @param {!Object} data
 */
function renderStats(histogramDiv, data) {
  sortedStats = [];
  renderHistograms(histogramDiv, data);
  histogramDiv.replaceChildren();
  for (stat of data.stats) {
    if (!stat.name) {
      continue;
    }
    let statRecord = nameStatsMap.get(stat.name);
    if (statRecord) {
      if (statRecord.value != stat.value) {
        statRecord.value = stat.value;
        ++statRecord.change_count;
      }
    } else {
      statRecord = {name: stat.name, value: stat.value, change_count: 0};
      nameStatsMap.set(stat.name, statRecord);
    }
    sortedStats.push(statRecord);
  }

  // Sorts all the stats. This is inefficient; we should just pick the top N
  // based on field "active-max-display-count" and sort those. The best
  // algorithms for this require a heap or priority queue. JS implementations
  // of those can be found, but that would bloat this relatively modest amount
  // of code, and compel us to do a better job writing tests.
  sortedStats.sort(compareStatRecords);

  const max = loadSettingOrUseDefault('active-max-display-count', 50);
  let index = 0;
  let text = '';
  for (const statRecord of sortedStats) {
    if (++index > max) {
      break;
    }
    text += `${statRecord.name}: ${statRecord.value} (${statRecord.change_count})\n`;
  }
  if (activeStatsPreElement) {
    activeStatsPreElement.textContent = text;
  }

  // If a post-render test-hook has been established, call it, but clear
  // the hook first, so that the callee can set a new hook if it wants to.
  if (postRenderTestHook) {
    const hook = postRenderTestHook;
    postRenderTestHook = null;
    hook();
  }

  statusDiv.textContent = '';
}

// We don't want to trigger any DOM manipulations until the DOM is fully loaded.
addEventListener('DOMContentLoaded', initHook);
addEventListener('popstate', navigateHook);
