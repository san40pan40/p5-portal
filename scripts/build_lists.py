#!/usr/bin/env python3
# Builds the preset blocklists the board downloads from the GitHub release "lists".
#   blocklists/sources.json  ->  <out>/<id>.bin, <out>/index.json, <out>/<id>.LICENSE.txt, <out>/NOTICE.txt
# Run by .github/workflows/blocklists.yml; by hand: python scripts/build_lists.py [--out DIR] [--local DIR]
# Python stdlib only.
#
# Input formats ("format" in sources.json):
#   hosts    "0.0.0.0 a.com b.com" / "127.0.0.1 a.com" (localhost-style entries are skipped)
#   domains  one domain per line
#   adblock  "||a.com^" lines (anything else - exceptions, paths, modifiers - is skipped)
# Every entry means "the domain and all its subdomains" (the board walks the parents of a queried name),
# so an entry whose parent domain is also listed is dropped. Single-label names (bare TLDs) are dropped:
# the board never looks them up.
#
# Hash: FNV-1a 64-bit (offset basis 0xcbf29ce484222325, prime 0x100000001b3) over the ASCII bytes of
# the lowercased domain in IDNA/punycode form, without a trailing dot:
#   h = 0xcbf29ce484222325; for each byte b: h ^= b; h = (h * 0x100000001b3) mod 2^64
#
# <id>.bin layout (all little-endian):
#   0  char[4] magic "P5BL"
#   4  u8      version = 1
#   5  u8      hashBits = 64
#   6  u16     reserved = 0
#   8  u32     count
#   12 u32     crc32 (zlib.crc32) of the hash payload that follows
#   16 u64[count] hashes, sorted ascending, unique
#
# index.json: {"v":1,"lists":[{"id","name","license","homepage","count","bytes","crc","updated"}, ...]}
#   flat objects only; crc is the payload crc32 as an unsigned decimal number, bytes is the .bin size,
#   updated is the build date (UTC, YYYY-MM-DD).
import argparse
import datetime
import json
import os
import re
import struct
import sys
import urllib.request
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = os.path.join(ROOT, "blocklists", "sources.json")
DEFAULT_OUT = os.path.join(ROOT, "blocklists", "out")
SIZE_WARN = 700 * 1024  # LittleFS on the board is 960 KB in total
UA = "p5-portal-build-lists"

FNV_OFFSET = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3
MASK64 = (1 << 64) - 1

HOSTS_SKIP = {"localhost", "localhost.localdomain", "local", "broadcasthost", "ip6-localhost",
              "ip6-loopback", "ip6-localnet", "ip6-mcastprefix", "ip6-allnodes", "ip6-allrouters",
              "ip6-allhosts", "0.0.0.0"}
LABEL_RE = re.compile(r"^[a-z0-9_]([a-z0-9_-]{0,61}[a-z0-9_])?$")
ADBLOCK_RE = re.compile(r"^\|\|([^\^/$*|]+)\^$")
IPV4_RE = re.compile(r"^\d{1,3}(\.\d{1,3}){3}$")


def fnv1a64(domain):
    h = FNV_OFFSET
    for b in domain.encode("ascii"):
        h ^= b
        h = (h * FNV_PRIME) & MASK64
    return h


def fetch(url):
    if os.path.exists(url):  # local path (tests)
        with open(url, "rb") as f:
            return f.read()
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def normalize(name):
    """Lowercased ASCII (IDNA) hostname without trailing dot, or None if invalid."""
    d = name.strip().strip(".").lower()
    if not d:
        return None
    if not d.isascii():
        try:
            d = d.encode("idna").decode("ascii").lower()
        except UnicodeError:
            return None
    if len(d) > 253 or d in HOSTS_SKIP or IPV4_RE.match(d):
        return None
    labels = d.split(".")
    if len(labels) < 2 or not all(LABEL_RE.match(l) for l in labels):
        return None
    return d


def parse(text, fmt):
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line or line.startswith("!") or line.startswith("["):
            continue
        if fmt == "hosts":
            parts = line.split()
            if len(parts) < 2:
                continue
            yield from parts[1:]
        elif fmt == "domains":
            yield line.split()[0]
        elif fmt == "adblock":
            m = ADBLOCK_RE.match(line)
            if m:
                yield m.group(1)
        else:
            raise ValueError("unknown format: %s" % fmt)


def drop_children(domains):
    """Keep only domains none of whose parents (with >= 2 labels) are also in the set."""
    out = []
    for d in domains:
        labels = d.split(".")
        if not any(".".join(labels[i:]) in domains for i in range(1, len(labels) - 1)):
            out.append(d)
    return out


