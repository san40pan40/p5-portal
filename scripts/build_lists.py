#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# Builds the preset blocklist the board downloads from the GitHub release "lists".
#   blocklists/sources.json (several upstream lists) -> ONE merged list:
#   <out>/p5.bin, <out>/index.json, <out>/<source id>.LICENSE.txt, <out>/NOTICE.txt
# Run by .github/workflows/blocklists.yml; by hand: uv run scripts/build_lists.py [--out DIR] [--local DIR]
# Python stdlib only.
#
# Input formats ("format" in sources.json):
#   hosts    "0.0.0.0 a.com b.com" / "127.0.0.1 a.com" (localhost-style entries are skipped)
#   domains  one domain per line
#   adblock  "||a.com^" lines (anything else - exceptions, paths, modifiers - is skipped)
# All sources are merged. Every entry means "the domain and all its subdomains" (the board walks the
# parents of a queried name), so an entry whose parent domain is also listed is dropped. Single-label
# names (bare TLDs) are dropped: the board never looks them up.
#
# Hash: FNV-1a 64-bit (offset basis 0xcbf29ce484222325, prime 0x100000001b3) over the ASCII bytes of
# the lowercased domain in IDNA/punycode form, without a trailing dot, folded to 48 bits:
#   h = 0xcbf29ce484222325; for each byte b: h ^= b; h = (h * 0x100000001b3) mod 2^64
#   h48 = (h ^ (h >> 48)) & 0xFFFFFFFFFFFF
# 48 bits keep ~115k domains in ~690 KB; a false match is about 1e-9 per lookup.
#
# p5.bin layout (all little-endian):
#   0  char[4] magic "P5BL"
#   4  u8      version = 1
#   5  u8      hashBits = 48 (entry size = hashBits / 8 bytes; the firmware also accepts 64)
#   6  u16     reserved = 0
#   8  u32     count
#   12 u32     crc32 (zlib.crc32) of the hash payload that follows
#   16 entries[count], 6 bytes each, sorted ascending, unique
#
# index.json: {"v":1,"gen":"<sha>","lists":[{"id","name","license","homepage","count","bytes","crc","updated","src"}]}
#   flat objects only; crc = payload crc32 as an unsigned decimal; bytes = .bin size; updated = build date
#   (UTC, YYYY-MM-DD); gen = sha256 of this script + sources.json; src = sha256 of all raw upstream texts.
# --prev <old index.json>: if gen and src are unchanged nothing is built ("changed=false" goes to
#   $GITHUB_OUTPUT); otherwise <out>/changed.txt lists the files to upload.
import argparse
import datetime
import hashlib
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
SIZE_WARN = 850 * 1024  # LittleFS on the board is 960 KB in total
UA = "p5-portal-build-lists"
LIST_ID = "p5"
HASH_BITS = 48

FNV_OFFSET = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3
MASK64 = (1 << 64) - 1
MASK48 = (1 << 48) - 1

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


def h48(domain):
    h = fnv1a64(domain)
    return (h ^ (h >> 48)) & MASK48


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


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def gh_output(line):
    path = os.environ.get("GITHUB_OUTPUT")
    if path:
        with open(path, "a") as f:
            f.write(line + "\n")


