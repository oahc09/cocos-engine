#!/usr/bin/env python3
import os
import subprocess
import sys
import threading
from pathlib import Path
from typing import Optional


CHILD_CMD = [r"C:\Users\caosh\.local\bin\renderdoc-mcp.exe"]
CHILD_ENV = {
    "FASTMCP_SHOW_CLI_BANNER": "false",
    "FASTMCP_CHECK_FOR_UPDATES": "off",
    "FASTMCP_LOG_LEVEL": "ERROR",
    "PYTHONWARNINGS": "ignore",
}


class ModeState:
    def __init__(self):
        self.mode: Optional[str] = None  # "headers" or "line"
        self._lock = threading.Lock()

    def set_if_unset(self, mode: str):
        with self._lock:
            if self.mode is None:
                self.mode = mode

    def get(self) -> Optional[str]:
        with self._lock:
            return self.mode


def _read_client_message(stdin):
    first = stdin.readline()
    if not first:
        return None, None

    stripped = first.strip()
    if stripped.startswith(b"{") or stripped.startswith(b"["):
        return stripped, "line"

    headers = {}
    line = first
    while True:
        if not line:
            return None, None
        if line in (b"\r\n", b"\n"):
            break
        if b":" in line:
            key, value = line.split(b":", 1)
            headers[key.strip().lower()] = value.strip()
        line = stdin.readline()

    if b"content-length" not in headers:
        return b"", "headers"
    length = int(headers[b"content-length"])
    body = stdin.read(length) if length > 0 else b""
    if body is None:
        return None, None
    return body, "headers"


def _write_content_length_framed(stdout, payload: bytes):
    header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
    stdout.write(header)
    stdout.write(payload)
    stdout.flush()


def _pump_client_to_child(child, mode_state: ModeState):
    in_buf = sys.stdin.buffer
    out_buf = child.stdin
    while True:
        body, mode = _read_client_message(in_buf)
        if body is None:
            break
        if mode:
            mode_state.set_if_unset(mode)
        if body.strip():
            out_buf.write(body.rstrip(b"\r\n") + b"\n")
            out_buf.flush()
    try:
        child.stdin.close()
    except Exception:
        pass


def _pump_child_to_client(child, mode_state: ModeState):
    in_buf = child.stdout
    out_buf = sys.stdout.buffer
    while True:
        line = in_buf.readline()
        if not line:
            break
        payload = line.rstrip(b"\r\n")
        if not payload:
            continue
        mode = mode_state.get() or "headers"
        if mode == "line":
            out_buf.write(payload + b"\n")
            out_buf.flush()
        else:
            _write_content_length_framed(out_buf, payload)


def _pump_stderr(child):
    for line in child.stderr:
        sys.stderr.buffer.write(line)
        sys.stderr.buffer.flush()


def main():
    env = dict(os.environ)
    env.update(CHILD_ENV)
    child = subprocess.Popen(
        CHILD_CMD,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
        env=env,
        cwd=str(Path.cwd()),
    )

    mode_state = ModeState()
    t1 = threading.Thread(target=_pump_client_to_child, args=(child, mode_state), daemon=True)
    t2 = threading.Thread(target=_pump_child_to_client, args=(child, mode_state), daemon=True)
    t3 = threading.Thread(target=_pump_stderr, args=(child,), daemon=True)
    t1.start()
    t2.start()
    t3.start()

    rc = child.wait()
    try:
        sys.exit(rc)
    except SystemExit:
        raise


if __name__ == "__main__":
    main()
