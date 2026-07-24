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
(shadow-driven `stop_id`, telemetry) — see the branch's commit messages for
the full design writeup. Both debug and release profiles build clean, and
the board was left flashed with the final build. What's still outstanding,
all requiring hands-on-bench access this session didn't have:

1. **Console wiring needs physical checking.** The board was reconnected to
   the bench (external J-Link SWD + separate CP2102N console cable) the
   morning this work started. Flashing works reliably (`nrfutil` program+
   verify succeed) for both this branch's firmware and a stock, unmodified
   `zephyr/samples/hello_world` control build, but neither produces a single
   byte on `/dev/ttyUSB0` at their respective correct baud rates (1,000,000
   / 115,200), across resets, pin-resets, and full recovers. Two independent
   images being equally silent rules out firmware as the cause — this is a
   cable/wiring/jumper issue between the 9160's TX pin and the CP2102N
   bridge, not something fixable from software. Check/reseat that wiring
   before anything else in this list.
2. **`CONFIG_PIGEON_WATCHDOG` + `watchdog_app.c` share one physical
   watchdog**, unconfirmed on real hardware. This board's `watchdog0`
   devicetree alias points at the exact same `wdt0` peripheral
   `watchdog_app.c` already owns end-to-end (`wdt_install_timeout`/
   `wdt_setup`, fed throughout `main.c`'s loop). Reading Zephyr's actual
   `task_wdt_init()`/`task_wdt_add()` source suggests this should fail
   gracefully — `wdt_install_timeout()` on an already-`wdt_setup()`'d device
   is expected to error out, which `pigeon_watchdog_start()` already handles
   by just logging and leaving its own channel unarmed, not crashing or
   otherwise interfering with `watchdog_app.c`. That's reasoned from source,
   not yet confirmed against a live console log — watch for a
   `task_wdt_init failed` line at boot and confirm `watchdog_app.c`'s own
   feed loop still runs normally either way.
3. **`zephyr/soc/nordic/Kconfig` patch**: see Setup step 10 above — required
   after every `west update` on this branch, not committed since it targets
   a vendored/gitignored file.
4. **Once console output is confirmed working**, the pending verification
   sequence is: boot → LTE attach → pigeon shadow sync (watch for
   `pigeon_shadow_get`/`Stop ID updated to:` log lines) → telemetry keys
   (`rsrp_dbm`/`uptime_s`/`swiftly_consecutive_failures`/
   `swiftly_last_success_age_s`) actually landing in the dashboard → the
   money demo: pushing a new `stop_id` via the shadow from the dashboard/API
   and confirming the sign switches stops without a reflash.

## Creating a Release
Update the [VERSION file](https://github.com/umts/embedded-departure-board/blob/main/app/VERSION).
On a successful push to the main branch the [release workflow](https://github.com/umts/embedded-departure-board/blob/main/.github/workflows/release.yml) will; create a new release, generate release notes, and upload the freshly built hex/bin files to the release.
