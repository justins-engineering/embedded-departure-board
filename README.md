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

This branch makes the board a managed PidgeIoT device
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
| Swiftly (native mbedTLS, sec tag 1) | Active: `AmazonRootCA3.cer` = **Amazon Root CA 3 only, as DER** (the efficient native-side encoding, by directive — 442B credential, no PEM parser in the image; PEM parsing was enabled briefly 2026-08-02 for a PEM anchor and deliberately reverted). Full published ATS set staged in `swiftly-ca-full.crt` (PEM, CA 1–4 + Starfield G2, SPKI-verified against amazontrust.com), NOT compiled in | Amazon CA 3 (image has ECDHE_ECDSA + P-256 + SHA-256 only) | Activating the full set is a PACKAGE, not a file swap. Always required: the crypto enablement — RSA (`MBEDTLS_RSA_C`/PSA RSA + ECDHE_RSA) and/or `SECP_R1_384`+`SHA_384` — because the socket's sec-tag check parses every root at setup and one UNPARSEABLE root bricks all Swiftly fetches (bench-verified). Then pick a trust-store form, eyes open: **(A) PEM bundle** — one concatenated credential, needs `CONFIG_MBEDTLS_PEM_PARSE_C` back, and the protected-storage caps (2KB/asset, 8KB partition, bench-verified) force the volatile credential backend or a bigger PS partition; **(B) one DER per sec tag** — no PEM parser (mbedTLS cannot parse concatenated DER in one credential, which is why the bundle is PEM), each root its own credential well under the 2KB/asset cap with a `sec_tag_list` on the socket (see `tls_setup()`), but the 8KB PS partition bounds how many roots fit (~4–5 with overhead + TOC) |

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

**`displays` (added 2026-08-02): the route→display-box layout — the LAST
per-stop compile-time knob.** With `stop_id` and `displays` both
shadow-tunable, one firmware image serves any stop and the per-stop
branch model (`stop_73`, `stop_1670`, …) is fully obsolete. Shape:

```json
"displays": [
  { "r": "R29", "d": "1", "p": 0 },
  { "r": "38",  "d": "1", "p": 1 },
  { "r": "B43", "d": "0", "p": 2 }
]
```

`r` = route id (≤4 chars), `d` = Swiftly directionId code (exactly one
char), `p` = physical box position (0–5). Whole-array replacement, never
a per-entry merge; any invalid entry rejects the entire array (a half
layout is worse than the old one). Omitted key keeps the current mapping
(boot default: the compile-time `DISPLAY_BOXES` table). The ack ALWAYS
echoes the effective layout. Per-box color/brightness stay compile-time
on purpose — brightness is a per-box power cap the shadow must never be
able to raise (`update_stop.h`).

**Verified end-to-end 2026-08-02** (shadow v4): stop 1670's real 3-route
layout applied and echoed back exactly; unmapped positions stay dark by
construction (`update_routes` turns every box off at the top of each pass
and writes only mapped positions).

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

### Verification status (closed out 2026-08-03)

Everything in the original bench plan has now been verified live:
console (wiring + the TF-M-silence release-boot fix), shadow-driven
`stop_id`/knob config with Kconfig fallbacks, batched telemetry at 300s
cadence, dictionary log shipping, the shadow-push round trip
(apply → as-applied ack → server-side convergence), the `displays`
layout key, and a full FOTA cycle: catalog upload → shadow `firmware`
target → 45m33s chunked download over LTE-M **with the day job running
throughout** → MCUboot test swap (~49s) → 0.13.2 boot → deferred confirm
on the first departure fetch → running-version ack 14s after boot.
Post-OTA soak: ~31h, 1513 telemetry reports at unbroken cadence.

#### OTA lessons (2026-08-02, all measured on hardware)

- **Pacing**: ~2.9s per 512B chunk under real LTE-M contention — a
  ~485KB image takes ~45 min, not the naive bandwidth estimate. Budget
  OTA windows accordingly; late-evening/off-peak windows likely help
  (the link visibly degraded — DNS timeouts on all hosts — after ~35 min
  of sustained transfer, consistent with carrier-side throttling).
