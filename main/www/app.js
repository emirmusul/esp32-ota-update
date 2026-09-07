/*
 * Polls /api/sensor and updates the dashboard, and drives the firmware
 * update flow.
 *
 * The next request is scheduled only after the previous one settles, so at
 * most one request is ever in flight. setInterval would keep firing even when
 * the board is slow to answer, piling up sockets against the server's limit
 * of seven.
 */
var POLL_INTERVAL_MS = 2000;
var STALE_THRESHOLD_MS = 6000;

/* Firmware details rarely change, but the image state does: it goes from
   PENDING_VERIFY to VALID a couple of seconds after a successful update.
   Refreshing on a slow timer catches that without adding traffic. */
var VERSION_POLL_INTERVAL_MS = 10000;

/* An update erases, downloads and writes about a megabyte, then reboots.
   Fifteen seconds of work plus five of start-up, with room to spare. */
var OTA_POLL_INTERVAL_MS = 2000;
var OTA_TIMEOUT_MS = 60000;

var elTemperature = document.getElementById("temperature");
var elHumidity = document.getElementById("humidity");
var elStatus = document.getElementById("status");

var elVersion = document.getElementById("fw-version");
var elSlot = document.getElementById("fw-slot");
var elState = document.getElementById("fw-state");
var elBuilt = document.getElementById("fw-built");
var elButton = document.getElementById("update-button");
var elOtaStatus = document.getElementById("ota-status");

/* Remembered across the reboot so the new image can be compared with the old
   one. Version alone is not enough: an update can carry the same version
   string, and a rollback restores a version that was running before. */
var currentBuild = null;

/* Suspends the background version poll while an update is in flight, so its
   requests do not race with the reboot watcher over the same information. */
var updateInProgress = false;

function setStatus(text, state) {
  elStatus.textContent = text;
  elStatus.className = "status" + (state ? " " + state : "");
}

function setOtaStatus(text, state) {
  elOtaStatus.textContent = text;
  elOtaStatus.className = "status" + (state ? " " + state : "");
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

function renderVersion(info) {
  elVersion.textContent = info.version;
  elSlot.textContent = info.slot;
  elState.textContent = info.state;
  elBuilt.textContent = info.built;

  elState.className = info.state === "PENDING_VERIFY" ? "pending" : "";
}

function fetchVersion() {
  return fetch("/api/version", { cache: "no-store" }).then(function (response) {
    if (!response.ok) {
      throw new Error("HTTP " + response.status);
    }
    return response.json();
  });
}

function pollVersion() {
  if (updateInProgress) {
    setTimeout(pollVersion, VERSION_POLL_INTERVAL_MS);
    return;
  }

  fetchVersion()
    .then(function (info) {
      currentBuild = info;
      renderVersion(info);
    })
    .catch(function () {
      /* The sensor poll already reports connectivity problems; a second
         message for the same outage would only be noise. */
    })
    .then(function () {
      setTimeout(pollVersion, VERSION_POLL_INTERVAL_MS);
    });
}

/* Waits out the reboot and reports what came back on the other side. The
   board is unreachable for several seconds in the middle, so failed requests
   here are expected and simply retried until the deadline. */
function watchForReboot(before) {
  var deadline = Date.now() + OTA_TIMEOUT_MS;

  function finish() {
    updateInProgress = false;
    elButton.disabled = false;
  }

  function check() {
    if (Date.now() > deadline) {
      setOtaStatus("Timed out waiting for the board to come back", "error");
      finish();
      return;
    }

    fetchVersion()
      .then(function (info) {
        var rebooted = info.uptime_s < before.uptime_s;

        if (!rebooted) {
          setOtaStatus("Downloading and writing to flash...", "warn");
          setTimeout(check, OTA_POLL_INTERVAL_MS);
          return;
        }

        currentBuild = info;
        renderVersion(info);

        if (info.built !== before.built) {
          setOtaStatus("Updated to " + info.version + " on " + info.slot, "ok");
        } else {
          /* Same build after a reboot means the new image failed its
             self-test and the bootloader reverted to this one. */
          setOtaStatus("Rolled back to " + info.version + " on " + info.slot,
                       "error");
        }

        finish();
      })
      .catch(function () {
        /* The board is mid-reboot. Not an error yet. */
        setOtaStatus("Board is restarting...", "warn");
        setTimeout(check, OTA_POLL_INTERVAL_MS);
      });
  }

  setTimeout(check, OTA_POLL_INTERVAL_MS);
}

function startUpdate() {
  if (currentBuild === null) {
    setOtaStatus("Firmware information not loaded yet", "error");
    return;
  }

  elButton.disabled = true;
  updateInProgress = true;
  setOtaStatus("Requesting update...", "warn");

  var before = currentBuild;

  fetch("/api/ota", { method: "POST", cache: "no-store" })
    .then(function (response) {
      if (response.status === 409) {
        throw new Error("an update is already running");
      }
      if (!response.ok) {
        throw new Error("HTTP " + response.status);
      }
      /* 202 only means the request was accepted. Whether an image is
         actually downloaded is decided on the board, which compares the
         version and ELF digest before erasing anything. */
      setOtaStatus("Update requested, watching for a reboot...", "warn");
      watchForReboot(before);
    })
    .catch(function (error) {
      setOtaStatus("Could not start update: " + error.message, "error");
      updateInProgress = false;
      elButton.disabled = false;
    });
}

document.addEventListener("DOMContentLoaded", function () {
  setStatus("Connecting...", null);
  poll();

  elButton.addEventListener("click", startUpdate);

  fetchVersion()
    .then(function (info) {
      currentBuild = info;
      renderVersion(info);
    })
    .catch(function (error) {
      setOtaStatus("Could not read firmware info: " + error.message, "error");
    })
    .then(function () {
      setTimeout(pollVersion, VERSION_POLL_INTERVAL_MS);
    });
});