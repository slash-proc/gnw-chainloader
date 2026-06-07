#!/usr/bin/env python3
"""Fast Framebuffer Capture — on-device tiled JPEG, host decode.

The device (src/fastcap/fastcap.bin, loaded into D2-SRAM1 and hooked into
gui_refresh) tiles the screen into 32x16 tiles (10x15 = 150), hashes each tile,
and hardware-JPEG-encodes only the changed ones.  Each STATUS_FLAG raise
corresponds to ONE frame's payload at FASTCAP_PAYLOAD_BUF:

  frame header (12 bytes):  u32 tile_count;  u32 total_bytes;  u32 skipped
  then tile_count records:  u32 idx;  u32 jpeg_len;  u8 jpeg[jpeg_len]   (packed)

Only the keyframe's first tile carries a full JPEG header; the host caches that
prefix and re-wraps the later headerless scans.  A fully-unchanged frame is sent
silently (tile_count=0) — `skipped` counts how many to replay so the mp4 timeline
stays uniform.  The host keeps a running RGB frame, decodes each tile JPEG and
composites it at (idx%10 * 32, idx//10 * 16); every frame is saved as a PNG.

Subcommands
-----------
  capture   stream to PNGs + mp4.  --mode async (live, drops frames) or sync
            (frame-perfect, device runs slower than real-time).  Add --live for an
            interactive OpenCV cockpit: watch the stream and drive the UI in the
            same window (WASD/arrows + J/K, plus a gamepad if present).
  grab      one frame: a compressed keyframe by default, or --raw for a pristine
            uncompressed framebuffer pull (no fastcap.bin needed).
  analyze   per-frame PNG stats (size, non-black %, delta-vs-prev %).

The OpenOCD SWD link can wedge on a long run; capture auto-recovers (kills the
stale process, reopens, re-arms with a keyframe reset).
"""
import argparse
import struct
import sys
import time
from collections import deque
from io import BytesIO
from pathlib import Path

import numpy as np
from PIL import Image

# scripts/ for `common`, scripts/debug/ for the sibling `remote_control`.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import device
from common import remote_input as ri
from remote_control import open_input_source, WindowKeySource

# ---- DTCM handshake cells (keep in sync with src/common/memory_map.h) ----
STATUS_FLAG_ADDR  = 0x2001FF00  # u32: device writes 1 (ready); host writes 0 (ack)
FASTCAP_HOOK_ADDR = 0x2001FF04  # void(*)(const uint16_t*): hook entry point
RESET_FLAG_ADDR   = 0x2001FF08  # u32: host writes 1 to force a fresh keyframe
FASTCAP_QUAL_ADDR = 0x2001FF0C  # u32: host writes JPEG quality 1..100 (read at reinit)
FASTCAP_MODE_ADDR = 0x2001FF14  # u32: host writes 0 = async, 1 = sync
DEFAULT_QUALITY   = 98          # device falls back to this if the cell is unset (0)

# ---- D2 AHB-SRAM1: fastcap lives here; its clock is off at reset, so the host
#      must enable it (RCC_AHB2ENR bits 29/30) before loading the binary. ----
RCC_AHB2ENR_ADDR = 0x580244DC
D2SRAM_EN        = 0x60000000   # AHBSRAM1EN | AHBSRAM2EN
RAM_BASE         = 0x3000E000   # fastcap.bin code (D2-SRAM1; data buffers below it)
PAYLOAD_BUF_ADDR = 0x30002400   # frame header + packed tile JPEGs
MAX_PAYLOAD      = 0xBC00        # 47 KB (PAY_BUF up to the code region at 0x3000E000)

SCREEN_W, SCREEN_H = 320, 240
TILE_W, TILE_H     = 32, 16
TILES_X, TILES_Y   = 10, 15      # 150 fixed tiles

# ---- Payload: frame header {u32 tile_count; u32 total_bytes}, then tile_count
#      records of {u32 idx; u32 jpeg_len; u8 jpeg[jpeg_len]} (packed). ----
FRAME_HDR_FMT   = "<III"          # tile_count, total_bytes, skipped
FRAME_HDR_BYTES = struct.calcsize(FRAME_HDR_FMT)
TILE_REC_FMT    = "<II"
TILE_REC_BYTES  = struct.calcsize(TILE_REC_FMT)
SKIP_REPEAT_CAP = 120             # cap host-side duplicate frames per silent gap (mp4 timing)

