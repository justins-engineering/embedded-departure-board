# PidgeIoT integration

This describes how the departure board works as a managed device on the
[PidgeIoT](https://pidgeiot.com) platform: what it sends, what the platform can
change on it, and what happens when the platform is unreachable.

The integration lives on the `pigeon-integration` branch rather than on `main`.
It adds the [`pigeon`](https://github.com/justins-engineering/pigeon) Zephyr
library as a west module and a client around it in `app/src/net/pigeon_client.c`.
Everything below refers to that branch. Configuration symbols named
`CONFIG_PIGEON_*` belong to the library; `CONFIG_PIGEON_CLIENT_*` are this
application's own.

## Contents

- [What the board sends](#what-the-board-sends)
- [Isolation from the day job](#isolation-from-the-day-job)
- [TLS and trust](#tls-and-trust)
- [Shadow configuration](#shadow-configuration)
- [The display gate](#the-display-gate)
- [Route to display box mapping](#route-to-display-box-mapping)
- [Fetch failures and self recovery](#fetch-failures-and-self-recovery)
- [Telemetry](#telemetry)
- [Log surfaces](#log-surfaces)
- [Firmware over the air](#firmware-over-the-air)
- [LTE power configuration](#lte-power-configuration)
- [What is still built per sign](#what-is-still-built-per-sign)
- [Release artifacts](#release-artifacts)

## What the board sends

Everything the sign sends is mobile originated. Nothing pages it: there is no
WebSocket connector on this board and no server push, so a configuration change
reaches the sign on its next poll rather than immediately.

| Traffic | Cadence | Direction |
| --- | --- | --- |
| Departure predictions fetch | 30 s, tunable 5 to 45 s | to the predictions API |
| Shadow poll and telemetry report | 300 s, tunable | to PidgeIoT |
| Dictionary log batch | up to every 5 min, or once 256 bytes have accumulated | to PidgeIoT |
| Firmware download | rare, a sustained chunked transfer | from PidgeIoT |

A poll is a small burst of back to back requests: a shadow GET, a telemetry
POST, and sometimes a shadow report or a log POST. `CONFIG_PIGEON_CLIENT_POLL_INTERVAL_SECONDS`
sets the interval used until the shadow supplies its own value. It is the one
knob that cannot wait for the shadow, because it paces the very sync that
delivers the shadow.

## Isolation from the day job

The sign's day job, fetching departures and driving the displays, shares no
thread, no TLS stack and no watchdog window with PidgeIoT. An unreachable
platform cannot delay boot, stall the display loop, or starve the hardware
watchdog.

- All PidgeIoT I/O runs on a dedicated priority 10 thread. Main runs at priority
  0 and preempts it at will.
- Boot arms the display timer before the client starts, and the client's
  initialisation does not block; its first shadow sync happens on its own thread.
- Main's loop idles in `k_sleep()` rather than spinning, so lower priority
  threads, the client's included, actually run. A spin here also silently
  prevents deferred mode logging from ever flushing.
- Failed cycles back off to twice, then a capped four times, the poll interval.
  One successful round trip resets the backoff.
- A shadow carrying `"reboot": true` reboots only after its acknowledgement has
  been delivered, so a half reachable platform cannot produce a reboot loop.

`CONFIG_PIGEON_CLIENT_THREAD_STACK_SIZE` sizes that thread for the library's
HTTPS client plus a JSON shadow decode, not for a TLS handshake: the sockets are
offloaded, so the handshake runs inside the modem and never on this stack. The
FOTA path stacks `dfu_target` and `flash_img` frames on top and is the case to
size against.

## TLS and trust

The two network users on this board deliberately do not share a TLS
implementation:

| Path | Implementation | Credential |
| --- | --- | --- |
| PidgeIoT | offloaded, inside the modem | sec tag 2, provisioned via `modem_key_mgmt`, compare before write |
| Predictions fetch | native mbedTLS | sec tag 1 |

Splitting them removes the only shared resource, since the mbedTLS heap is sized
for a single connection.

Both paths carry multi root trust bundles so that a certificate authority change
at renewal is not an automatic outage. The PidgeIoT bundle holds three roots,
because the edge in front of the platform does not guarantee a fixed authority
across renewals, and a single root store would take out every managed device at
once, including the firmware update path that is the only remote way to fix it.

Two constraints worth knowing before changing a trust store:

- The modem's credential store accepts PEM and rejects DER, so the PidgeIoT
  bundle must stay PEM.
- The native path's socket parses every root in a credential at setup, so one
  unparseable root breaks every fetch rather than just the chain that needed it.
  mbedTLS cannot parse concatenated DER in a single credential, so widening that
  trust store means either enabling the PEM parser or giving each root its own
  sec tag and listing them on the socket.

Widening the native trust store is a package rather than a file swap: it also
needs whatever key types and curves the new roots use to be compiled in, and the
protected storage partition bounds how many credentials fit.

## Shadow configuration

The platform holds a desired `target_config` for each device. The board fetches
it, applies what it can, and acknowledges what it actually ended up running. The
acknowledgement reports the effective value, so a clamped or rejected setting is
visible on the dashboard rather than silently different from what was pushed.

```json
{
  "stop_id": "00000",
  "displays": [
    { "r": "RT1", "d": "0", "p": 0 },
    { "r": "RT2", "d": "1", "p": 1 },
    { "r": "RT3", "d": "0", "p": 2 }
  ],
  "telemetry_interval": 300,
  "update_stop_interval": 30,
  "http_retry_count": 1,
  "reboot": false,
  "firmware": { "version": "0.0.0", "size": 250000, "sha256": "<64 hex>" }
}
```

- `stop_id` is required for a configuration to apply.
- An omitted key keeps the value currently in force, whether that came from an
  earlier configuration or from the compiled default. `displays` is the one
  exception; see below.
- Out of range values are clamped rather than refused: `update_stop_interval` to
  5 to 45 seconds, `http_retry_count` to 0 to 3. The ceiling on the fetch
  interval exists to keep main's watchdog feeding loop inside
  `CONFIG_MAX_TIME_INACTIVE_BEFORE_RESET_MS`.
- `reboot` is a one shot command and is never echoed in the acknowledgement.
- Unknown keys are skipped by the decoder, so a shadow still carrying retired
  keys applies cleanly.

The whole document must fit `CONFIG_PIGEON_SHADOW_CONFIG_MAX`. A document over
that cap is refused rather than truncated, and so is a `displays` array with more
entries than the board has capacity for: too many entries fails the array decoder
before any key is extracted, so the configuration is refused with no `stop_id`
either.

**Boot apply.** The first successful fetch after boot applies `target_config`
even when the platform already considers the shadow converged. Convergence is the
platform's memory of what some past image acknowledged, which a freshly flashed
or replaced board does not share; without this, such a board would run compiled
defaults indefinitely under a dashboard reporting success. Two guards ride along:
the `reboot` one shot is masked on that path, so a long acknowledged reboot flag
cannot fire on every boot, and a shadow nothing has ever been pushed to is
skipped outright.

## The display gate

A board lights nothing until the platform has told it both which stop it serves
and how that stop's routes are laid out across its boxes. Until the first full
configuration lands, the displays stay dark and each pass is skipped without
counting as a failure, so the watchdog is still fed and a sign waiting on its
first sync does not look like a failing one.

**The trade is deliberate: a sign that cannot reach the platform at boot stays
dark indefinitely.** There is no timeout fallback to the compiled default and no
persistence across reboots. Both were considered and rejected. A compiled default
may name another sign's stop, which is exactly the bad data the gate exists to
prevent, and a persisted stop goes stale the moment a sign is repointed from the
dashboard. No data beats wrong data on a public sign.

The compiled fetch interval stays armed from boot even though those early passes
are skipped, because the skipped passes are where main feeds the watchdog. A sign
waiting out a long platform outage has to stay alive in order to sync at all.

## Route to display box mapping

`displays` carries the layout: `r` is the route id, at most four characters, `d`
is the single character direction code the predictions API uses, and `p` is the
physical box position. Positions run from 0 to one less than the board's display
box capacity, which is fixed by how many display switches the board overlay wires
rather than by a build option, and is asserted at compile time against both the
switch table and the per box parameter table.

**The mapping's positions are the active set.** Every pass sweeps all boxes off
at the top and then writes only the positions a matched entry names, so a box no
entry names is never turned on. A sign with fewer physical panels than the board
has switch outputs therefore needs no build of its own; it simply names fewer
positions, and a switch with no panel behind it enables nothing. Positions need
not be contiguous.

The array is replaced whole, never merged per entry, and validation is all or
nothing:

- A **present but invalid** array refuses the entire configuration: no stop, no
  interval changes, and on a sign that has not synced yet the gate stays shut.
  Applying the stop while keeping the previous layout would put the right stop's
  departures on the wrong boxes with nothing on the sign saying so.
- An **omitted** key keeps the current mapping, but only once there is one to
  keep. On a sign that has had no mapping applied this boot the compiled table
  does not count, because it describes whichever sign the firmware was built for
  rather than this one. So `displays` is required on a sign's first sync of a
  boot and optional afterwards.

The acknowledgement always echoes the effective layout, including on the refusal
paths, so a rejected push shows up as a shadow that will not converge.

Per box color and brightness stay compile time on purpose. Brightness is a power
cap that keeps a box with every LED lit inside its current limit, so the shadow
must be able to remap routes to boxes but never to raise a box's drive current.

## Fetch failures and self recovery

- **Retries are spaced, not immediate.** `CONFIG_HTTP_REQUEST_RETRY_BACKOFF_MS`
  waits before the first retry and doubles up to
  `CONFIG_HTTP_REQUEST_RETRY_BACKOFF_MAX_MS`. Upstream failures arrive in
  clusters, so an instant retry reissues the request inside the same disturbance
  that just rejected it. The retry loop feeds the watchdog at the top of every
  attempt, so the interval that has to stay inside the watchdog window is one
  attempt plus one backoff rather than the sum of them. A range continuation
  retry during a firmware download is not delayed, since a partial transfer is
  progress rather than a failure.
- **A reset takes a streak, not a single failure.**
  `CONFIG_UPDATE_FAILURES_BEFORE_RESET` consecutive failed cycles are required. A
  reset costs a full LTE reattach, which leaves the sign dark for longer than a
  lone upstream hiccup would have. Below the threshold the display holds its last
  state and the loop keeps feeding the watchdog, so a genuinely wedged device is
  still caught. The counter is reported as telemetry.
- **The modem restarts itself out of a resolver wedge.** After
  `CONFIG_DNS_FAILURES_BEFORE_MODEM_RESTART` consecutive fetches failing inside
  `getaddrinfo()`, the modem is cycled through offline functional mode and back.
  This exists for an observed failure where the resolver returned `EAGAIN` for
  many hours and nothing short of a modem restart cleared it. Only the
  predictions fetch feeds this count, since the platform client resolves a
  different host on its own schedule. In ordinary operation the reset policy
  above fires long first; the window this is really for is a firmware download,
  where that reset policy is deliberately suppressed and this is the only self
  recovery the device has.

One architectural hazard is worth stating because it is not visible from either
side alone. The board runs two independent TLS clients against one modem, on
different threads and sharing no lock. The modem permits several concurrent TLS
sessions but only one handshake at a time, and reports a violation as a spurious
"credential not found" on an otherwise valid sec tag. The library serialises its
own transport against itself and knows nothing about the application's client.

## Telemetry

Each poll reports a batch of values in one request rather than one request per
value. `CONFIG_PIGEON_TELEMETRY_MAX_KEYS` bounds how many keys a report carries.
The platform stores latest value per key, so a key keeps its own timestamp until
the next report that mentions it.

## Log surfaces

Both build profiles run `CONFIG_LOG_MODE_DEFERRED` with
`CONFIG_PIGEON_LOG_UPLOAD` enabled. Dictionary encoded records batch up and POST
to the device's ring buffer on the platform, bounded by
`CONFIG_PIGEON_LOG_UPLOAD_BUF_SIZE` and flushed at latest every
`CONFIG_PIGEON_LOG_UPLOAD_MAX_INTERVAL_MS`. A failed upload drops that batch by
design: this is an opportunistic debug channel, not a durable log store.

Minimal log mode never dispatches to log backends at all, which is why deferred
mode is required for this to work. The bench console stays human readable in both
profiles, because dictionary encoding is per backend and the UART text backend is
still enabled; its output is just flushed asynchronously now.

**Decoding needs the dictionary artifact from the exact build that produced the
image.** `log_dictionary.json` is regenerated on every build and shares almost no
string addresses with its predecessor, so an older dictionary decodes a newer
image's logs into plausible looking nonsense rather than failing. Treat it as part
of the flashed artifact set, and re-upload it to the platform after every reflash.
The alternative is decoding host side with
`zephyr/scripts/logging/dictionary/log_parser.py`.

The release profile's console shows warnings and above. Raising the default log
level needs the log thread's stack raised in the same change.

## Firmware over the air

A `firmware` key in the shadow names a version string, an expected size and a
sha256. The device compares that version against its own and, if they differ,
downloads the image in Range requests into the MCUboot secondary slot in external
flash, verifies the sha256 as it streams, stages a one time test swap, and
reboots. The download runs on the client thread, so the displays keep updating
throughout.

- **Chunk size.** `CONFIG_PIGEON_FOTA_CHUNK_SIZE` must not go past 2048 on this
  board. The transfer rides modem TLS, and oversized responses hit the modem's
  receive limit.
- **The day job and a download compete for the link.** Two mitigations, both
  needed: the reset policy is softened, never removed, while a download is
  active, and `CONFIG_PIGEON_FOTA_CHUNK_YIELD_MS` gives the day job airtime
  between chunks. The active flag carries a self expiring ceiling of 60 minutes,
  sized from the measured transfer pace, so a wedged download can never disable
  self recovery permanently.
- **Attempt budget.** Downloads per version are capped and the counter persists
  across reboots in settings storage, so an image that boot loops burns its
  budget and stops rather than retrying forever. Transient failures take a
  holdoff, but the holdoff is in RAM and a reboot wipes it; the persisted counter,
  not the holdoff, is what bounds retries. Targeting a different version string
  resets the budget.
- **Confirmation is deferred.** A new image boots as a test swap and becomes
  permanent only after it completes a successful departure fetch. Its next poll
  then acknowledges the firmware key. The old image never acknowledges on the new
  one's behalf, so a reverted swap leaves the shadow visibly unconverged, which is
  the truth.
- **Sequencing.** A poll applies configuration keys before starting a download,
  and the acknowledgement for a firmware target is only ever sent by the image
  actually running that version.
- **Pace.** Expect seconds per chunk under real cellular contention, so a
  several hundred kilobyte image is a transfer measured in tens of minutes rather
  than the naive bandwidth estimate. A sustained transfer has been observed to
  degrade the link itself, which is consistent with carrier side throttling.

Two traps that have each cost a debugging session:

- **A locally uploaded image is also a test swap now.** An image loaded over the
  serial bootloader that never completes a departure fetch, at a desk with no
  cellular coverage for instance, reverts on the next reset. Images written with
  an external programmer are unaffected.
- **The device credential is baked in at build time.** An image built before a
  credential rotation still carries the old one and will be rejected on every
  request once booted into, even though the update mechanism itself worked
  perfectly. After a rotation the image must be rebuilt, not merely re-uploaded.

## LTE power configuration

The board is mains powered, so the point here is carrier signalling, airtime and
modem duty cycle, and a configuration that carries over to battery powered
products, rather than battery life on this device.

| Knob | Setting | Why |
| --- | --- | --- |
| System mode | LTE-M only | No GNSS user in this application, and narrowband is the wrong shape for a multi kilobyte HTTPS fetch every 30 s. Also a smaller reacquisition scan space. |
| PSM | requested, long TAU, active time 0 | Everything is mobile originated, so a paging window after release protects nothing. Active time 0 drops straight to the PSM floor between fetches. |
| eDRX | requested, longest cycle | A fallback for networks that deny PSM. With PSM granted there are no idle paging occasions left for it to stretch. |
| Release assistance | requested, and asserted after each completed fetch | Where the network honours it, this deletes the carrier's connected mode inactivity tail, which is the dominant airtime cost at a 30 s cadence. |
| TLS session resumption | active | This was previously guarded by a server side mbedTLS symbol that is set nowhere, so every fetch paid a full handshake. The socket option drives the client cache, which was compiled in all along. |

Guard rails that must not regress:

- Release assistance is **skipped while a firmware download is active**. A
  download opens one connection per chunk, and a release between chunks would add
  a service request to each. A download runs on exactly the radio behaviour that
  predates this configuration.
- Release assistance is **skipped below a 15 s fetch interval**. Under the
  carrier's own inactivity timer the connection stays warm across fetches for
  free, and forcing a release would add tens of thousands of connection setups a
  day at the 5 s floor.
- The hint asserted is "last data", not "no further data", and only after a
  complete response, because the socket still owes the wire its TLS close notify
  and the release must be sequenced after that final uplink rather than racing it.
- PSM, eDRX and release assistance are all *requests*. A network that denies them
  leaves the sign on its previous behaviour, so the failure mode of every knob
  here is no change.

Grants are per attach and at carrier discretion, so none of this can be confirmed
without a modem on a real cell, and it must be confirmed from the application
image's own console rather than from a modem test image, which attaches with its
own requests. What to look for: the registration mode at boot; the granted
periodic update and active timers, where an active time of -1 means PSM was
denied; a modem sleep notification appearing between fetches, which is the proof
the modem really sleeps rather than merely releasing; and the delay from the last
received byte to the connection state notification, which is the real measure of
whether release assistance changed anything. Building once with release
assistance disabled on the same cell and hour gives both the baseline for that
comparison and the carrier's own inactivity timer, which is what the 15 s floor
has to sit above.

Release assistance can be honoured by the modem and still make no measurable
difference, if the cell predates the feature. That is a finding rather than a
failure, and the knob is worth leaving on for the networks that do honour it.

## What is still built per sign

With the stop and the layout both delivered by the shadow, one image serves every
sign. What remains per sign is the device identity: the platform endpoint and the
device credential, supplied from the untracked `app/prj.local.conf` described in
the [README](../README.md#per-sign-configuration). There is no runtime
provisioning path for those, which is what makes the baked credential trap above
possible.

## Release artifacts

One artifact per version string, ever, and the artifact is only decodable
alongside the dictionary from its exact build, so they travel as a set. Two
different binaries under one version string is the failure this rule exists to
prevent: the version a device reports no longer identifies what it is running.

For every image that reaches a real device slot, keep together:

- `zephyr.signed.bin`, the update and bootloader upload artifact
- `zephyr.signed.hex` and `merged.hex` for programming
- `log_dictionary.json` from the same build directory, without which that
  device's logs are permanently undecodable
- the build's `.config`, for provenance

A version bump is cheap and an ambiguous version string is not, so a change worth
flashing is worth its own version even when it is a single log line.
