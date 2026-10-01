# PicoDeck E2E tests

pytest suite that drives the SDL2 + Unicorn simulator (`build_sim/picodeck_simulator`)
over its JSON-RPC control channel.

## Running

```bash
pip install -r tests/e2e/requirements.txt
make simulator

SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto          # whole suite
pytest tests/e2e/test_system.py -v                       # one module
pytest tests/e2e --show-window                           # watch the simulator
pytest tests/e2e --update-baselines                      # rewrite golden PNGs
pytest tests/e2e --html=reports/e2e.html --self-contained-html \
                 --artifacts-dir reports/failures        # CI-style reports
```

### Sanitizer builds

```bash
make simulator-asan      # ASan + UBSan, build_sim_asan/ (clang; SIM_SAN_CC to override)
PICODECK_SIM_BINARY=build_sim_asan/picodeck_simulator SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto
make simulator-tsan      # TSan, build_sim_tsan/ (informational, see below)
```

`PICODECK_SIM_BINARY` (or `--simulator-path`) picks the binary. The harness asks it
`--build-info` and runs the `asan_only` tests only against an ASan build; they
skip (allow-listed) otherwise. Every simulator gets `ASAN_OPTIONS`,
`UBSAN_OPTIONS` and `TSAN_OPTIONS` from `picodeck_simulator.SANITIZER_ENV` (any
already set in the environment win): ASan and UBSan reports are fatal, leak
checking is off (options you set yourself are merged in per key and win).
Set `PICODECK_SIM_EXPECT_SANITIZE=address` (the ASan CI leg does) to make the run
refuse to start unless `--build-info` confirms the sanitizer; if the probe
fails without it, `asan_only` skips are no longer allow-listed. Install
`llvm-symbolizer` (or set `ASAN_SYMBOLIZER_PATH`) for symbolised stacks. The
sanitizer build leaves SIGSEGV/SIGABRT to the sanitizer, so the
evidence is the report on stderr rather than the sim's crash log. stderr is
drained from the moment the process starts, and the report is captured apart
from the 2000-line tail (capped at 600 lines), so neither report volume nor
later output can hide it or back up the pipe.

TSan reports on nearly every test, from the sim's own threads: the RPC socket
thread reads the framebuffer, launcher and keyboard state that the main thread
owns, and the Core 1 thread reads audio and lifecycle globals the main thread
writes, all unlocked (`specs/test-audit-2026-09-24.md` §2.6). So the TSan leg is
nightly and informational; `tests/e2e/tsan.supp` suppresses third-party
(libdbus) reports only.

### Firmware network stack (`firmware_net`)

```bash
make simulator-net       # build_sim_net/ (also simulator-net-asan, simulator-net-tsan)
PICODECK_SIM_BINARY=build_sim_net/picodeck_simulator SDL_VIDEODRIVER=dummy \
    pytest tests/e2e/test_network_firmware.py -n auto
```

`make simulator-net` runs the firmware's own `src/drivers/wifi.c`, `http.c` and
`tcp.c` on Mongoose/POSIX (TLS off) instead of the sim's libcurl layer, with
Core 0 and Core 1 as two host threads (`simulator/net/`). The `firmware_net`
tests (`test_network_firmware.py`) run only against it (`--build-info` says
`firmware_net=1`) and skip, allow-listed, elsewhere;
`PICODECK_SIM_EXPECT_FIRMWARE_NET=1` makes the run refuse to start without it.
Each case runs in its own simulator against local servers
(`net_servers.py`: HTTP on 127.0.0.1 with `/ok`, `/big`, `/chunked`, `/close`,
`/drip`, `/hang`, `/reset`, `/echo`; a TCP echo/flood server; a black-hole
port). The app `net_fw/main.lua` is staged per test and runs the one case
named in `/data/com.test.net_fw/servers.json`, so a case that crashes the
simulator fails alone. They include regression tests for the fixed Core 0/
Core 1 close races from the code review (`KNOWN_BUGS` is empty). The whole
E2E suite also passes against this build.

