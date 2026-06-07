# Device tests (`scripts/tests/`)

On-hardware tests that drive the Game & Watch over the debug probe and assert on
device state. They depend **only** on `scripts/common/` — never on
`scripts/debug/`. Anything a test needs lives in `common/` (input backend in
`common/remote_input.py`, harness helpers in `common/harness.py`).

## Prerequisites

- A device connected via debug probe.
- Firmware built and flashed with the remote-input hook:
  `make clean && make REMOTE_INPUT=1 -j16 && make REMOTE_INPUT=1 flash_chainloader`
  (the boot-selector test additionally needs `make REMOTE_INPUT=1 flash_all`).

## Running

```bash
python3 scripts/tests/test_remote_input.py     # input timing + menu navigation (no flashing)
python3 scripts/tests/boot_selector_test.py     # LAUNCH selector boots each target + Left+Game escape
```

Each script exits 0 on success, non-zero on failure, and prints a PASS/FAIL line
per check.