def build(src, text, out_dir):
    raw = list(parse(text, src["format"]))
    domains = set()
    bad = 0
    for name in raw:
        d = normalize(name)
        if d is None:
            bad += 1
        else:
            domains.add(d)
    kept = drop_children(domains)
    by_hash = {}
    collisions = []
    for d in kept:
        h = fnv1a64(d)
        if h in by_hash and by_hash[h] != d:
            collisions.append((h, by_hash[h], d))
        by_hash[h] = d
    hashes = sorted(by_hash)
    payload = struct.pack("<%dQ" % len(hashes), *hashes)
    crc = zlib.crc32(payload) & 0xffffffff
    header = b"P5BL" + struct.pack("<BBHII", 1, 64, 0, len(hashes), crc)
    data = header + payload
    with open(os.path.join(out_dir, src["id"] + ".bin"), "wb") as f:
        f.write(data)
    print("%-14s entries %7d  invalid/skipped %6d  unique %7d  after parent dedupe %7d  -> %s.bin %d bytes (%.0f KB) crc %08x"
          % (src["id"], len(raw), bad, len(domains), len(hashes), src["id"], len(data), len(data) / 1024, crc))
    if len(data) > SIZE_WARN:
        print("WARNING: %s.bin is %d KB (> %d KB); LittleFS is 960 KB in total"
              % (src["id"], len(data) // 1024, SIZE_WARN // 1024))
    return {"count": len(hashes), "bytes": len(data), "crc": crc}, collisions


def main():
    ap = argparse.ArgumentParser(description="Build P5 Portal preset blocklists (.bin + index.json).")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output directory (default blocklists/out)")
    ap.add_argument("--sources", default=SOURCES, help="sources.json path")
    ap.add_argument("--local", help="read <DIR>/<id>.txt instead of downloading (tests)")
    ap.add_argument("--sample", nargs="*", default=["doubleclick.net", "ad.de.doubleclick.net", "example.com"],
                    help="domains whose hashes are printed (for checking the firmware side)")
    args = ap.parse_args()

    with open(args.sources, encoding="utf-8") as f:
        sources = json.load(f)
    os.makedirs(args.out, exist_ok=True)
    today = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")

    lists = []
    notice = ["P5 Portal preset blocklists", "",
              "Each <id>.bin holds 64-bit FNV-1a hashes of the domains of an upstream list (see",
              "scripts/build_lists.py in https://github.com/san40pan40/p5-portal). The upstream",
              "lists, their authors and licenses:", ""]
    failed = False
    for src in sources:
        sid = src["id"]
        url = os.path.join(args.local, sid + ".txt") if args.local else src["url"]
        try:
            text = fetch(url).decode("utf-8", errors="replace")
        except Exception as e:  # noqa: BLE001 - report and fail the build
            print("ERROR: %s: cannot read %s: %s" % (sid, url, e))
            failed = True
            continue
        info, collisions = build(src, text, args.out)
        for h, a, b in collisions:
            print("COLLISION %016x: %s / %s" % (h, a, b))
            failed = True
        lists.append({"id": sid, "name": src["name"], "license": src["license"], "homepage": src["homepage"],
                      "count": info["count"], "bytes": info["bytes"], "crc": info["crc"], "updated": today})
        notice += ["%s: %s" % (sid, src["name"]), "  source:   %s" % src["url"],
                   "  homepage: %s" % src["homepage"], "  license:  %s" % src["license"]]
        if src.get("notice_url"):
            lic = sid + ".LICENSE.txt"
            try:
                with open(os.path.join(args.out, lic), "wb") as f:
                    f.write(fetch(src["notice_url"]))
                notice.append("  license text: %s" % lic)
            except Exception as e:  # noqa: BLE001
                print("ERROR: %s: cannot read license %s: %s" % (sid, src["notice_url"], e))
                failed = True
        notice.append("")

    with open(os.path.join(args.out, "index.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write('{"v":1,"lists":[\n')
        f.write(",\n".join(json.dumps(l, ensure_ascii=True, separators=(",", ":")) for l in lists))
        f.write("\n]}\n")
    with open(os.path.join(args.out, "NOTICE.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(notice))

    for d in args.sample:
        print("fnv1a64(%s) = %016x" % (d, fnv1a64(d)))
    if failed:
        print("FAILED")
        sys.exit(1)
    print("OK: %d lists -> %s" % (len(lists), args.out))


if __name__ == "__main__":
    main()