Paths are absolute, so any working directory inside the repo works. The config
is the repo-level `pytest.ini`: 60 s per-test timeout (`pytest-timeout`), strict
xfail, and markers `slow`, `hardware`, `both`, `native`, `asan_only`, `flaky`,
`sd`.

### On a PicoCalc (`--target hw`)

```bash
ls /dev/serial/by-id/                       # usb-Raspberry_Pi_PicoDeck_Device_<serial>-if00
PORT=/dev/serial/by-id/usb-Raspberry_Pi_PicoDeck_Device_<serial>-if00
pytest tests/e2e --target hw:$PORT -v       # every hardware + both test, serially
pytest tests/e2e/test_hw_device.py --target hw:$PORT -v
PICODECK_HW_HOST_IP=192.168.1.20 pytest tests/e2e/test_hw_http_close.py --target hw:$PORT -v
```

`--target hw:<port>` (or `PICODECK_E2E_TARGET`) runs only tests marked
`@pytest.mark.hardware` or `@pytest.mark.both`, one at a time (no `-n`); every
other test is deselected. On the simulator (`--target sim`, the default)
`hardware` tests skip, allow-listed, and `both` tests run. A `both` test must
use the `target` fixture only: it gets `hw_target.SimTarget` over its
simulator, or the session's `hw_target.HwTarget`. Both offer `launch_app`,
`wait_for_exit`, `exit_app`, `keypress(_sequence)`, `pad`, `screenshot`,
`read_file`/`write_file`/`delete_file`, `push_app`/`stage_lua_app`,
`run_lua_app`, `wait_for_results`, `status` and log reading. Hardware-only
fixture apps live in `hw_apps/` (pushed by the test, never on the
simulator's default SD card; `test_mp3_pacing.py` stages `mp3_bench` on its
own card to run the same app in the simulator).

`HwTarget` drives the device through `tools/picodeck_mcp.py`'s serial helpers
and builds the device's traps in:

- **Results come from files.** The serial capture drops `[APP]` lines, so a
  Lua app's verdict is its picotest `/data/<APP_ID>/test_results.json`, read
  back with `getb64`; the outcome of a launch is `status` polling plus
  `/system/error.log` growth (`returned`, `error`, `exit_sentinel`,
  `load_failed`, `device_rebooted` when uptime goes back). Log lines
  (`get_log_lines`, `wait_for_log`) are advisory.
- **The launcher caches `app.json` at boot.** `push_app` reboots when the
  pushed manifest (id, name, requirements, `min_psram_kb`,
  `system_clock_khz`) differs from the card's, or `list` does not show the
  app, then polls `list` for up to 20 s (`rescan_timeout`): a `list` straight
  after the reboot can miss an app the launcher shows a few seconds later.
- **Dev commands while an app runs.** `reboot` and `reboot-flash` are
  honoured mid-app (a Lua app's instruction hook / `sys.sleep`, a native
  app's `sys->poll`; a native app that never polls leaves them latched for
  the launcher) and kill the app without teardown. `reboot-ota`, which
  applies an OTA flash, is dropped (`reboot-ota ignored: an app is
  running`). `usb` waits for the launcher. So `reboot()` exits the app first
  (or refuses) and `flash()` refuses while an app runs. A reboot is proven
  by uptime going back, never by `ver` (its timestamp lies after
  incremental builds).