MODE_ASYNC, MODE_SYNC = 0, 1

DEFAULT_SWD_KHZ = 4000
DEFAULT_BIN     = "build/fastcap/fastcap.bin"
DEFAULT_OUT_DIR = "build/capture"
POLL_SLEEP_S    = 0.002


# ---- Frame decoder ----

def _jpeg_prefix(jpeg: bytes):
    """Return a full JPEG's header bytes (SOI through the SOS segment, before the
    entropy scan), or None if it doesn't parse. Used to cache the shared header
    so headerless tile scans can be re-wrapped for decoding."""
    if len(jpeg) < 4 or jpeg[0] != 0xFF or jpeg[1] != 0xD8:
        return None
    i = 2
    while i + 4 <= len(jpeg):
        if jpeg[i] != 0xFF:
            return None
        marker = jpeg[i + 1]
        seg_len = (jpeg[i + 2] << 8) | jpeg[i + 3]
        if marker == 0xDA:                      # SOS — header ends after this segment
            return jpeg[: i + 2 + seg_len]
        i += 2 + seg_len
    return None


class FrameDecoder:
    """Running RGB888 frame; each device frame is a set of 32x16 tile JPEGs."""

    def __init__(self):
        self.frame = np.zeros((SCREEN_H, SCREEN_W, 3), dtype=np.uint8)
        self.frames_total = 0
        self.tiles_total = 0
        self.skips = 0
        self.bytes_total = 0
        self.max_frame_bytes = 0    # largest single-frame payload (keyframe headroom check)
        self.max_frame_tiles = 0
        self.header_prefix = None   # cached JPEG header (SOI..SOS) for headerless tiles

    def reset(self):
        """Clear to black — call when the device is about to re-send all tiles.
        Also drop the cached JPEG header: a re-arm may change quality, so the old
        DQT/DHT prefix is stale; the forced keyframe re-sends a header-ful tile."""
        self.frame[:] = 0
        self.header_prefix = None

    def apply_frame(self, payload: bytes) -> int:
        """Parse and composite tile records from one frame's payload (the bytes
        after the 8-byte frame header). Returns the number of tiles applied."""
        frame_bytes = FRAME_HDR_BYTES + len(payload)
        if frame_bytes > self.max_frame_bytes:
            self.max_frame_bytes = frame_bytes
        off, n = 0, 0
        while off + TILE_REC_BYTES <= len(payload):
            idx, jlen = struct.unpack_from(TILE_REC_FMT, payload, off)
            off += TILE_REC_BYTES
            if off + jlen > len(payload):
                print(f"  *** truncated tile record (idx {idx}, len {jlen}) ***")
                break
            data = payload[off:off + jlen]
            off += jlen
            if idx >= TILES_X * TILES_Y:
                continue
            # A header-ful tile starts with SOI (FFD8): decode it and cache its
            # header prefix; a headerless tile is just an entropy scan we re-wrap.
            if len(data) >= 2 and data[0] == 0xFF and data[1] == 0xD8:
                jpeg = data
                prefix = _jpeg_prefix(data)
                if prefix is not None:
                    self.header_prefix = prefix
            elif self.header_prefix is not None:
                jpeg = self.header_prefix + data + b"\xFF\xD9"
            else:
                print(f"  *** headerless tile {idx} but no cached header yet ***")
                continue
            try:
                arr = np.asarray(Image.open(BytesIO(jpeg)).convert("RGB"), dtype=np.uint8)
            except Exception as e:
                print(f"  *** tile {idx} JPEG decode error: {e} ***")
                continue
            x0, y0 = (idx % TILES_X) * TILE_W, (idx // TILES_X) * TILE_H
            h, w = min(TILE_H, arr.shape[0]), min(TILE_W, arr.shape[1])
            self.frame[y0:y0 + h, x0:x0 + w] = arr[:h, :w]
            self.bytes_total += jlen
            n += 1
        self.tiles_total += n
        if n > self.max_frame_tiles:
            self.max_frame_tiles = n
        return n

    def save(self, path: Path):
        Image.fromarray(self.frame).save(path)
        self.frames_total += 1


# ---- Frame-rate meter (rolling, from cumulative frame counts) ----

class FpsMeter:
    """Smoothed frame rate over a short sliding window.  Fed the cumulative
    frame total (including skip-replays) so it tracks the same number the final
    summary reports, and stays live even while the screen is static."""
    def __init__(self, window=1.5):
        self.window = window
        self.samples = deque()   # (monotonic_t, cumulative_frames)

    def update(self, now, total):
        self.samples.append((now, total))
        while len(self.samples) > 1 and now - self.samples[0][0] > self.window:
            self.samples.popleft()

    def fps(self):
        if len(self.samples) < 2:
            return 0.0
        t0, f0 = self.samples[0]
        t1, f1 = self.samples[-1]
        dt = t1 - t0
        return (f1 - f0) / dt if dt > 0 else 0.0


# ---- Live preview (optional OpenCV window) ----

class LivePreview:
    def __init__(self, enabled, scale=2, control=False, hold_ms=150):
        self.enabled = enabled
        self.scale = max(1, int(scale))
        self.cv2 = None
        self.window = "fastcap (live)"
        self.show_fps = True       # overlay on by default; toggle with 'f'
        self.keys = None           # WindowKeySource when control is on
        if not enabled:
            return
        try:
            import cv2
            self.cv2 = cv2
            self.window = "fastcap (live — drive: WASD/arrows JK)" if control else "fastcap (live)"
            cv2.namedWindow(self.window, cv2.WINDOW_NORMAL)
            cv2.resizeWindow(self.window, SCREEN_W * self.scale, SCREEN_H * self.scale)
        except Exception as e:
            print(f"  *** --live requested but OpenCV unavailable ({e}); continuing ***")
            self.enabled = False
            return
        if control:
            self.keys = WindowKeySource(hold_ms)
            print(f"  Live control: {WindowKeySource.KEY_HELP}")
            print("  (click the preview window so it has keyboard focus)")

    def pump(self, now):
        """Pump the OpenCV window event loop and capture control keys. Call every
        loop iteration (keeps the window responsive + renders the last imshow even
        when the screen is static). Returns (quit, button_mask)."""
        if not self.enabled or self.cv2 is None:
            return (False, 0)
        try:
            key = self.cv2.waitKeyEx(1)
        except Exception:
            return (False, 0)
        if key != -1:
            if key == ord('f'):
                self.show_fps = not self.show_fps        # toggle the on-screen FPS overlay
            elif self.keys is not None:
                self.keys.feed(key, now)
        if self.keys is not None:
            return (self.keys.quit, self.keys.poll(now))
        return (False, 0)

    def _draw_fps(self, bgr, fps):
        label = f"{fps:4.1f} fps"
        font = self.cv2.FONT_HERSHEY_SIMPLEX
        fs = 0.5 * self.scale
        th = max(1, self.scale)
        (tw, tht), base = self.cv2.getTextSize(label, font, fs, th)
        h, w = bgr.shape[:2]
        x = w - tw - 6 * self.scale
        y = h - 6 * self.scale
        self.cv2.putText(bgr, label, (x + 1, y + 1), font, fs, (0, 0, 0),
                         th + 2, self.cv2.LINE_AA)              # shadow for legibility
        self.cv2.putText(bgr, label, (x, y), font, fs, (0, 255, 0),
                         th, self.cv2.LINE_AA)

    def show(self, rgb, fps=None):
        """Queue a frame for display (imshow only — pump() runs the event loop)."""
        if not self.enabled or self.cv2 is None:
            return
        try:
            bgr = np.ascontiguousarray(rgb[:, :, ::-1])
            if self.scale != 1:
                bgr = self.cv2.resize(bgr, (SCREEN_W * self.scale, SCREEN_H * self.scale),
                                      interpolation=self.cv2.INTER_NEAREST)
            if self.show_fps and fps is not None:
                self._draw_fps(bgr, fps)
            self.cv2.imshow(self.window, bgr)
        except Exception:
            pass

    def close(self):
        if self.enabled and self.cv2 is not None:
            try:
                self.cv2.destroyWindow(self.window)
            except Exception:
                pass


# ---- Hook load / arm ----

def arm_hook(backend, entry):
    device.swd_write(backend, FASTCAP_HOOK_ADDR, entry)


def disable_hook(backend):
    try:
        device.swd_write(backend, FASTCAP_HOOK_ADDR, 0)
        print("Hook disabled (FASTCAP_HOOK_ADDR cleared).")
    except Exception as e:
        print(f"  Warning: could not clear hook pointer: {e}")


def load_binary(backend, bin_path, mode, quality=DEFAULT_QUALITY):
    """Enable the D2 clock, load fastcap.bin into D2-SRAM1, set MODE/QUALITY, reset, arm. Returns entry."""
    backend.halt()

    # D2 AHB-SRAM is unclocked at reset — enable it before writing the binary there.
    cur = device.swd_read(backend, RCC_AHB2ENR_ADDR)
    device.swd_write(backend, RCC_AHB2ENR_ADDR, cur | D2SRAM_EN)
    print(f"  D2 SRAM clock: RCC_AHB2ENR 0x{cur:08X} -> 0x{cur | D2SRAM_EN:08X}")
    device.swd_write(backend, PAYLOAD_BUF_ADDR, 0xCAFED2D2)
    d2 = device.swd_read(backend, PAYLOAD_BUF_ADDR)
    print(f"  D2 write/readback @0x{PAYLOAD_BUF_ADDR:08X}: 0x{d2:08X}"
          + ("" if d2 == 0xCAFED2D2 else "  *** D2 NOT WRITABLE ***"))

    print(f"  Loading {bin_path} into D2-SRAM1 at 0x{RAM_BASE:08X}...")
    data = Path(bin_path).read_bytes()
    backend.write_memory(RAM_BASE, data)
    entry = int.from_bytes(data[4:8], "little")

    # Clear STATUS (DTCM holds garbage at power-on) so the async drop-check
    # doesn't see "host busy" and drop every frame.  MODE and QUALITY must be
    # written BEFORE RESET so they latch at the device's reinit.
    device.swd_write(backend, STATUS_FLAG_ADDR, 0)
    device.swd_write(backend, FASTCAP_MODE_ADDR, mode)
    device.swd_write(backend, FASTCAP_QUAL_ADDR, quality)
    device.swd_write(backend, RESET_FLAG_ADDR, 1)
    arm_hook(backend, entry)
    print(f"  Hook armed: 0x{FASTCAP_HOOK_ADDR:08X} = 0x{entry:08X}  "
          f"mode={'sync' if mode == MODE_SYNC else 'async'}  quality={quality}")
    backend.resume()
    return entry


def read_frame(backend):
    """Read one frame's payload (header + packed tile records) and ack.
    Returns (tile_count, records_bytes)."""
    hdr = device.swd_read_mem(backend, PAYLOAD_BUF_ADDR, FRAME_HDR_BYTES)
    tile_count, total_bytes, skipped = struct.unpack(FRAME_HDR_FMT, hdr)
    payload = b""
    if tile_count > 0 and 0 < total_bytes <= MAX_PAYLOAD:
        payload = device.swd_read_mem(backend, PAYLOAD_BUF_ADDR + FRAME_HDR_BYTES, total_bytes)
    device.swd_write(backend, STATUS_FLAG_ADDR, 0)   # ack ASAP (ends the sync per-frame gate)
    return tile_count, skipped, payload


# ---- capture ----

def _capture_session(backend, transport, src, decoder, out_dir, preview, deadline, state):
    """One OpenOCD session's capture loop.  Returns True on clean stop/deadline,
    False on an SWD wedge (caller reconnects)."""
    t_hb = time.monotonic()
    while deadline is None or time.monotonic() < deadline:
        now = time.monotonic()

        # (a) live input: pump the OpenCV window (keeps it responsive + captures
        #     control keys) and OR its mask with any gamepad; write on change.
        win_quit, win_mask = preview.pump(now)
        if win_quit:                         # 'q'/Esc in the live window
            state["stop"] = True
            return True
        src_mask = 0
        if src is not None:
            m = src.poll(now)
            if m is None:                    # quit (q/ESC/Ctrl-C in raw kbd)
                state["stop"] = True
                return True
            src_mask = m
        mask = win_mask | src_mask
        if mask != state["last_mask"]:
            try:
                transport.write_mask(mask)
            except device.SWDTimeout:
                return False
            state["last_mask"] = mask

        # (b) capture: poll STATUS, drain one record when ready.
        try:
            if device.swd_read(backend, STATUS_FLAG_ADDR) != 1:
                time.sleep(POLL_SLEEP_S)
            else:
                tile_count, skipped, payload = read_frame(backend)
                # The device stayed silent on `skipped` unchanged/dropped frames —
                # repeat the prior frame to keep the mp4 timeline (capped).
                for _ in range(min(skipped, SKIP_REPEAT_CAP)):
                    state["frame_n"] += 1
                    decoder.save(out_dir / f"frame_{state['frame_n']:05d}.png")
                    decoder.skips += 1
                if tile_count > 0:                  # tile_count 0 = pure timing anchor
                    decoder.apply_frame(payload)
                    state["frame_n"] += 1
                    decoder.save(out_dir / f"frame_{state['frame_n']:05d}.png")
                    state["fps"].update(now, decoder.frames_total)
                    preview.show(decoder.frame, state["fps"].fps())
        except device.SWDTimeout:
            print("\n  SWD wedged — reconnecting...")
            return False

        if now - t_hb >= 0.5:
            state["fps"].update(now, decoder.frames_total)   # keep fps live while static
            el = now - state["t_start"]
            sys.stdout.write(f"\r  T+{el:5.1f}s  frames={decoder.frames_total} "
                             f"fps={state['fps'].fps():4.1f} "
                             f"tiles={decoder.tiles_total} skips={decoder.skips}   ")
            sys.stdout.flush()
            t_hb = now
    return True


def run_capture(args):
    mode = MODE_SYNC if args.mode == "sync" else MODE_ASYNC
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for stale in out_dir.glob("frame_*.png"):
        stale.unlink()

    decoder = FrameDecoder()
    # With --live the OpenCV window is the cockpit: it owns the keyboard (its own
    # key map) and a gamepad can drive alongside it. Without --live, input is the
    # raw terminal / gamepad as before. --input none = passive (no control).
    control = args.live and args.input != "none"
    preview = LivePreview(enabled=args.live, scale=args.live_scale,
                          control=control, hold_ms=args.hold)
    if args.input == "none":
        src = None
    elif args.live:
        try:                       # window handles the keyboard; add a gamepad if one's present
            src = open_input_source("gamepad", args.device, args.hold)
        except Exception:
            src = None
    else:
        src = open_input_source(args.input, args.device, args.hold)

    t_start = time.monotonic()
    deadline = None if args.duration <= 0 else t_start + args.duration
    state = {"entry": None, "frame_n": 0, "stop": False, "last_mask": -1,
             "t_start": t_start, "fps": FpsMeter()}

    print(f"Capture: mode={args.mode}, swd={args.swd_khz}kHz, "
          f"duration={'∞' if deadline is None else f'{args.duration}s'}, tiles={TILES_X}x{TILES_Y}@{TILE_W}x{TILE_H}")
    print(f"  Output: {out_dir}")

    try:
        while not state["stop"] and (deadline is None or time.monotonic() < deadline):
            try:
                with device.open_backend(halt=False) as backend:
                    backend.set_frequency(args.swd_khz * 1000)
                    if state["entry"] is None:
                        state["entry"] = load_binary(backend, args.load, mode, args.quality)
                    else:
                        print("  Re-arming after reconnect (keyframe reset)...")
                        device.swd_write(backend, STATUS_FLAG_ADDR, 0)
                        device.swd_write(backend, FASTCAP_MODE_ADDR, mode)
                        device.swd_write(backend, FASTCAP_QUAL_ADDR, args.quality)
                        device.swd_write(backend, RESET_FLAG_ADDR, 1)
                        decoder.reset()
                        arm_hook(backend, state["entry"])

                    # Share THIS session with input — one OpenOCD connection.
                    transport = ri.ShadowCellTransport(backend=backend)
                    dev = ri.RemoteInput(transport)
                    dev.open()
                    dev.tap([ri.BTN_SELECT])   # dismiss idle-hide so gui_refresh (and the hook) runs
                    state["last_mask"] = -1

                    clean = _capture_session(backend, transport, src, decoder,
                                             out_dir, preview, deadline, state)
                    if clean:
                        disable_hook(backend)
                        break
                    try:
                        backend._openocd_process.kill()
                    except Exception:
                        pass
            except KeyboardInterrupt:
                state["stop"] = True
            except device.SWDTimeout:
                print("  SWD timeout during setup — retrying...")
                time.sleep(1.0)
            except Exception as e:
                print(f"  Backend closed: {e}; waiting 1s...")
                time.sleep(1.0)
    except KeyboardInterrupt:
        state["stop"] = True
    finally:
        if src is not None:
            src.close()
        preview.close()

    elapsed = time.monotonic() - t_start
    fps = decoder.frames_total / elapsed if elapsed > 0 else 0
    changed = max(1, decoder.frames_total - decoder.skips)
    print(f"\n--- Capture summary ---")
    print(f"  Duration : {elapsed:.2f}s")
    print(f"  Frames   : {decoder.frames_total} ({fps:.1f} fps)")
    print(f"  Tiles sent / skipped frames: {decoder.tiles_total} / {decoder.skips}")
    print(f"  Avg JPEG : {decoder.bytes_total / 1024 / changed:.1f} KB per changed frame")
    print(f"  Max frame: {decoder.max_frame_bytes / 1024:.1f} KB "
          f"({decoder.max_frame_tiles} tiles) of {MAX_PAYLOAD / 1024:.0f} KB cap")

    if not args.no_mp4 and decoder.frames_total > 0:
        mp4_path = Path(args.mp4) if args.mp4 else (out_dir / "capture.mp4")
        mp4_fps = args.mp4_fps if args.mp4_fps else max(fps, 1.0)
        device.frames_to_mp4(out_dir, mp4_path, mp4_fps)


# ---- grab ----

def run_grab(args):
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)

    if args.raw:
        # Pristine uncompressed pull: halt + read the LTDC framebuffer directly.
        with device.open_backend(halt=True) as backend:
            img, info = device.read_framebuffer(backend)
        img.save(out)
        print(f"Wrote {out}  (raw {info['width']}×{info['height']} {info['format']})")
        return

    # Compressed: load fastcap and grab the first keyframe (all tiles after RESET).
    decoder = FrameDecoder()
    got = False
    with device.open_backend(halt=False) as backend:
        backend.set_frequency(args.swd_khz * 1000)
        load_binary(backend, args.load, MODE_ASYNC, args.quality)
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            try:
                if device.swd_read(backend, STATUS_FLAG_ADDR) != 1:
                    time.sleep(POLL_SLEEP_S)
                    continue
                tile_count, _skipped, payload = read_frame(backend)
            except device.SWDTimeout:
                break
            if tile_count > 0:
                decoder.apply_frame(payload)
                got = True
                if tile_count >= TILES_X * TILES_Y:   # full keyframe → complete image
                    break
        disable_hook(backend)

    if got:
        decoder.save(out)
        print(f"Wrote {out}  (compressed, {decoder.tiles_total} tiles)")
    else:
        print("*** no frame received within 5s — is the menu on screen? ***")
        sys.exit(1)