- **The day job and a download fight for the link.** Without mitigation
  the sign's reset-on-fetch-failure policy rebooted the board
  mid-download three times in a row (burning the persisted 3-attempt
  budget). Two-part fix, both live-verified: main softens (never
  removes) the reset policy while `pigeon_client_fota_active()` — an
  atomic flag **bounded by a 60-min self-expiring ceiling**, sized from
  the measured pace, so a wedged download can never disable
  self-recovery — and `CONFIG_PIGEON_FOTA_CHUNK_YIELD_MS=100` (pigeon
  `174316e`) gives the day job periodic airtime. In the passing run the
  suppression absorbed 9 Swiftly hard-fail cycles with zero reboots.
- **Attempt accounting**: the per-version NVS counter is the real retry
  bound — the 30-min in-RAM holdoff is wiped by any reboot (by design;
  the counter, not the holdoff, is what stops reboot-loops). A version
  that burns its 3 attempts is refused until the shadow targets a
  DIFFERENT version string.
- **Artifact-drift rule, binding from now on**: exactly one artifact per
  version string, ever. The catalog's `0.13.2` (485719 B, `4d3aa3d4…`)
  is authoritative and is what the board runs; the git tip builds a
  different `0.13.2` binary that must never be uploaded — the next
  shipped image is **`0.13.3`**.
- **Sequencing** (by design, verified): a poll applies config keys
  (stop_id, knobs, displays) BEFORE starting a firmware download, and
  the ack for a firmware target is only ever sent by the image actually
  running that version.

#### Proposed next (not implemented)

- **FOTA download resume** (pigeon repo, 0.13.3-era): persist
  `{version, offset, sha256-stream-state}` across attempts, query the
  written offset from `flash_img`/`dfu_target` on init, Range-GET from
  the persisted offset, invalidate on version change. Today every
  attempt restarts from byte 0 — the first failed 0.13.2 attempt threw
  away 351KB (72%) of already-paid-for LTE data.
- **Soften the day-job reset policy generally**: the 31h soak logged 8
  Swiftly-double-failure reboots (each recovering in ~15s). Recovery
  works, but a bounded retry/backoff before rebooting would cut the
  churn; product call.

#### Remaining open items (ranked)

1. ~~RAM is effectively full~~ CLOSED 2026-08-08: measured-watermark
   diet landed (see the `RAM diet:` commit) — debug 89.83% / release
   89.73%, evidence table in the commit message.
2. ~~Latent jsmn token-budget gap~~ CLOSED 2026-08-08: jsmntok_t packed
   to 8B funds the full 934-token max-shape budget (see the
   `swiftly: pack jsmn tokens` commit). Residual, by config design: a
   stop served by MORE than `STOP_MAX_ROUTES` (5) routes still
   token-exhausts and fails the fetch cycle — raising it costs
   ~187 tokens (~1.5KB of main stack) per extra route.
3. Occupancy data (`occupancyPercent`/`occupancyCount`) is parsed into
   the model (`Destination.occupancy_{percent,count}`, -1 = absent —
   optional per-vehicle APC fields, present only when the predicted
   vehicle reports passenger counts; `occupancyStatus`, the string-enum
   sibling, stays accept-and-ignore). NOT yet surfaced anywhere:
   display use and/or telemetry export are open product decisions.
4. First shadow poll right after LTE attach can fail parse (-22,
   partial body) and self-heals on the next cycle — observed on the
   bench 2026-08-08, cosmetic at the 5-min poll cadence. Candidate for
   root-cause during the 0.13.4 leg (smells like recv-loop termination
   during slow first RRC/TLS warm-up).
5. Upstream filings worth making: the TF-M `TFM_LOG_LEVEL_SILENCE`
   boot-kill (minimal repro exists: single-flag A/B on this board) and
   pigeon's chunk-yield rationale.
6. Swiftly full ATS trust-set activation is a documented package (see
   the rotation table): crypto enablement + volatile credential backend
   or bigger PS partition.
7. Release-profile console shows only WRN+ — fine for the field;
   `LOG_DEFAULT_LEVEL=3` needs the log-thread stack raised to 2048 in
   the same change if ever wanted.

## Creating a Release
Update the [VERSION file](https://github.com/umts/embedded-departure-board/blob/main/app/VERSION).
On a successful push to the main branch the [release workflow](https://github.com/umts/embedded-departure-board/blob/main/.github/workflows/release.yml) will; create a new release, generate release notes, and upload the freshly built hex/bin files to the release.