- **One reader per port.** Before opening the port the run refuses one that
  another process holds (stop the MCP server's `stop_log_capture` first), a
  path that is not a serial tty, and a device that does not answer `ping`
  within 5 s: `--target hw:/dev/null` fails at once with the reason.
- Every wait is bounded; hardware tests carry a 300 s timeout because the
  first push reboots the device.

Pre-release checklist (not in CI): flash the release candidate
(`make flash-ota`), reboot so the heap is unfragmented, stop any serial log
capture, then run `pytest tests/e2e --target hw:$PORT -v` with WiFi
configured and `PICODECK_HW_HOST_IP` set. All hardware and both tests must
pass; a skip needs a reason (no WiFi) that is fixed before the release.

## Layout

```
tests/e2e/
├── conftest.py          fixtures and hooks (simulator, SD card, health check, skip allow-list)
├── helpers.py           SD staging, the Lua test-kit runner, golden compare, heap-metric check
├── picodeck_simulator.py   JSON-RPC client and process wrapper
├── hw_target.py         --target sim|hw: SimTarget / HwTarget (serial device backend)
├── hw_apps/<name>/      hardware-only fixture apps, pushed to the device by the test
├── lib/picotest.lua     Lua test kit, staged to /system/lib/picotest.lua
├── apps/<name>/         fixture apps (app.json + main.lua), staged onto every SD card
├── native/              source + Makefile of the native fixtures (apps/native_api_probe,
│                        apps/native_leaky, fixtures/native_pad_probe)
├── elfgen.py            struct.pack ELF32 builder, valid or malformed, for the loader tests
├── fixtures/fonts/      golden PNGs
├── fixtures/native_pad_probe/  the gbc / c64 gamepad adapters (staged by test_gamepad.py)
└── skip_allowlist.txt   tests that may skip; any other skip fails the run
```

## Native apps

`test_native.py` launches the committed `apps/hello_c/main.elf` (smoke) and
`apps/native_api_probe`, which logs the `PicoCalcAPI` layout it was compiled
against next to what the simulator handed it. Expected values come from the
sources: `g_api.version` from `src/main.c`, the layout from `src/os/os.h`
(parsed by `tools/check_native_abi.py`). The probe ELF is committed, so this
job needs no ARM toolchain; after changing `sdk/native/os.h`, rebuild it with
`make -C tests/e2e/native` and commit it. CI's `native-sdk` job (build-sim.yml)
compiles `sdk/native/main.c` and the probe and runs
`tools/check_native_abi.py --cc arm-none-eabi-gcc`, which compares the three
ABI copies (`src/os/os.h`, `sdk/native/os.h`, the trampolines' version write).

`test_native_resources.py` runs `apps/native_leaky` (source
`native/leaky.c`, built by the same Makefile), which opens files, makes
images, players, a terminal and a `qmiAlloc` block and returns without
freeing any of them, between runs of the Lua `apps/mem_report` (PSRAM free,
largest block, how many of 16 simultaneous opens succeed) under
`--real-umm`: after the native app exits, everything must be back.

`test_gamepad.py` stages `fixtures/native_pad_probe` (source
`native/pad_probe.c`, built by the same Makefile from the games' own
`apps/gbc/input.c` and `apps/c64/pad_input.h`), which logs the Game Boy
joypad and C64 joystick those adapters produce, on the gamepad and on a
copy of the API that looks like firmware before it. It is not under
`apps/` because the default card has no slot to spare (see "App cap").
After changing either adapter, rebuild it with `make -C tests/e2e/native`
and commit it.

`test_native_malformed.py` stages one `elfgen.build_elf(...)` image per
malformation (`specs/test-audit-2026-09-24.md` §3.5 plus the other
`elf_plan` refusals) as `/apps/badelf_<case>/main.elf` and asserts the launch
ends `load_failed` with the exact `[UNICORN] ELF rejected: <reason>` on
stderr, nothing ran, the SD card is unchanged and the simulator still answers.
The reason goes to stderr only (not the log buffer or `/system/error.log`,
which the firmware loader writes).

## Fixtures

- `simulator`: a fresh simulator per test, in `--test-mode` (Lua error screens
  return at once and their text goes to the log's `err` source), with no UNIX
  socket and a crash log in the test's tmp dir.
- `test_sd_card`: the per-test SD card. It is built from a manifest, not a copy
  of `simulator/assets/sd_card`: `apps/hello`, every `tests/e2e/apps/*` fixture,
  and `system/lib/*.lua` plus `picotest.lua`. Stage more with
  `@pytest.mark.sd(extra=["apps/foo", ("path/in/repo", "sd/dest")])`.
  The launcher keeps at most `MAX_APPS` apps: see "App cap" below.
- `sim_factory`: start additional simulators (`sim_factory(sd_path, **kwargs)`).
- `lua_suite`: run a test-kit app once in its own simulator and return its
  cases (use from a module-scoped fixture).

After every test the harness checks the simulators the test used: `simulator`
and any other fixture value that is a simulator, plus every simulator started
through `sim_factory` (registered on the test) or `sim_module_factory`
(registered on the module). The process must still be running, there must be
no crash log, and stderr must not contain a sanitizer report. A failing test
gets the simulator's stdout/stderr tails, log buffer, `/system/error.log` and a
screenshot attached (terminal sections and pytest-html extras; files under
`--artifacts-dir`).

`lua_suite` stops its simulator before its tests run, so it is not in that
hook. Its health goes into `LuaRun.problems` instead: if the simulator dies
mid-run, `run_lua_app` returns outcome `simulator_died` with the crash log,
exit status, sanitizer lines and stderr tail as problems, and every case id
(`check_case`, `assert_*`) fails with that evidence.

## App cap

The launcher keeps at most `MAX_APPS` (64, `src/os/launcher.c`) apps, in
directory order, and drops the rest with `[LAUNCHER] WARNING: app cap (64)
reached, ignoring '<dir>'`. Directory order depends on the host filesystem
(tmpfs lists new entries first, ext4 and btrfs do not), so a test on an
over-full card passes on one filesystem and fails on another. The cap is a
device memory decision (the app table lives in PSRAM): do not raise it and do
not add a simulator-only cap.

The default card holds `apps/hello` plus every `tests/e2e/apps/*` fixture, and
must leave one slot under the cap free: many tests stage a single app without a
marker, so `test_sd_card.py` fails a new fixture app that would use that slot,
before a hundred tests fail on health checks. A test that stages apps at
runtime (`stage_lua_app`, `stage_native_app`, copying an app in) asks for room:

```python
@pytest.mark.sd(fixtures=["fs_test"], reserve=3)   # per-test card
def test_x(simulator, test_sd_card): ...

sim_module_factory(setup=..., fixtures=[], reserve=20)   # module-scoped
lua_suite("fs_test", setup=..., fixtures=["fs_test"], reserve=2)
```

`fixtures` lists the fixture apps the test launches (`[]` = only `apps/hello`;
omitted = all of them); `reserve` is how many apps the test stages. Two guards
fail with an explanation, on every filesystem:

- `build_sd_card` raises when the card, plus `reserve`, would exceed `MAX_APPS`
  (read from `launcher.c`). Adding a fixture app past the cap trips
  `test_sd_card.py::test_default_card_holds_every_fixture_and_fits_the_cap`.
- A simulator whose launcher logged the drop warning is unhealthy: the
  after-test health check (and `stop_and_check`) fails naming the fix, so a test
  that stages more than it declared cannot pass by luck of directory order.

## Quarantined flaky tests

`@pytest.mark.flaky(reason=...)` quarantines a known flake: when it fails, the
failure is reported as an xfail with a `quarantined flake: <reason>` reason and
listed in its own "quarantined flaky tests that failed" summary section, and the
run does not fail. Nothing is retried. A quarantined test whose simulator
crashed, wrote a crash log or reported a sanitizer error is **not** forgiven: it
fails the run like any other. Keep crash assertions out of flaky tests (see
`test_cdogs_memory.py::test_gameplay_drive_did_not_crash`). Quarantine is a
stopgap: each marker names the cause and the fix.

## Synchronising with the simulator

Prefer these over `time.sleep`:

| Need | Call |
|---|---|
| app printed X | `sim.wait_for_log(regex, timeout, since_seq=, src=)` |
| app finished | `sim.launch_app(name)` then `sim.wait_for_exit(timeout)` → `{found, result, error, runtime_ms, launch_id}` |
| N frames presented | `sim.wait_frames(n)` |
| injected key consumed | `seq = sim.keypress(k)["input_seq"]; sim.wait_input_consumed(seq)` |
| app staged after boot | `stage_lua_app(sd, name, code, requirements=[...])` then `launch_app` (the sim rescans on a miss) |
| app running | `sim.call("get_running_app")` → always `{running, name}` |
| timed behaviour (animations, sleeps, repeat) | a `virtual_time=True` sim: `sys.sleep` runs at 50x and reads back exact; `sim.set_time_multiplier(0)` pauses, `sim.step_time(ms)` advances |

Virtual time (`sim_factory(sd, virtual_time=True)`) is opt-in. Only the
OS/app thread moves the clock; audio playback and network I/O stay on real
time, so suites that time audio (`test_fileplayer.py`) or rely on network
timeouts keep the wall clock. On a paused clock an app that never sleeps
(panels.lua) sees time move only on `step_time`; sync on input with explicit
`inject_button` press/release plus `wait_input_consumed` (a click's 80 ms
auto-release is also on the sim clock), as `test_panels.py` does.
`--test-mode` (every suite sim) also seeds `math.random` and the string hash
with constants and pins the clock to 2026-01-01T00:00:00Z at boot.

## Writing a Lua test app (picotest)

```lua
local T = picocalc.sys.loadlib("picotest")

T.case("adds", function()
    T.eq(1 + 1, 2)
end)
T.case("needs network", function()
    T.skip("not in the simulator")   -- fails the run unless allow-listed
end)
T.done()
```

Checks: `T.eq(got, want, msg)`, `T.ok(v, msg)`, `T.fail(msg)`,
`T.raises(fn, pattern)`, `T.skip(reason)`. Each case logs
`[T] CASE <name> PASS|FAIL|SKIP <detail>`; `T.done()` logs
`[T] DONE pass=N fail=M skip=K`. Results are also written to
`/data/<APP_ID>/test_results.json` after every case. The kit restores
`APP_ID`/`APP_DIR`/`APP_NAME`/`APP_REQUIREMENTS` after each case.

On the Python side:

```python
from helpers import lua_case_names

CASES = lua_case_names("my_test")          # T.case names, read statically

@pytest.fixture(scope="module")
def my_run(lua_suite):
    return lua_suite("my_test")

@pytest.mark.parametrize("case", CASES)
def test_my_case(my_run, case):
    my_run.check_case(case)

def test_my_suite_complete(my_run):
    my_run.assert_all_passed(CASES)
```

For a case that depends on a known, unfixed bug, mark the pytest id
`xfail(strict=True, reason="review: <row>")`: it must fail today, and the fix
turns it into an XPASS, which fails the run until the marker is removed.

## Golden images

A missing golden fails the test. Create or refresh goldens with
`--update-baselines` and review the diff before committing them.

## Known limits

- The simulator runs its own display, audio, keyboard and network code, not
  `src/drivers/*` (see `specs/test-audit-2026-09-24.md` §2.7), so goldens and
  pixel values prove simulator output. The exception is `make simulator-net`,
  which runs the firmware's `wifi.c`/`http.c`/`tcp.c` (without TLS).
- `get_heap_info` in the simulator counts the live `umm_*`/Lua bytes (a
  counting allocator over malloc: 8 MB minus live). The largest free block is
  approximated by the free total and fragmentation is always 0, so
  fragmentation and `min_psram_kb` behaviour are still device-only.
  `test_heap_metrics_live` proves the metric moves; the leak tests check it
  first (`require_heap_metrics_live`).
- `test_cdogs_memory.py` needs a built checkout of PicoDeck/cdogs
  (`PICODECK_CDOGS_DIR`, default `~/Projects/PicoDeck/cdogs`); it is allow-listed to skip.