# ---- analyze ----

def run_analyze(args):
    d = Path(args.dir)
    frames = sorted(d.glob("frame_*.png"))
    if not frames:
        print(f"No frame_*.png in {d}")
        sys.exit(1)
    total = SCREEN_W * SCREEN_H
    print(f"{'Frame':<20}{'Size':>10}{'Non-black':>20}{'vs prev':>18}")
    prev = None
    for i, f in enumerate(frames, 1):
        if i < args.from_n:
            continue
        arr = np.asarray(Image.open(f).convert("RGB"), dtype=np.uint8)
        nonblack = int(np.count_nonzero(arr.any(axis=2)))
        size_kb = f.stat().st_size / 1024
        if prev is not None and prev.shape == arr.shape:
            changed = int(np.count_nonzero(np.any(arr != prev, axis=2)))
            vs = f"{changed:,} px ({100 * changed / total:4.1f}%)"
        else:
            vs = "—"
        print(f"{f.name:<20}{size_kb:>7.1f} KB"
              f"{nonblack:>11,} px ({100 * nonblack / total:4.1f}%){vs:>18}")
        prev = arr


# ---- CLI ----

def build_parser():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("capture", help="stream the framebuffer to PNGs + mp4")
    c.add_argument("--mode", choices=("async", "sync"), default="async",
                   help="async = live/real-time, drops frames (~10-15 fps); "
                        "sync = frame-perfect, device runs slower than real-time (default: async)")
    c.add_argument("--input", choices=("auto", "gamepad", "keyboard", "none"), default="auto",
                   help="live-drive input source: auto (gamepad first, else keyboard), "
                        "or force one, or none (default: auto)")
    c.add_argument("--device", help="explicit evdev gamepad path, e.g. /dev/input/event5")
    c.add_argument("--hold", type=int, default=150, help="keyboard key hold-ms (default 150)")
    c.add_argument("--duration", type=float, default=0,
                   help="seconds to capture; 0 = run until Ctrl-C / q (default: 0)")
    c.add_argument("--swd-khz", type=int, default=DEFAULT_SWD_KHZ,
                   help=f"SWD clock kHz (default {DEFAULT_SWD_KHZ}; ST-Link V2 up to 10000)")
    c.add_argument("--quality", type=int, default=DEFAULT_QUALITY,
                   help=f"JPEG quality 1..100 (default {DEFAULT_QUALITY}; higher = crisper but "
                        f"bigger frames → fewer distinct fps on the throughput-bound async link)")
    c.add_argument("--out-dir", default=DEFAULT_OUT_DIR, help=f"frame dir (default {DEFAULT_OUT_DIR})")
    c.add_argument("--load", default=DEFAULT_BIN, help=f"fastcap.bin (default {DEFAULT_BIN})")
    c.add_argument("--mp4", default=None, help="mp4 output path (default: <out-dir>/capture.mp4)")
    c.add_argument("--no-mp4", action="store_true", help="skip mp4 assembly")
    c.add_argument("--mp4-fps", type=float, default=None, help="override mp4 fps (default: measured)")
    c.add_argument("--live", action="store_true",
                   help="OpenCV live window. With --input != none it's an interactive cockpit: "
                        "drive with WASD/arrows + J/K (and a gamepad if present) while watching. "
                        "F toggles the FPS overlay, Q/Esc quits.")
    c.add_argument("--live-scale", type=int, default=2, help="live preview upscale (default 2)")
    c.set_defaults(func=run_capture)

    g = sub.add_parser("grab", help="capture a single frame")
    g.add_argument("-o", "--output", default="build/capture/grab.png", help="output PNG")
    g.add_argument("--raw", action="store_true",
                   help="uncompressed pristine framebuffer pull (no fastcap.bin)")
    g.add_argument("--load", default=DEFAULT_BIN, help=f"fastcap.bin (default {DEFAULT_BIN})")
    g.add_argument("--swd-khz", type=int, default=DEFAULT_SWD_KHZ, help=f"SWD kHz (default {DEFAULT_SWD_KHZ})")
    g.add_argument("--quality", type=int, default=DEFAULT_QUALITY,
                   help=f"JPEG quality 1..100 (default {DEFAULT_QUALITY})")
    g.set_defaults(func=run_grab)

    a = sub.add_parser("analyze", help="per-frame PNG stats")
    a.add_argument("--dir", default=DEFAULT_OUT_DIR, help=f"frame dir (default {DEFAULT_OUT_DIR})")
    a.add_argument("--from", dest="from_n", type=int, default=1, help="start at frame N")
    a.set_defaults(func=run_analyze)

    return p


def main():
    args = build_parser().parse_args()
    try:
        args.func(args)
    except KeyboardInterrupt:
        print("\nInterrupted.")


if __name__ == "__main__":
    main()
