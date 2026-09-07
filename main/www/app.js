/*
 * Polls /api/sensor and updates the dashboard.
 *
 * The next request is scheduled only after the previous one settles, so at
 * most one request is ever in flight. setInterval would keep firing even when
 * the board is slow to answer, piling up sockets against the server's limit
 * of seven.
 */
var POLL_INTERVAL_MS = 2000;
var STALE_THRESHOLD_MS = 6000;

var elTemperature = document.getElementById("temperature");
var elHumidity = document.getElementById("humidity");
var elStatus = document.getElementById("status");

function setStatus(text, state) {
  elStatus.textContent = text;
  elStatus.className = "status" + (state ? " " + state : "");
}

function render(data) {
  elTemperature.textContent = data.temperature.toFixed(1);
  elHumidity.textContent = data.humidity.toFixed(1);

  if (data.age_ms > STALE_THRESHOLD_MS) {
    setStatus("Stale reading: " + Math.round(data.age_ms / 1000) + " s old", "warn");
  } else {
    setStatus("Updated at " + new Date().toLocaleTimeString(), "ok");
  }
}

function poll() {
  fetch("/api/sensor", { cache: "no-store" })
    .then(function (response) {
      if (response.status === 503) {
        setStatus("Waiting for the first sensor reading...", "warn");
        return null;
      }
      if (!response.ok) {
        throw new Error("HTTP " + response.status);
      }
      return response.json();
    })
    .then(function (data) {
      if (data !== null) {
        render(data);
      }
    })
    .catch(function (error) {
      /* Reached when the board reboots, WiFi drops, or the body is not JSON.
         The last known values stay on screen so the page does not flash. */
      setStatus("Connection lost: " + error.message, "error");
    })
    .then(function () {
      /* Runs after both success and failure, so polling survives an outage. */
      setTimeout(poll, POLL_INTERVAL_MS);
    });
}

document.addEventListener("DOMContentLoaded", function () {
  setStatus("Connecting...", null);
  poll();
});
