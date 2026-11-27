#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# Prints the release notes (Markdown) for one version tag.
# The notes are the section "## [X.Y.Z]" of CHANGELOG.md plus the installation steps.
# The release workflow runs it. By hand: uv run scripts/release_notes.py v0.2.0
# Exit code 1: CHANGELOG.md has no section for this version.
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

INSTALL = """### How to install
- **Online:** in the panel, go to **Settings → System**. Push **Check for updates**, then **Install**.
- **File:** download `firmware.bin` below. In the panel, go to **Settings → System** and upload the file.
- **First installation, or a board with firmware older than 0.2.0:** install over USB once.
  OTA cannot change the partition table. Export your rule lists first, because the USB installation removes them.
"""


def section(text, version):
    """Return the body of the section "## [version]" without its heading, or None."""
    m = re.search(r"^## \[%s\][^\n]*\n(.*?)(?=^## |^\[[^\]]+\]: |\Z)" % re.escape(version), text, re.M | re.S)
    return m.group(1).strip() if m else None


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: release_notes.py <tag>, for example v0.2.0")
    version = sys.argv[1].lstrip("v")
    with open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8") as f:
        body = section(f.read(), version)
    if not body:
        sys.exit("CHANGELOG.md has no section '## [%s]'. Add it before you push the tag." % version)
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")  # Windows consoles default to a legacy code page
    sys.stdout.write(body + "\n\n" + INSTALL)


if __name__ == "__main__":
    main()
