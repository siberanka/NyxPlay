#!/usr/bin/env python3
"""
Synchronise the NyxPlay version across all components.

NyxPlay uses calendar versioning: ``YYYY.MM.DD.BUILD`` (for example ``2026.11.11.1``).
The value is stored in the repository root ``VERSION`` file and copied into:

* ``host/CMakeLists.txt``      -> ``project(NyxPlayHost VERSION ...)``
* ``android/app/build.gradle`` -> ``versionName`` / ``versionCode`` (``YYYYMMDDBB``)
* ``pc/app/version.txt``       -> qmake ``VERSION``

Usage::

    python scripts/set-version.py                 # apply the VERSION file
    python scripts/set-version.py 2026.11.12.1    # set a new version everywhere
    python scripts/set-version.py --next          # today's date, next build number
    python scripts/set-version.py --check         # verify all components agree
"""

from __future__ import annotations

import argparse
import datetime as _dt
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION_FILE = ROOT / "VERSION"
VERSION_RE = re.compile(r"^(\d{4})\.(\d{1,2})\.(\d{1,2})\.(\d{1,2})$")


def parse(version: str) -> tuple[int, int, int, int]:
    match = VERSION_RE.match(version.strip())
    if not match:
        raise SystemExit(f"Invalid version '{version}', expected YYYY.MM.DD.BUILD (build 1-99)")
    year, month, day, build = (int(x) for x in match.groups())
    _dt.date(year, month, day)  # validates the calendar date
    if not 1 <= build <= 99:
        raise SystemExit("The build number must be between 1 and 99 (Android versionCode limit)")
    return year, month, day, build


def normalise(version: str) -> str:
    year, month, day, build = parse(version)
    return f"{year}.{month:02d}.{day:02d}.{build}"


def version_code(version: str) -> int:
    year, month, day, build = parse(version)
    return int(f"{year}{month:02d}{day:02d}{build:02d}")


def _replace(path: Path, pattern: str, repl: str) -> bool:
    text = path.read_text(encoding="utf-8")
    new, count = re.subn(pattern, repl, text, count=1, flags=re.M)
    if count != 1:
        raise SystemExit(f"Could not find the version field in {path.relative_to(ROOT)}")
    if new != text:
        path.write_bytes(new.encode("utf-8"))
        return True
    return False


def apply(version: str) -> None:
    code = version_code(version)
    changed = []
    if _replace(ROOT / "host" / "CMakeLists.txt", r"^(project\(NyxPlayHost VERSION )[0-9.]+", rf"\g<1>{version}"):
        changed.append("host/CMakeLists.txt")
    gradle = ROOT / "android" / "app" / "build.gradle"
    if _replace(gradle, r'^(\s*versionName ")[^"]+(")', rf"\g<1>{version}\g<2>"):
        changed.append("android/app/build.gradle (versionName)")
    if _replace(gradle, r"^(\s*versionCode = )\d+", rf"\g<1>{code}"):
        changed.append("android/app/build.gradle (versionCode)")
    pc_version = ROOT / "pc" / "app" / "version.txt"
    if pc_version.read_text(encoding="utf-8").strip() != version:
        pc_version.write_text(version + "\n", encoding="utf-8", newline="\n")
        changed.append("pc/app/version.txt")
    if VERSION_FILE.read_text(encoding="utf-8").strip() != version:
        VERSION_FILE.write_text(version + "\n", encoding="utf-8", newline="\n")
        changed.append("VERSION")
    print(f"NyxPlay {version} (Android versionCode {code})")
    for item in changed:
        print(f"  updated {item}")
    if not changed:
        print("  everything already up to date")


def check(version: str) -> int:
    code = version_code(version)
    problems = []
    host = (ROOT / "host" / "CMakeLists.txt").read_text(encoding="utf-8")
    if f"project(NyxPlayHost VERSION {version}" not in host:
        problems.append("host/CMakeLists.txt")
    gradle = (ROOT / "android" / "app" / "build.gradle").read_text(encoding="utf-8")
    if f'versionName "{version}"' not in gradle or f"versionCode = {code}" not in gradle:
        problems.append("android/app/build.gradle")
    if (ROOT / "pc" / "app" / "version.txt").read_text(encoding="utf-8").strip() != version:
        problems.append("pc/app/version.txt")
    if problems:
        print(f"Version mismatch with VERSION ({version}): " + ", ".join(problems))
        return 1
    print(f"All components are at {version}")
    return 0


def next_version(current: str) -> str:
    today = _dt.date.today()
    year, month, day, build = parse(current)
    if (year, month, day) == (today.year, today.month, today.day):
        return f"{year}.{month:02d}.{day:02d}.{build + 1}"
    return f"{today.year}.{today.month:02d}.{today.day:02d}.1"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version", nargs="?", help="new version (YYYY.MM.DD.BUILD)")
    parser.add_argument("--next", action="store_true", help="bump to today's date / next build number")
    parser.add_argument("--check", action="store_true", help="only verify that all components agree")
    args = parser.parse_args()

    current = normalise(VERSION_FILE.read_text(encoding="utf-8"))
    if args.check:
        return check(current)
    if args.next:
        target = next_version(current)
    elif args.version:
        target = normalise(args.version)
    else:
        target = current
    apply(target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
