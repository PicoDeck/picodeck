---
title: "Simulator and Testing"
---

## Simulator control protocol (TCP 127.0.0.1, or the ./picodeck_control UNIX socket)

- `launch_app` → `{queued, busy, launch_id}`; `app.exited` notifications carry
  `{name, id, found, result: returned|error|exit_sentinel|load_failed, error,
  runtime_ms, launch_id}`; `get_last_outcome` returns the last one.
- `get_log_buffer {since_seq, tail}` → `{lines:[{seq,t_ms,src,text}], next_seq,
  dropped, more}`; `subscribe {"logs":true}` pushes `log` notifications.
- `inject_*` return `input_seq`; `get_input_state` reports `consumed_seq`.
- `dev_command {"cmd": "..."}` runs `ping`/`exit`/`unzip`/`rm`/`pad` through
  the firmware's handlers.
- `step_time {ms}` and `set_time_multiplier` drive the virtual clock.
- Flags: `--test-mode` (error screens return at once; constant `math.random`
  seed and string hash seed; clock pinned to 2026-01-01), `--virtual-time`,
  `--unix-socket PATH|none`, `--real-umm` (firmware allocator; real
  fragmentation), `--build-info`, `--gamepad` / `--no-gamepad`.
- A game controller plugged into the computer drives `picocalc.gamepad`
  (D-pad or left stick, A/B/X/Y, shoulders = L/R, Start, Back = Select, Guide
  opens the system menu), with hotplug. It is read unless `--no-gamepad`, and
  not under `--test-mode` unless `--gamepad` is given, so tests never see the
  host's controller.

## Running the tests

- `SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto` (release simulator).
- Sanitizers: `make simulator-asan` / `simulator-tsan`, then
  `PICODECK_SIM_BINARY=build_sim_asan/picodeck_simulator pytest tests/e2e -n auto`.
- Firmware network stack in the simulator: `make simulator-net`,
  `PICODECK_SIM_BINARY=build_sim_net/picodeck_simulator pytest tests/e2e/test_network_firmware.py`.
- On a real device: `pytest tests/e2e --target hw:/dev/serial/by-id/<PicoDeck device>`
  runs the `hardware`/`both` tests.
- Host unit tests: `make test-unit`; fuzzers: `make fuzz`.

## Dev commands (USB serial)

- `stack` prints `msp_peak`, `core1_peak`, the running app's stack peak and
  `os_cmd_peak` (the last launcher command's peak on its 32 KB PSRAM stack).
- `exit` with no app running replies `Error: exit: no app running`.
- `pad <buttons|none|off> [hold_ms]` drives a test gamepad, as a connected
  controller would: `pad up+a` holds Up and A until the next `pad` command,
  `pad a 100` holds A for 100 ms, `pad none` releases everything, `pad off`
  disconnects it, and `home` opens the system menu, and closes it when it is open (`pad home 100`). Buttons:
  `up down left right a b x y l r start select home`, joined by `+`. Replies
  `Pad: up+a`. The MCP `pad` tool sends it (`pad("home, up, a")` presses each
  in turn).
- While an app runs: `reboot` and `reboot-flash` act at once (no teardown);
  `reboot-ota` (apply a staged, signed update) is dropped; `usb` waits for the
  launcher. Exit to the launcher before flashing.
