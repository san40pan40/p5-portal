#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# Builds the firmware, sends it to the board over Wi-Fi (OTA) and does a smoke test.
# The script prints only the important lines, so the logs stay short.
#   uv run scripts/dev.py [all|build|ota|check|usb] [--ip 192.168.50.62] [--pass <panel password>]
# "all" is the default. It does build, then ota, then check.
# The panel password comes from --pass or from the environment variable P5_PASS.
# Exit code 1: a step failed.
import argparse
import base64
import json
import os
import random
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, ".pio", "build", "esp32-s3-zero", "firmware.bin")
PAGES = ("/", "/dns", "/bypass", "/config", "/style.css", "/app.js")

# esptool prints Unicode progress bars. In a cp1251 console, the output thread of PIO stops
# with an error and the upload does not end. UTF-8 mode prevents this.
PIO_ENV = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8")

ip = ""
auth = ""


def out(line):
    print(line, flush=True)


def fail(line):
    out(line)
    sys.exit(1)


def find_pio():
    home = os.path.expanduser("~")
    for p in (os.path.join(home, ".platformio", "penv", "Scripts", "pio.exe"),
              os.path.join(home, ".platformio", "penv", "bin", "pio")):
        if os.path.isfile(p):
            return p
    p = shutil.which("pio") or shutil.which("platformio")
    if not p:
        fail("pio not found: install PlatformIO")
    return p


def run_pio(args, keep):
    """Runs pio and prints the lines that match the regex keep. Returns the exit code."""
    r = subprocess.run([find_pio(), *args], cwd=ROOT, env=PIO_ENV, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, encoding="utf-8", errors="replace")
    lines = [s.strip() for s in r.stdout.splitlines() if re.search(keep, s, re.I)]
    for s in lines[:40]:
        out(s)
    return r.returncode


# ---------------------------------------------------------------- HTTP
def request(path, data=None, headers=None, timeout=5):
    """Sends one request to the board. Returns (status, headers, body). Status 0: no answer."""
    h = dict(headers or {})
    if auth:
        h["Authorization"] = auth
    if data is not None:
        h["Origin"] = f"http://{ip}"   # the browser sends this header with each POST
    req = urllib.request.Request(f"http://{ip}{path}", data=data, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()
    except (OSError, ValueError):
        return 0, {}, b""


def get_json(path):
    code, _, body = request(path)
    if code != 200:
        return None
    try:
        return json.loads(body)
    except ValueError:
        return None


# ---------------------------------------------------------------- DNS
def dns_a(name):
    """Sends an A query to the board on UDP port 53. Returns the IPv4 answers, or None if no answer."""
    qid = random.randrange(65536)
    q = struct.pack(">HHHHHH", qid, 0x0100, 1, 0, 0, 0)
    q += b"".join(bytes([len(p)]) + p.encode() for p in name.split(".")) + b"\0"
    q += struct.pack(">HH", 1, 1)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(3)
        for _ in range(2):
            try:
                s.sendto(q, (ip, 53))
                r, _ = s.recvfrom(1500)
            except OSError:
                continue
            if len(r) >= 12 and struct.unpack(">H", r[:2])[0] == qid:
                return parse_a(r)
    return None


def skip_name(r, i):
    while i < len(r):
        n = r[i]
        if n == 0:
            return i + 1
        if n & 0xC0 == 0xC0:   # compression pointer: 2 bytes
            return i + 2
        i += n + 1
    return i


def parse_a(r):
    qd, an = struct.unpack(">HH", r[4:8])
    i = 12
    for _ in range(qd):
        i = skip_name(r, i) + 4
    ips = []
    for _ in range(an):
        i = skip_name(r, i)
        if i + 10 > len(r):
            break
        rtype, _, _, rdlen = struct.unpack(">HHIH", r[i:i + 10])
        i += 10
        if rtype == 1 and rdlen == 4:
            ips.append(socket.inet_ntoa(r[i:i + 4]))
        i += rdlen
    return ips


# ---------------------------------------------------------------- commands
def cmd_build():
    if run_pio(["run", "-d", ROOT], r"error|warning: unused|RAM:|Flash:|SUCCESS|FAILED"):
        fail("BUILD FAILED")


def wait_board():
    for _ in range(30):
        if request("/", timeout=2)[0] == 200:
            return True
        time.sleep(1)
    return False


def cmd_ota():
    if not os.path.isfile(BIN):
        fail(f"no {BIN} - build first")
    t = time.time()
    with open(BIN, "rb") as f:
        fw = f.read()
    # The form on /config sends the file in the multipart field "fw"
    boundary = f"----p5dev{random.randrange(1 << 48):x}"
    body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"fw\"; filename=\"firmware.bin\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n").encode() + fw + f"\r\n--{boundary}--\r\n".encode()
    _, _, resp = request("/update", body, {"Content-Type": f"multipart/form-data; boundary={boundary}"}, 120)
    txt = resp.decode("utf-8", "replace")
    if "url=/'" not in txt:   # only the restart page has this redirect
        txt = re.sub(r"<style[\s\S]*?</style>", "", txt)
        txt = re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", txt)).strip()
        fail(f"OTA FAILED: {txt[:300] or 'no answer'}")
    time.sleep(3)
    if not wait_board():
        fail("OTA sent, board did not come back in 30 s")
    out(f"OTA OK in {time.time() - t:.0f} s")


