#!/usr/bin/env bash
#
# Builds Tactility and the Retro-Go apps, then merges them into fw.img: one
# 16MB image for offset 0x0.
#
# Usage: ./build_all.sh [--flash [port]] [--clean]
#
# Uses idf.py when it is on PATH, otherwise runs the same commands inside the
# espressif/idf container with podman or docker.
#
# Environment:
#   IDF_IMAGE         container image (default: docker.io/espressif/idf:v5.5.2)
#   CONTAINER_ENGINE  force "podman" or "docker"

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TACTILITY="$ROOT/tactility"
RETRO_GO="$ROOT/retro-go"
TARGET=romhack2026-badge
CHIP=esp32s3
FLASH_SIZE=16MB
IDF_IMAGE="${IDF_IMAGE:-docker.io/espressif/idf:v5.5.2}"
APPS=(launcher retro-core prboom-go)
OUT=fw.img

PORT=""
do_flash=false
do_clean=false
while [ $# -gt 0 ]; do
  case "$1" in
  --flash)
    do_flash=true
    if [ $# -gt 1 ] && [ -e "${2:-}" ]; then PORT="$2"; shift; fi
    ;;
  --clean) do_clean=true ;;
  *) echo "ERROR: unknown option '$1'" >&2; exit 1 ;;
  esac
  shift
done

# A local IDF is preferred: no image pull, and the build cache stays on the host
# either way because the trees are bind-mounted.
if [ -n "${CONTAINER_ENGINE:-}" ]; then
  engine="$CONTAINER_ENGINE"
elif command -v idf.py >/dev/null 2>&1; then
  engine=local
elif command -v podman >/dev/null 2>&1; then
  engine=podman
elif command -v docker >/dev/null 2>&1; then
  engine=docker
else
  echo "ERROR: no idf.py on PATH and neither podman nor docker is installed." >&2
  exit 1
fi
echo "Build engine: $engine"

engine_args=()
# Rootless podman maps container root onto the host user already; forcing a numeric
# user there writes files as an unusable subuid. Docker needs the opposite.
[ "$engine" = docker ] && engine_args+=(--user "$(id -u):$(id -g)" -e HOME=/tmp)

# run_in <host dir> <mount point> <shell command>
run_in() {
  local dir="$1" mnt="$2" cmd="$3"
  case "$engine" in
  local) (cd "$dir" && eval "$cmd") ;;
  *) "$engine" run --rm -i "${engine_args[@]}" -v "$dir:$mnt" -w "$mnt" "$IDF_IMAGE" bash -c "$cmd" ;;
  esac
}

if [ ! -f "$TACTILITY/Libraries/lvgl/CMakeLists.txt" ] ||
  [ ! -f "$TACTILITY/Libraries/esp-adf/components/esp-adf-libs/CMakeLists.txt" ]; then
  echo "ERROR: submodules are not initialised. Run: tactility/Buildscripts/init-submodules.sh" >&2
  exit 1
fi

if [ "$do_clean" = true ]; then
  rm -rf "$TACTILITY/build" "$TACTILITY/sdkconfig"
  for app in "${APPS[@]}"; do rm -rf "$RETRO_GO/$app/build"; done
fi

echo "==> Tactility"
run_in "$TACTILITY" /project "python device.py '$TARGET' && idf.py build"

echo "==> Retro-Go (${APPS[*]})"
run_in "$RETRO_GO" /retro-go "python rg_tool.py --target '$TARGET' build ${APPS[*]}"

echo "==> Checking each app against its partition"
python3 "$TACTILITY/Buildscripts/check-retrogo-partitions.py" "${APPS[@]}"

echo "==> Merging into $OUT"
python3 - "$ROOT" "${APPS[@]}" <<'PY' > "$ROOT/.merge_args"
import struct, sys, os
root, apps = sys.argv[1], sys.argv[2:]
build = os.path.join(root, "tactility/build")

# Tactility's own regions, at the offsets its build recorded.
args = []
with open(os.path.join(build, "flash_project_args")) as handle:
    for line in handle:
        parts = line.split()
        if not parts:
            continue
        if parts[0].startswith("--"):
            continue
        args.append((int(parts[0], 16), "tactility/build/" + parts[1]))

# The Retro-Go slots, read from the partition table the build actually produced
# rather than from the CSV, where the offsets are derived and left blank.
table = open(os.path.join(build, "partition_table/partition-table.bin"), "rb").read()
slots = {}
for i in range(0, len(table), 32):
    entry = table[i:i + 32]
    if entry[:2] != b"\xaa\x50":
        break
    offset, size = struct.unpack("<II", entry[4:12])
    slots[entry[12:28].rstrip(b"\x00").decode()] = offset

for app in apps:
    if app not in slots:
        sys.exit(f"ERROR: no '{app}' partition in the built table")
    args.append((slots[app], f"retro-go/{app}/build/{app}.bin"))

for offset, path in sorted(args):
    if not os.path.exists(os.path.join(root, path)):
        sys.exit(f"ERROR: missing {path}")
    print(f"0x{offset:x} {path}")
PY

merge_args="$(tr '\n' ' ' < "$ROOT/.merge_args")"
rm -f "$ROOT/.merge_args"

# esptool renamed merge_bin to merge-bin in v5; the IDF 5.5 image still ships v4.
run_in "$ROOT" /work "sub=\$(python -m esptool --help 2>&1 | grep -oE 'merge[-_]bin' | head -1); \
  python -m esptool --chip $CHIP \$sub -o $OUT --flash_mode dio --flash_freq 80m --flash_size $FLASH_SIZE $merge_args"

echo "==> $OUT  ($(stat -c%s "$ROOT/$OUT") bytes, flash at 0x0)"

if [ "$do_flash" = true ]; then
  if [ -z "$PORT" ]; then
    mapfile -t found < <(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true)
    [ "${#found[@]}" -eq 1 ] || { echo "ERROR: pass the port: --flash /dev/ttyACM0" >&2; exit 1; }
    PORT="${found[0]}"
  fi
  [ -w "$PORT" ] || { echo "ERROR: no write access to '$PORT' (add yourself to dialout)." >&2; exit 1; }
  echo "==> Flashing $OUT to $PORT"
  flash_args=(--device "$PORT")
  # Rootless podman drops the host user's supplementary groups, so dialout is lost.
  [ "$engine" = podman ] && flash_args+=(--group-add keep-groups)
  [ "$engine" = docker ] && flash_args+=(--group-add dialout)
  case "$engine" in
  local) (cd "$ROOT" && python -m esptool --chip "$CHIP" -p "$PORT" -b 460800 write_flash 0x0 "$OUT") ;;
  *) "$engine" run --rm -i "${engine_args[@]}" "${flash_args[@]}" -v "$ROOT:/work" -w /work "$IDF_IMAGE" \
      bash -c "python -m esptool --chip $CHIP -p '$PORT' -b 460800 write_flash 0x0 $OUT" ;;
  esac
fi
