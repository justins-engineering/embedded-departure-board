# embedded-departure-board
## Development
### Requirements
- Python 3
- [Zephyr SDK](https://github.com/zephyrproject-rtos/sdk-ng/releases)
- [32 bit Swiftly API key](#building)

### Setup
1. Clone and enter the repo
2. Create a Python virtual environment
   ```sh
   python3 -m venv ./.venv
   ```
4. Activate your Python virtual environment
   ```sh
   source .venv/bin/activate
   # Or, if you're a direnv user, leave and come back
   cd
   cd -
   ```
6. Install West
   ```sh
   pip install west
   ```
8. Run
   ```sh
   west update
   ```
9. Install Python dependencies
   ```sh
   pip install -r zephyr/scripts/requirements.txt
   pip install -r nrf/scripts/requirements.txt
   pip install -r bootloader/mcuboot/scripts/requirements.txt
   ```
### Recomended
- Read the [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) to install the required development tools (system packages, Zephyr SDK, and udev rules).

- To save on space, you may want to intall a minimal bundle Zephyr SDK [release](https://github.com/zephyrproject-rtos/sdk-ng/releases) and the arm-zephyr-eabi toolchain. The full release includes all avaiable toolchains.

- Read the [Introduction to the nRF9160 Feather](https://docs.circuitdojo.com/nrf9160-introduction.html) to better understand the dev board we are using.

- If you plan making changes to the LTE modem configuration you should understand the various modes: [Maximizing battery lifetime in cellular IoT: An analysis of eDRX, PSM, and AS-RAI](https://devzone.nordicsemi.com/nordic/nordic-blog/b/blog/posts/maximizing-battery-lifetime-in-cellular-iot-an-analysis-of-edrx-psm-and-as-rai)

### Docker
Zephyr supplies various [Docker images](https://github.com/zephyrproject-rtos/docker-image#zephyr-docker-images) for development.

Our Github Actions [build workflow](https://github.com/umts/embedded-departure-board/blob/main/.github/workflows/build_test.yml) uses the Base Image (ci-base).


## Building
Currently this build requires a 32 bit [Swiftly](https://www.goswift.ly/) API key placed in `${CMAKE_CURRENT_SOURCE_DIR}/keys/private/swiftly-api.key` (Wrapped in `""`). If that key file does not exist CMake will create it with a fake key to allow the build to succeed.

If you are not using a [pre-signed binary](https://github.com/umts/embedded-departure-board/releases/latest) and you would like a functioning API key, [you can request one here](https://swiftly.zendesk.com/hc/en-us/requests/new?ticket_form_id=33738491027469).

### Testing

```sh
west build --sysbuild ./app -b circuitdojo_feather/nrf9160/ns
```
### Release

**You need a copy of the MCUBoot key file placed here:** `app/keys/private/boot-ecdsa-p256.pem`
```sh
west build --sysbuild ./app -b circuitdojo_feather/nrf9160/ns -- -DFILE_SUFFIX=release
```

## Programming
### Flashing
Flashing the device with an external programmer is quicker than using a bootloader. More importantly, it's the easiest way (and currently the only tested way) to secure the bootloader, update the modem firmware, and use the cortex-debugger.

#### Requirements
- External programming device, the [nRF5340 Dk](https://www.nordicsemicom/Products/Development-hardware/nRF5340-DK) is what we currently use
- 6-pin [Tag Connect cable](https://www.tag-connect.com/product/tc2030-ctx-nl-6-pin-no-legs-cable-with-10-pin-micro-connector-for-cortex-processors)
- [J-Link](https://www.segger.com/downloads/jlink/) software
- [nRF Util](https://www.nordicsemi.com/Products/Development-tools/nRF-Util)

#### Running
```sh
west flash -r nrfutil --erase --softreset
```

### Uploading via the bootloader
The only way to flash the device without an external programming device, uses serial over the builtin USB port.

#### Requirements
- [Newtmgr](https://mynewt.apache.org/latest/newtmgr/install/index.html)

#### Setup
1. Create a "serial" profile in newtmgr:
```sh
newtmgr conn add serial type=serial connstring="dev=/dev/ttyUSB0,baud=1000000"
```
2. Put the device into bootloader mode.

#### Run
```sh
newtmgr -c serial image upload ./build/app/zephyr/zephyr.signed.bin
```

## VSCode
This repo includes `.vscode/tasks.json` to make develpoment easier. The included tasks are:
- Build
- Load image via bootloader
  - Expects a connection profile named "serial" in `newtmgr`
- Serial Monitor
  - Uses `pyserial-miniterm`, which should already be installed in your venv
  - Assumes the device is connected at `/dev/ttyUSB0`
- West Flash
- West Flash and Monitor
- Build AT Client
  - Builds the At Client sample provided by nrf.
  - Useful for debugging via AT commands. Use a serial console to send AT commands

## PidgeIoT integration (`pigeon-integration` branch) — bench-day handoff

This branch bumps NCS to 3.4.0 and makes the board a managed PidgeIoT device
— see the branch's commit messages for the full design writeups (they carry
the file:line traces). History: 2026-07-29 synced pigeon `9bb5117`→`0db1214`
and settled the `CONFIG_PIGEON_WATCHDOG`-off decision (see
`app/prj_release.conf`'s comment); 2026-07-30 batched the telemetry cycle
into one POST (pigeon `e0cba00`).

**Refreshed 2026-08-01** — four new capabilities landed, each its own
commit (offline resilience, shadow-tunable runtime knobs, dictionary log
upload, FOTA), plus two commits in the `pigeon` library itself. Fresh
artifacts for the bench: **`build_rebase_debug/` and
`build_rebase_release/`** (pigeon `53c669e`; both profiles built clean
after every commit). Older `./build`, `build_debug/`, `build_release/`
dirs predate all of this.

### Offline-resilience guarantees (task #4 — audit + fixes, 2026-08-01)

The sign's day job (Swiftly fetch → display) no longer shares a thread,
a TLS stack, or a watchdog window with PidgeIoT:

- All pigeon I/O runs on a dedicated prio-10 thread; main (prio 0) preempts
  it at will. Boot arms the display timer before `pigeon_client_init()`,
  which no longer blocks (its first shadow sync happens on the thread). An
  unreachable `api.pidgeiot.com` cannot delay boot, stall the display loop,
  or starve the 60s hardware watchdog.
- Pigeon TLS runs **inside the modem** (CA provisioned via
  `modem_key_mgmt` under sec tag 2, compare-before-write); Swiftly stays on
  native mbedTLS. This fixed a latent 100%-failure (the modem cert store
  was empty — pigeon TLS could never have handshaken as previously built)
  AND removed the only shared resource (the mbedTLS heap is sized for
  one connection). The provisioned credential is a **three-root PEM
  bundle** (`keys/public/pigeon-ca.crt`: GTS Root R4 + ISRG Root X1 + X2):
  Cloudflare doesn't guarantee a CA across ~90-day renewals, and a
  GTS-only store would kill all pigeon TLS — including FOTA, the only
  remote fix path — fleet-wide if it reissued under Let's Encrypt. Must
  stay PEM; the modem `%CMNG` store rejects DER (bench-found, see git
  history).
- Failed pigeon cycles back off to 2x then a capped 4x of the poll
  interval; one successful round trip resets it. Shadow `"reboot": true`
  fires only after its ack lands (no reboot loops against a half-reachable
  platform). `CONFIG_PIGEON_REBOOT_ON_FATAL` was audited: it only overrides
  `k_sys_fatal_error_handler()`, which connectivity errors cannot reach.
- Main's loop idles via `k_sleep()` now, not `k_cpu_idle()` — the old spin
  starved every lower-priority thread (this was also silently blocking any
  deferred-mode logging from ever flushing).

#### CA rotation survivability (trust bundles, 2026-08-02)

Both TLS paths carry multi-root PEM bundles so a CA rotation at renewal
is not an automatic outage. What each image can actually verify TODAY
(from the built `.config`, not the datasheet):

| Path | Bundle (`keys/public/`) | Usable today | Inert until config change |
|---|---|---|---|
| Pigeon (modem TLS, sec tag 2) | `pigeon-ca.crt`: GTS R4, ISRG X1, ISRG X2 | **all three** (modem has RSA + P-256 + P-384) | — |
| Swiftly (native mbedTLS, sec tag 1) | Active: `swiftly-ca.crt` = **Amazon Root CA 3 only**. Full published ATS set staged in `swiftly-ca-full.crt` (CA 1–4 + Starfield G2, SPKI-verified against amazontrust.com), deliberately NOT compiled in | Amazon CA 3 (image has ECDHE_ECDSA + P-256 + SHA-256 only) | Activating `swiftly-ca-full.crt` is a PACKAGE, not a file swap: the socket's sec-tag check parses every root at setup and one UNPARSEABLE root bricks all Swiftly fetches (bench-verified) — so RSA (`MBEDTLS_RSA_C`/PSA RSA + ECDHE_RSA) and/or `SECP_R1_384`+`SHA_384` must land in the SAME change, plus the protected-storage caps (2KB/asset, 8KB partition, bench-verified too) require the volatile credential backend or a bigger PS partition |

So: a Cloudflare CA switch on the PidgeIoT side is a non-event; an Amazon
switch away from CA 3 (to RSA or P-384) still needs a Kconfig/TF-M crypto
change — but only that, the trust store is already ready. No GTS root in
the Swiftly bundle: no CT or documentation evidence was found that
api.goswift.ly ever served a Google chain (checked 2026-08-02; crt.sh was
down, Google CT API retired, web search clean — re-check crt.sh if this
ever becomes load-bearing).

### Shadow `target_config` schema (task #1)

`stop_id` is required for a config to apply; everything else is optional —
an omitted key keeps the value currently in force (last applied, or the
Kconfig default from boot). Out-of-range values are clamped and the ACK
reports the clamped (i.e. real) value. Full example:

```json
{
  "stop_id": "73",
  "telemetry_interval": 300,
  "update_stop_interval": 30,
  "ntp_server_primary": "time.nist.gov",
  "ntp_server_fallback": "us.pool.ntp.org",
  "ntp_timeout_ms": 4000,
  "ntp_retry_count": 2,
  "http_retry_count": 1,
  "reboot": false,
  "firmware": { "version": "0.13.1", "size": 250000, "sha256": "<64 hex>" }
}
```

Clamps: `update_stop_interval` 5–45s (the ceiling is the 60s hardware
watchdog window — see `net/pigeon_client.c`), `ntp_timeout_ms` 500–30000,
`ntp_retry_count` 1–5, `http_retry_count` 0–3. `reboot` is a one-shot
command, never echoed in the ack. `firmware` drives FOTA (below).

### Remote dictionary logs (task #2)

Both profiles now run `CONFIG_LOG_MODE_DEFERRED` (minimal mode never
dispatches to log backends at all) with `CONFIG_PIGEON_LOG_UPLOAD=y`:
dictionary-encoded records batch up and POST to the pigeon's `/logs` ring
buffer (≤5min cadence, 256B batch threshold; failed uploads drop that
batch by design — this is an opportunistic debug channel). **The bench
console stays human-readable in BOTH profiles** — dictionary encoding is
per-backend, and the UART text backend is still enabled (verified in both
built `.config`s); output is just flushed asynchronously now.

Decoding needs the build's dictionary artifact:
**`build_rebase_<profile>/app/zephyr/log_dictionary.json`** — regenerated
every build and only valid for the image from that exact build; treat it
as part of the flashed artifact set. To read logs in the dashboard, open
the pigeon's Log Viewer and upload that file (platform task #5, live as of
2026-08-01: `PUT /pigeons/:id/log-dictionary`, decoded stream renders
inline with a "Decoded .txt" download; re-upload after every reflash).
Host-side alternative: `zephyr/scripts/logging/dictionary/log_parser.py`.

### FOTA (task #3)

Shadow `firmware` target → chunked (1KiB) device-authed Range download
into the external-flash MCUboot secondary slot → streamed sha256 verify →
one-time test swap → graceful reboot. Runs on the pigeon thread; the
display keeps updating throughout. Do not raise
`CONFIG_PIGEON_FOTA_CHUNK_SIZE` past 2048 on this board (pigeon rides
modem TLS; large responses hit the 2k modem recv limitation from the 2024
JES_FOTA attempt).

Test procedure (use the test pigeon, never the production one):

1. Bump `app/VERSION` (e.g. PATCHLEVEL → `0.13.1`). CMake bakes this into
   `CONFIG_PIGEON_FOTA_CURRENT_VERSION` automatically (generated conf
   fragment — do NOT set it by hand; under sysbuild a plain CMake
   `set(CONFIG_…)` is silently ignored, which is why it works this way).
2. Build the release profile; the OTA artifact is
   `build_rebase_release/app/zephyr/zephyr.signed.bin`.
3. Upload it to the flock's firmware catalog in the dashboard under the
   version string **exactly** `0.13.1` (must match step 1 byte-for-byte).
4. Assign it to the pigeon (the dashboard's firmware-assign patches just
   the shadow's `firmware` key).
5. Within one poll interval the sign logs
   `FOTA: downloading 0.13.1 (... attempt 1/3)`, streams ~200 chunks, then
   `FOTA: image staged -- rebooting into MCUboot test swap`.
6. The new image boots as a TEST swap. It becomes permanent only after its
   first successful departure fetch (`confirm_image_if_healthy()`,
   `fota.c`); its next shadow poll then acks the firmware key and the
   dashboard shows converged. **The old image never pre-acks** — a
   reverted swap leaves the shadow visibly unconverged, which is the truth.
7. Safety rails to know about: per-version download attempts are capped at
   3 and persist across reboots (settings/NVS `edb/fota/attempts`), so a
   boot-looping image burns ≤3 downloads and stops; transient failures
   take a 30min holdoff; a version change resets the budget.

Two bench-behavior changes from the deferred confirm:

- `newtmgr`-uploaded images are also test swaps now: an uploaded image
  that never completes a departure fetch (e.g. no LTE at the bench)
  REVERTS on the next reset. External-programmer (`west flash`) images are
  unaffected.
- The baked-token gotcha still applies: an OTA artifact carries
  `CONFIG_PIGEON_TOKEN` from build time — rotate the token and the staged
  image 401s forever; rebuild, don't re-upload.

### West-update fragility (READ before any `west update`)

There are **zero required local patches to west-managed files** as of
2026-08-01: the old `zephyr/soc/nordic/Kconfig` patch turned out not to
be needed and was reverted (fresh clean-dir builds of both profiles
confirm), and the old pigeon printk patch is upstream since pigeon
`c9790f8`. One thing remains fragile:

**The workspace `pigeon/` checkout is TWO COMMITS AHEAD of its GitHub
`main`** (`ef5d980` SOCK_NATIVE_TLS opt-in — available but not used by
this board; `53c669e` `CONFIG_PIGEON_SHADOW_CONFIG_MAX` — REQUIRED,
both prj confs set it to 512). They exist only in `/home/justin/pigeon`
(local commits, not pushed — pushing is your call). Until they're
pushed, a `west update` reverts `pigeon/` to `c9790f8` and the build
fails loudly on the unknown `CONFIG_PIGEON_SHADOW_CONFIG_MAX` symbol;
recover with:
`cd pigeon && git fetch /home/justin/pigeon main && git checkout FETCH_HEAD`

### Still outstanding for the bench

1. **Console wiring physical check** (unchanged from the last handoff):
   flashing works, but neither this firmware nor a stock hello_world
   produces a byte on `/dev/ttyUSB0` — two independent images equally
   silent points at the TX→CP2102N wiring, not software. Check first.
2. **Reflash before verifying** — the board still carries the 2026-07-30
   pre-refresh release image. Flash from `build_rebase_release/` (or
   `_debug/`).
3. **RAM is effectively full**: 98.8% (debug) / 98.7% (release) of the
   app image's 128K region. Any new static allocation needs an explicit
   offset; the levers already spent are listed in the task #2/#3 commit
   messages.
4. Verification sequence once console works: boot → LTE attach → shadow
   sync (`Stop ID updated to:`) → the four telemetry keys landing batched
   in the dashboard → push a new `stop_id` and watch the sign re-stop →
   push an `update_stop_interval`/NTP change and watch the ack report the
   applied values → upload `log_dictionary.json` and see decoded logs →
   the FOTA procedure above → (deliberate) revert test: pull the antenna
   after a swap boots and confirm MCUboot reverts on reset instead of
   confirming a sign that can't fetch departures.

## Creating a Release
Update the [VERSION file](https://github.com/umts/embedded-departure-board/blob/main/app/VERSION).
On a successful push to the main branch the [release workflow](https://github.com/umts/embedded-departure-board/blob/main/.github/workflows/release.yml) will; create a new release, generate release notes, and upload the freshly built hex/bin files to the release.