def main():
    ap = argparse.ArgumentParser(description="Build the P5 Portal preset blocklist (.bin + index.json).")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output directory (default blocklists/out)")
    ap.add_argument("--sources", default=SOURCES, help="sources.json path")
    ap.add_argument("--local", help="read <DIR>/<id>.txt instead of downloading (tests)")
    ap.add_argument("--prev", help="previous index.json: skip the build if nothing changed")
    ap.add_argument("--sample", nargs="*", default=["doubleclick.net", "ad.de.doubleclick.net", "example.com"],
                    help="domains whose hashes are printed (for checking the firmware side)")
    args = ap.parse_args()

    with open(args.sources, encoding="utf-8") as f:
        sources = json.load(f)
    today = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
    gen_h = hashlib.sha256()
    for path in (os.path.abspath(__file__), args.sources):
        with open(path, "rb") as f:
            gen_h.update(f.read())
    gen = gen_h.hexdigest()[:16]

    texts = {}
    src_h = hashlib.sha256()
    for src in sources:
        url = os.path.join(args.local, src["id"] + ".txt") if args.local else src["url"]
        try:
            texts[src["id"]] = fetch(url)
        except Exception as e:  # noqa: BLE001 - report and fail the build
            sys.exit("ERROR: %s: cannot read %s: %s" % (src["id"], url, e))
        src_h.update(hashlib.sha256(texts[src["id"]]).digest())
    src_sha = src_h.hexdigest()[:16]

    prev = None
    if args.prev and os.path.exists(args.prev):
        try:
            with open(args.prev, encoding="utf-8") as f:
                prev = json.load(f)
        except Exception as e:  # noqa: BLE001 - a bad old index just means "rebuild"
            print("previous index unreadable (%s): full rebuild" % e)
    prev_entry = ((prev or {}).get("lists") or [{}])[0]
    if prev and prev.get("gen") == gen and prev_entry.get("id") == LIST_ID and prev_entry.get("src") == src_sha:
        print("UNCHANGED: upstream lists and builder are the same as in the previous index; nothing to do")
        gh_output("changed=false")
        return

    os.makedirs(args.out, exist_ok=True)
    domains = set()
    names, licenses = [], []
    changed = [LIST_ID + ".bin", "index.json", "NOTICE.txt"]
    notice = ["P5 Portal preset blocklist", "",
              "p5.bin holds 48-bit hashes of the domains of the upstream lists below, merged (see",
              "scripts/build_lists.py in https://github.com/san40pan40/p5-portal). The upstream",
              "lists, their authors and licenses:", ""]
    for src in sources:
        sid = src["id"]
        raw = list(parse(texts[sid].decode("utf-8", errors="replace"), src["format"]))
        good = {d for d in (normalize(n) for n in raw) if d}
        print("%-14s entries %7d  valid unique %7d" % (sid, len(raw), len(good)))
        domains |= good
        names.append(src["name"])
        if src["license"] not in licenses:
            licenses.append(src["license"])
        notice += ["%s: %s" % (sid, src["name"]), "  source:   %s" % src["url"],
                   "  homepage: %s" % src["homepage"], "  license:  %s" % src["license"]]
        if src.get("notice_url"):
            lic = sid + ".LICENSE.txt"
            try:
                data = fetch(src["notice_url"])
            except Exception as e:  # noqa: BLE001
                sys.exit("ERROR: %s: cannot read license %s: %s" % (sid, src["notice_url"], e))
            with open(os.path.join(args.out, lic), "wb") as f:
                f.write(data)
            notice.append("  license text: %s" % lic)
            changed.append(lic)
        notice.append("")

    by_hash = {}
    for d in drop_children(domains):
        h = h48(d)
        if h in by_hash:
            sys.exit("COLLISION %012x: %s / %s" % (h, by_hash[h], d))
        by_hash[h] = d
    hashes = sorted(by_hash)
    payload = b"".join(h.to_bytes(HASH_BITS // 8, "little") for h in hashes)
    crc = zlib.crc32(payload) & 0xffffffff
    data = b"P5BL" + struct.pack("<BBHII", 1, HASH_BITS, 0, len(hashes), crc) + payload
    with open(os.path.join(args.out, LIST_ID + ".bin"), "wb") as f:
        f.write(data)
    print("merged unique %d, after parent dedupe %d -> %s.bin %d bytes (%.0f KB) crc %08x"
          % (len(domains), len(hashes), LIST_ID, len(data), len(data) / 1024, crc))
    if len(data) > SIZE_WARN:
        print("WARNING: %s.bin is %d KB (> %d KB); LittleFS is 960 KB in total"
              % (LIST_ID, len(data) // 1024, SIZE_WARN // 1024))

    entry = {"id": LIST_ID, "name": " + ".join(names), "license": " + ".join(licenses),
             "homepage": "https://github.com/san40pan40/p5-portal/releases/tag/lists",
             "count": len(hashes), "bytes": len(data), "crc": crc, "updated": today, "src": src_sha}
    write_text(os.path.join(args.out, "index.json"),
               '{"v":1,"gen":"%s","lists":[\n%s\n]}\n' % (gen, json.dumps(entry, ensure_ascii=True,
                                                                         separators=(",", ":"))))
    write_text(os.path.join(args.out, "NOTICE.txt"), "\n".join(notice))
    write_text(os.path.join(args.out, "changed.txt"), "\n".join(changed) + "\n")
    # Notes for the GitHub release "lists" (not uploaded as a file)
    rows = "\n".join("| [%s](%s) | %s |" % (s["name"], s["homepage"], s["license"]) for s in sources)
    write_text(os.path.join(args.out, "RELEASE_NOTES.md"), """This release holds the preset blocklist for P5 Portal. It is not a firmware release.

### Current list
- File: `p5.bin`
- Domains: %d
- Size: %d KB
- Build date: %s

### How to use
On the DNS page of the panel, turn on the switch **%s**. The board downloads the list and checks for a new version each day.

### Sources
| List | License |
|---|---|
%s

Each list keeps its upstream license. See `NOTICE.txt` and the `*.LICENSE.txt` files.

The workflow `blocklists` builds this release. It changes the files only when an upstream list changes.
""" % (len(hashes), len(data) // 1024, today, LIST_ID, rows))
    gh_output("changed=true")
    for d in args.sample:
        print("h48(%s) = %012x" % (d, h48(d)))
    print("OK -> %s; upload: %s" % (args.out, " ".join(changed)))


if __name__ == "__main__":
    main()