def cmd_check():
    bad = 0
    for p in PAGES:
        code = request(p)[0]
        if code != 200:
            out(f"FAIL {p} -> {code}")
            bad += 1
    # Live values API: valid JSON with the uptime key
    j = get_json("/api/status")
    if not isinstance(j, dict) or not j.get("up"):
        out("FAIL /api/status: no JSON")
        bad += 1
    # Chart history API: both sets are valid JSON with the interval key
    for s in ("sys", "dns"):
        h = get_json(f"/api/hist?set={s}")
        if not isinstance(h, dict) or not h.get("iv"):
            out(f"FAIL /api/hist?set={s}: no JSON")
            bad += 1
    # The board keeps the static files gzipped and sends them gzipped
    _, hd, _ = request("/style.css", headers={"Accept-Encoding": "gzip"})
    if (hd.get("Content-Encoding") or "").lower() != "gzip":
        out("FAIL /style.css: not gzip")
        bad += 1
    ads = dns_a("doubleclick.net")
    if not ads or "0.0.0.0" not in ads:
        # The filter switch is the /dns/toggle form: its checkbox has "checked" when the filter is on.
        html = request("/dns")[2].decode("utf-8", "replace")
        a = html.find("action='/dns/toggle'")
        b = html.find("</form>", a)
        if a >= 0 and b > a and " checked" not in html[a:b]:
            out("WARN DNS: filter is switched OFF in the panel - blocking not tested")
        else:
            out("FAIL DNS: doubleclick.net not blocked")
            bad += 1
    ok = dns_a("example.com")
    if not ok or "0.0.0.0" in ok:
        out("FAIL DNS: example.com not resolved")
        bad += 1
    if bad:
        sys.exit(1)
    out("CHECK OK (6 pages 200, api + hist JSON, gzip css, DNS blocks + resolves)")


def cmd_usb(port, dry_run):
    args = ["run", "-d", ROOT, "-t", "upload"] + (["--upload-port", port] if port else [])
    if dry_run:
        out(" ".join([find_pio(), *args]))
        return
    if run_pio(args, r"error|SUCCESS|FAILED|Hard resetting"):
        fail("USB UPLOAD FAILED")


def main():
    global ip, auth
    # pio lines can contain characters that a cp1251 console cannot show
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description="Build, flash (OTA or USB) and smoke-test the P5 Portal board.")
    ap.add_argument("cmd", nargs="?", default="all", choices=("all", "build", "ota", "check", "usb"))
    ap.add_argument("--ip", default="192.168.50.62", help="board address (default 192.168.50.62)")
    ap.add_argument("--pass", dest="password", default=os.environ.get("P5_PASS", ""),
                    help="panel password (default: env P5_PASS)")
    ap.add_argument("--port", default="", help="usb: serial port (default: auto)")
    ap.add_argument("--dry-run", action="store_true", help="usb: print the pio command, do not run it")
    a = ap.parse_args()
    ip = a.ip
    if a.password:
        auth = "Basic " + base64.b64encode(f"admin:{a.password}".encode()).decode()
    if a.cmd == "build":
        cmd_build()
    elif a.cmd == "ota":
        cmd_ota()
    elif a.cmd == "check":
        cmd_check()
    elif a.cmd == "usb":
        cmd_usb(a.port, a.dry_run)
    else:
        cmd_build()
        cmd_ota()
        cmd_check()


if __name__ == "__main__":
    main()
