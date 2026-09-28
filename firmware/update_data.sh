#!/usr/bin/env bash
# Refreshes the preloaded contents of the badge's data partition from the live sources.
set -euo pipefail

MAP_URL="https://romhack.io/RomHack%20Camp%20Map.png"
SCHEDULE_URL="https://cfp.romhack.io/romhack-camp-2026/schedule/export/schedule.json"

DATA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/tactility/Data/data"
if [ ! -d "$DATA_DIR" ]; then
    echo "error: $DATA_DIR not found" >&2
    exit 1
fi

# Staged outside the data folder so a failed run cannot leave a partial file to be flashed.
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "Downloading map..."
curl -fsSL -D "$WORK_DIR/map.headers" -o "$WORK_DIR/map.png" "$MAP_URL"
mv "$WORK_DIR/map.png" "$DATA_DIR/map.png"

# The Last-Modified that MapApp sends back as If-Modified-Since, so a badge that already has this
# map answers its first sync with a 304 instead of transferring a megabyte again.
last_modified="$(sed -n 's/^[Ll]ast-[Mm]odified: *//p' "$WORK_DIR/map.headers" | tr -d '\r' | tail -1)"
if [ -n "$last_modified" ]; then
    printf '%s' "$last_modified" > "$DATA_DIR/map.png.stamp"
else
    rm -f "$DATA_DIR/map.png.stamp"
fi

echo "Downloading schedule..."
curl -fsSL -o "$WORK_DIR/schedule-full.json" "$SCHEDULE_URL"

# Reduced to the fields the app reads, in the shape serializeSchedule() in Agenda.cpp writes after
# a sync, so the preloaded file is the same thing the badge would produce for itself.
python3 - "$WORK_DIR/schedule-full.json" "$WORK_DIR/schedule.json" <<'PY'
import json
import sys

# The characters Agenda.cpp's normalizeText() substitutes for want of a Montserrat glyph.
SUBSTITUTIONS = {"‑": "-", "‛": "'", "‟": '"'}


def normalize(value):
    text = value if isinstance(value, str) else ""
    for old, new in SUBSTITUTIONS.items():
        text = text.replace(old, new)
    return text


with open(sys.argv[1], encoding="utf-8") as stream:
    schedule = json.load(stream)

days = []
for day in schedule["schedule"]["conference"]["days"]:
    rooms = {}
    for name, events in (day.get("rooms") or {}).items():
        kept = [
            {
                "title": normalize(event.get("title")),
                "start": normalize(event.get("start")),
                "track": event.get("track") or "",
            }
            for event in events
        ]
        kept.sort(key=lambda event: event["start"])
        rooms[normalize(name)] = kept
    days.append({"date": normalize(day.get("date")) or "Day", "rooms": rooms})

if not days:
    sys.exit("error: the schedule contains no days")

with open(sys.argv[2], "w", encoding="utf-8") as stream:
    json.dump({"schedule": {"conference": {"days": days}}}, stream, ensure_ascii=False, separators=(",", ":"))
PY
mv "$WORK_DIR/schedule.json" "$DATA_DIR/schedule.json"

echo
echo "Installed in $DATA_DIR:"
du -h "$DATA_DIR/map.png" "$DATA_DIR/schedule.json"
