#!/usr/bin/env python3
"""Receive potluck-amiga's frames on the PC: a window with the Amiga's picture, and the stream's numbers.

    python tools/frame_viewer.py [--port 47023] [--headless] [--log out.jsonl] [--png last.png]

The board sends each frame's changed lines as UDP (src/emu.cpp has the format): 320 x 256 pixels, one byte each
(RGB 3-3-2), four lines to a packet, then an empty packet that closes the frame. Every second this prints -- and
with --log writes as a JSON line -- the frames that arrived whole, the frames the board numbered but never closed
here (lost on the way), the lines and the kilobits a second. --headless skips the window (for measuring); --png
saves the last picture on exit. Standard library only (tkinter for the window).

Windows asks once whether Python may receive on this network: allow it on private networks, or nothing arrives.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time

W, H = 320, 256
ENTRY = 2 + W
LUT = [bytes(((v >> 5) * 255 // 7, ((v >> 2) & 7) * 255 // 7, (v & 3) * 255 // 3)) for v in range(256)]


class Stream:
    def __init__(self) -> None:
        self.rgb = bytearray(W * H * 3)
        self.frames = self.lost = self.lines = self.bytes = 0
        self.last_frame: int | None = None
        self.dirty = False

    def packet(self, data: bytes) -> bool:
        """Takes one packet; True when it closed a frame."""
        if len(data) < 8 or data[0:2] != b"PA" or data[2] != 1:
            return False
        n = data[3]
        frame = struct.unpack_from("<I", data, 4)[0]
        self.bytes += len(data)
        if n == 0:
            if self.last_frame is not None and frame > self.last_frame + 1:
                self.lost += frame - self.last_frame - 1  # numbers the board streamed but whose close never came
            if self.last_frame is None or frame > self.last_frame:
                self.last_frame = frame
                self.frames += 1
            return True
        for i in range(n):
            off = 8 + i * ENTRY
            if off + ENTRY > len(data):
                break
            y = data[off] | (data[off + 1] << 8)
            if y >= H:
                continue
            row = data[off + 2:off + ENTRY]
            self.rgb[y * W * 3:(y + 1) * W * 3] = b"".join(LUT[v] for v in row)
            self.lines += 1
        self.dirty = True
        return False

    def png(self, path: str) -> None:
        import zlib
        raw = b"".join(b"\x00" + bytes(self.rgb[y * W * 3:(y + 1) * W * 3]) for y in range(H))

        def chunk(tag: bytes, body: bytes) -> bytes:
            return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) +
                    chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=47023)
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--log")
    ap.add_argument("--png")
    a = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.bind(("0.0.0.0", a.port))
    sock.setblocking(False)
    s = Stream()
    log = open(a.log, "a", encoding="utf-8") if a.log else None
    prev = (0, 0, 0, 0)
    t_prev = time.time()

    def second() -> None:
        nonlocal prev, t_prev
        now = time.time()
        dt = now - t_prev
        if dt < 1.0:
            return
        f, lost, lines, b = s.frames - prev[0], s.lost - prev[1], s.lines - prev[2], s.bytes - prev[3]
        rec = {"t": round(now, 3), "fps": round(f / dt, 2), "lost": lost, "lines_s": round(lines / dt, 1),
               "kbps": round(b * 8 / 1000 / dt, 1), "frame": s.last_frame}
        print(json.dumps(rec), flush=True)
        if log:
            log.write(json.dumps(rec) + "\n")
            log.flush()
        prev, t_prev = (s.frames, s.lost, s.lines, s.bytes), now

    def drain() -> None:
        for _ in range(2000):
            try:
                data = sock.recv(2048)
            except (BlockingIOError, OSError):
                return
            s.packet(data)

    try:
        if a.headless:
            while True:
                drain()
                second()
                time.sleep(0.002)
        import tkinter as tk
        root = tk.Tk()
        root.title("potluck-amiga")
        img = tk.PhotoImage(width=W, height=H)
        big = img.zoom(2, 2)
        label = tk.Label(root, image=big, bg="black")
        label.pack()

        def tick() -> None:
            nonlocal big
            drain()
            second()
            if s.dirty:
                s.dirty = False
                img.configure(data=b"P6\n%d %d\n255\n" % (W, H) + bytes(s.rgb), format="PPM")
                big = img.zoom(2, 2)
                label.configure(image=big)
            root.after(15, tick)

        root.after(15, tick)
        root.mainloop()
    except KeyboardInterrupt:
        pass
    finally:
        if a.png:
            s.png(a.png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
