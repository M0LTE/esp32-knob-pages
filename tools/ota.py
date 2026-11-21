#!/usr/bin/env python3
"""Update a desk knob over Wi-Fi.

    python3 tools/ota.py --host desk-knob build/desk_knob.bin

Uploads the app image to the knob's /api/ota (HTTP basic auth, user "admin"
by default), then watches /api/version until the new version answers. A new
image has to prove itself healthy within about 90 s of starting or the knob
goes back to the previous one by itself; this reports which happened.

The admin password comes from $DESK_KNOB_PASSWORD, or the first line of a
file (--password-file, default ~/.config/desk-knob/admin). It is never printed.
Python 3 standard library only.
"""
import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request


def image_version(data):
    # esp_image_header_t (24 bytes) + first segment header (8) = 32, then esp_app_desc_t:
    # magic (4), secure_version (4), reserved (8), version[32], project_name[32].
    if len(data) < 32 + 80 or data[0] != 0xE9:
        sys.exit("not an ESP app image")
    desc = data[32:32 + 80]
    if int.from_bytes(desc[0:4], "little") != 0xABCD5432:
        sys.exit("no app description in the image")
    version = desc[16:48].split(b"\0")[0].decode(errors="replace")
    project = desc[48:80].split(b"\0")[0].decode(errors="replace")
    return version, project


def get_version(base, timeout=3):
    try:
        with urllib.request.urlopen(base + "/api/version", timeout=timeout) as r:
            return json.load(r)
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser(description="Update a desk knob over Wi-Fi.")
    ap.add_argument("--host", required=True, help="knob host name or address")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password-file", default=os.path.expanduser("~/.config/desk-knob/admin"))
    ap.add_argument("--wait", type=int, default=180, help="seconds to wait for the new version to settle")
    ap.add_argument("image")
    a = ap.parse_args()

    password = os.environ.get("DESK_KNOB_PASSWORD")
    if not password:
        try:
            with open(a.password_file) as f:
                password = f.readline().strip()
        except OSError:
            sys.exit(f"no password: set DESK_KNOB_PASSWORD or put it in {a.password_file}")

    data = open(a.image, "rb").read()
    version, project = image_version(data)
    base = "http://" + a.host
    before = get_version(base)
    if not before:
        sys.exit(f"{a.host} does not answer /api/version")
    print(f"{a.host}: running {before['version']} from {before['partition']}")
    print(f"uploading {project} {version} ({len(data) // 1024} KB)")

    auth = base64.b64encode(f"{a.user}:{password}".encode()).decode()
    req = urllib.request.Request(base + "/api/ota", data=data, method="POST",
                                 headers={"Content-Type": "application/octet-stream",
                                          "Authorization": "Basic " + auth})
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            reply = json.load(r)
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        if e.code == 401:
            sys.exit("refused: wrong user or password")
        sys.exit(f"refused ({e.code}): {body}")
    if not reply.get("ok"):
        sys.exit(f"refused: {reply}")
    print(f"written in {time.time() - t0:.0f} s, restarting into {reply.get('version')}")

    # Watch it come back; a new image is on trial until it marks itself healthy.
    restarted = time.time()
    time.sleep(8)
    deadline = time.time() + a.wait
    seen_new = False
    while time.time() < deadline:
        v = get_version(base)
        if v:
            if v["version"] == version:
                if not seen_new:
                    print(f"{version} is up on {v['partition']}; waiting for it to confirm itself")
                seen_new = True
            elif seen_new or time.time() - restarted > 100:
                sys.exit(f"rolled back: {a.host} is running {v['version']} again (see /api/crash)")
            if seen_new and time.time() - restarted > 125:
                break  # past the trial: it has confirmed itself
        time.sleep(3)
    if seen_new:
        print(f"done: {a.host} runs {version}")
    else:
        sys.exit(f"{a.host} did not come back with {version} within {a.wait} s")


if __name__ == "__main__":
    main()
