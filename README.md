# esp32-ota-update

Over-the-air firmware updates for the ESP32, built with ESP-IDF v6.0. The
board runs a WiFi sensor dashboard, downloads a new firmware image over HTTP
into its inactive flash slot, verifies it, reboots into it, and automatically
reverts to the previous image if the new one fails its post-boot health check.

Built on top of [esp32-http-server](https://github.com/emirmusul/esp32-http-server),
which supplies the WiFi station mode, the DHT22 sampling task and the embedded
web dashboard.

![Dashboard during an update](docs/dashboard.png)

## Features

- Two-slot OTA partition layout with `otadata` boot selection
- HTTP download written straight into the inactive slot, no staging buffer
- Version and ELF digest read from the first packet, so an unchanged build
  costs one HTTP request instead of a full erase and write
- SHA256 verification before the boot partition is switched
- Post-boot self-test with automatic rollback to the last working image
- Updates triggered from the dashboard or with a single `curl` call
- Firmware panel showing the running version, slot and image state

## How OTA works here

Application code on the ESP32 executes directly from flash through the MMU
and cache. Erasing the region the running code lives in would leave the CPU
with no next instruction, so an image can never overwrite itself. The flash is
therefore split into two application slots.

An update writes into whichever slot is not running, then records the choice
in `otadata` — a two-sector region holding a monotonically increasing sequence
number. The bootloader reads both sectors, discards any whose CRC fails, and
boots the slot implied by the higher sequence number. Because the new number
is always written into the sector that is not currently authoritative, a power
cut mid-write leaves the old selection intact. There is no window in which no
valid record exists.

## Partition layout

    Name        Type   SubType   Offset      Size
    nvs         data   nvs       0x9000      24 KB
    otadata     data   ota       0xF000      8 KB
    phy_init    data   phy       0x11000     4 KB
    ota_0       app    ota_0     0x20000     1984 KB
    ota_1       app    ota_1     0x210000    1984 KB

Two constraints govern these numbers. App partitions must start on a 0x10000
boundary because the flash cache maps in 64 KB pages, which is why `ota_0`
begins at 0x20000 rather than immediately after `phy_init`. And `otadata` must
be exactly 0x2000 — two sectors, no more, no less.

No `factory` partition is defined. Rollback moves between the two OTA slots and
never consults `factory`, so a third copy would consume a megabyte while giving
a false sense of safety as it grows stale.

## Skipping unnecessary downloads

Every ESP-IDF image carries an `esp_app_desc_t` at a fixed offset, right after
the image header and the first segment header. Reading the first 288 bytes of
the body is therefore enough to learn what the server is offering, before any
sector has been erased.

The comparison uses both the version string and the ELF digest. The string
alone is not enough during development, where rebuilding without bumping
`PROJECT_VER` is common; the digest catches that case. When both match, the
transfer is abandoned and the flash is left untouched.

The bytes consumed while inspecting the header cannot be re-read — TCP has no
rewind — so they are held in the buffer and become the first `esp_ota_write()`
once the decision to proceed is made.

## Rollback

With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` set, a freshly booted image starts
in `PENDING_VERIFY` state. The bootloader will not start a `PENDING_VERIFY`
image twice: if the board resets before the image confirms itself, the slot is
marked `INVALID` and the previous one is selected instead.

The confirmation is the application's responsibility. This project's health
check asks two questions:

- Is the station associated and holding an IP? Without this there is no way to
  push a fixing image, which is the exact failure rollback exists to prevent.
- Has the sensor produced a valid reading within the timeout? That is what the
  board is for.

mDNS is deliberately not checked. The dashboard is reachable by IP when mDNS
fails, so treating it as fatal would revert working firmware.

The sensor check polls rather than sampling once: the sampling task starts
after a 1.5 s delay and individual reads fail occasionally, so a single miss
proves nothing. The default 8 s window allows several attempts.

## What gets validated, and when

Four separate checks guard the update, each catching something the others
cannot:

| Check | Runs | Catches |
|---|---|---|
| `Content-Length` | after the headers | An image too large for the slot |
| Magic word | first 288 bytes | A body that is not an ESP-IDF application |
| SHA256 | `esp_ota_end()` | Corruption introduced during transfer |
| Self-test | after the reboot | A valid image that does not work |

The first three ask whether the file is right; the last asks whether the code
runs. The magic word check exists so a wrong URL or an error page is rejected
before a single sector is erased, rather than after a full write cycle.

## API

    POST /api/ota

    202 Accepted, application/json
    {"status":"update started"}

    409 Conflict, application/json
    {"error":"update already running"}

The response is sent before the download begins. A full update takes roughly
15 seconds and ends in a reboot, so holding the connection open would only make
the client time out.

    GET /api/version

    200 OK, application/json
    {"version":"2.0.0","project":"esp32-ota-update",
     "built":"Sep  7 2026 19:57:48","slot":"ota_1",
     "state":"VALID","elf_sha":"ea2cde7991a4b3c2","uptime_s":142}

The browser polls this to notice that a reboot has happened and that the image
on the other side of it is different. The ELF digest is what distinguishes the
two: an incremental build can carry the same version string and even the same
compile timestamp, so neither is reliable on its own.

    GET /api/sensor

    200 OK, application/json
    {"temperature":26.4,"humidity":61.2,"age_ms":842}

## Build and flash

    idf.py set-target esp32
    idf.py menuconfig      # WiFi credentials, DHT GPIO, mDNS hostname, OTA URL
    idf.py build
    idf.py -p <PORT> flash monitor

`sdkconfig` is not tracked in git because it holds the WiFi password. Project
settings live under "esp32-ota-update Configuration" in menuconfig; the flash
size, partition table and rollback support are pinned in `sdkconfig.defaults`
so a fresh clone builds correctly.

## Performing an update

Serve the build directory from the development machine:

    cd build && python3 -m http.server 8070

Point `OTA firmware URL` in menuconfig at that host and flash once over USB, so
the board learns the address. From then on, bump `PROJECT_VER` in the root
`CMakeLists.txt`, rebuild, and press **Check for update** on the dashboard.
The same thing from a terminal:

    curl -X POST http://esp32-ota.local/api/ota

Rebuilding matters: editing `PROJECT_VER` without running `idf.py build` leaves
the old binary in place, and the board correctly reports that no update is
needed.

## Failure modes, and what happens

| Failure | Behaviour |
|---|---|
| Server unreachable | `ESP_ERR_HTTP_CONNECT`, nothing erased, board keeps running |
| Connection drops mid-download | Handle aborted, slot left incomplete, boot partition unchanged, next attempt succeeds normally |
| Body is not an ESP-IDF image | Rejected on the magic word, before the erase |
| Corrupted download | `esp_ota_end()` fails the SHA256 check, slot never activated |
| Power cut while writing | Old slot still selected; `otadata` is untouched until the end |
| Power cut while writing `otadata` | The other sector is still valid and wins |
| New image fails its self-test | Marked `INVALID`, bootloader reverts to the previous slot |

A dropped connection surfaces two ways. Killing the server sends a TCP reset,
which appears as a negative read; a server that closes cleanly before
`Content-Length` is satisfied gives a zero-length read instead. Both paths call
`esp_ota_abort()`, and the fact that a retry then works is what proves the
handle was released rather than leaked.

Rollback is not proven until it has actually fired. To force it, set the DHT22
GPIO to an unconnected pin and build an image tagged `9.9.9-broken`:

    W self_test: Image is PENDING_VERIFY — this boot decides its fate
    I self_test: WiFi OK: <ssid>, rssi -58, ip 192.168.1.101
    E self_test: No sensor reading within 8000 ms
    E self_test: Health check FAILED — rolling back to the other slot
    I esp_ota_ops: Rollback to previously worked partition.
    I boot: Loaded app from partition at offset 0x210000

The board recovers on its own, with no USB connection.

## Known limitations

- **Verification is integrity, not authenticity.** `esp_ota_end()` compares the
  SHA256 appended at build time against what was written, which catches a
  truncated or corrupted download. It does not stop an attacker who controls
  the server, since they can supply a matching digest. Signed images require
  Secure Boot, with the public key burned into eFuses.
- **HTTP, not HTTPS.** The image travels in the clear over the local network.
- **No anti-rollback protection.** Nothing prevents installing an older image.
  `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` and the `secure_version` field exist for
  that, and are a different concern from the download-skipping above: one is
  security, the other is wear.
- **Sensor reads fail during the update.** Writing to flash disables the cache,
  so code that lives in flash — including the bit-banged DHT22 driver — stalls
  mid-transfer. Four to six timeouts per update are expected; `age_ms` grows and
  the task recovers once the download finishes.
- **A skipped update looks like a timeout in the browser.** When the server
  offers the running build, the board answers 202, decides not to download and
  never reboots. The page waits out its deadline for a restart that will not
  come. Reporting this properly needs a status endpoint the background task can
  write to.
- **The update URL is compiled in.** Changing it requires a USB flash, because
  the running firmware only knows the old address.

## Project layout

    main/
      app_main.c            application entry point and startup order
      boot_info.c/.h        boot slot and image state logging
      ota.c/.h              download, version check, write, verify, boot switch
      self_test.c/.h        post-boot health check and rollback decision
      wifi_sta.c/.h         station mode setup and connection events
      http_server.c/.h      URI handlers and the static asset table
      sensor.c/.h           background sampling task and shared state
      dht.c/.h              bit-banged DHT22 driver
      mdns_service.c/.h     hostname and service advertisement
      www/                  dashboard assets embedded into the firmware
    partitions.csv          two-slot OTA layout
    sdkconfig.defaults      flash size, partition table, rollback support