#!/usr/bin/env bash
# Mo terminal SEGGER RTT (log + shell) qua J-Link, chi can day SWD.
#
# Probe J-Link hien tai KHONG tu quet duoc RTT Control Block tren nRF54L15
# (JLinkRTTLogger / west rtt bao "RTT Control Block not found"), nen script nay
# lay dia chi _SEGGER_RTT tu file ELF va chi dinh truc tiep cho J-Link.
#
# Dung:
#   scripts/rtt.sh [BUILD_DIR] [-l LOGFILE]
#   JLINK_SN=69750999 scripts/rtt.sh build/bike   (mac dinh: build/main)
# Thoat: Ctrl+C

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build/main"
LOGFILE=""

while [ $# -gt 0 ]; do
    case "$1" in
        -l|--log) LOGFILE="$2"; shift 2 ;;
        -h|--help) sed -n 2,12p "$0"; exit 0 ;;
        *) BUILD_DIR="$1"; shift ;;
    esac
done

# shellcheck disable=SC1091
source "$ROOT/env.sh" >/dev/null

# Sysbuild: ELF cua app nam o <build>/<ten_app>/zephyr/zephyr.elf
APP_NAME="$(sed -n 's/^APP_DIR:PATH=.*\///p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null || true)"
ELF="$BUILD_DIR/${APP_NAME:-$(basename "$BUILD_DIR")}/zephyr/zephyr.elf"
[ -f "$ELF" ] || ELF="$BUILD_DIR/zephyr/zephyr.elf"
if [ ! -f "$ELF" ]; then
    echo "[rtt] Khong tim thay zephyr.elf trong $BUILD_DIR - hay build truoc." >&2
    exit 1
fi

RTT_ADDR="$(arm-zephyr-eabi-nm "$ELF" | awk '/ _SEGGER_RTT$/ {print "0x"$1}')"
if [ -z "$RTT_ADDR" ]; then
    echo "[rtt] ELF khong co _SEGGER_RTT - bat CONFIG_USE_SEGGER_RTT=y trong prj.conf." >&2
    exit 1
fi

RTT_PORT="${RTT_PORT:-19021}"

# Moi lan chi nen co 1 phien RTT: 2 phien cung doc 1 bo dem se "gianh" log cua nhau
if ss -ltnpH "sport = :$RTT_PORT" 2>/dev/null | grep -q .; then
    echo "[rtt] Da co mot phien RTT khac dang mo tren cong $RTT_PORT:" >&2
    ss -ltnpH "sport = :$RTT_PORT" 2>/dev/null | sed 's/^/[rtt]   /' >&2
    echo "[rtt] Dong phien do (Ctrl+C trong terminal cua no) roi chay lai 'make rtt'." >&2
    exit 1
fi

SEL=()
[ -n "${JLINK_SN:-}" ] && SEL=(-USB "$JLINK_SN")

TMP="$(mktemp -d)"
FIFO="$TMP/jlink.in"
mkfifo "$FIFO"

cleanup() {
    { echo "exit" >&3; } 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    sleep 0.3
    [ -n "${JLINK_PID:-}" ] && kill "$JLINK_PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

# JLinkExe giu phien ket noi va mo RTT telnet server tren cong $RTT_PORT
JLinkExe -NoGui 1 -Device nRF54L15_M33 -If SWD -Speed 4000 -AutoConnect 1 \
    -RTTTelnetPort "$RTT_PORT" "${SEL[@]}" <"$FIFO" >"$TMP/jlink.log" 2>&1 &
JLINK_PID=$!
exec 3>"$FIFO"

# Ngay sau "make flash" probe co the con bi nrfutil giu -> ket noi lan dau that bai.
# Cho den khi JLinkExe nhan ra CPU, neu khong thi yeu cau connect lai.
connected=0
for attempt in 1 2 3 4 5; do
    for _ in $(seq 1 25); do
        if grep -q "Cortex-M33 identified" "$TMP/jlink.log" 2>/dev/null; then connected=1; break; fi
        kill -0 "$JLINK_PID" 2>/dev/null || break
        sleep 0.2
    done
    [ "$connected" = 1 ] && break
    kill -0 "$JLINK_PID" 2>/dev/null || break
    echo "[rtt] Chua ket noi duoc chip (lan $attempt), thu lai..." >&2
    echo "connect" >&3
done
if [ "$connected" != 1 ]; then
    echo "[rtt] Khong ket noi duoc nRF54L15 qua J-Link - kiem tra day SWD/nguon mach:" >&2
    tail -20 "$TMP/jlink.log" >&2
    exit 1
fi
echo "exec SetRTTAddr $RTT_ADDR" >&3

for _ in $(seq 1 50); do
    # Chi kiem tra cong dang LISTEN, khong ket noi thu (ket noi thu se "nuot" mat log dau)
    if ss -ltnH "sport = :$RTT_PORT" 2>/dev/null | grep -q .; then break; fi
    if ! kill -0 "$JLINK_PID" 2>/dev/null; then
        echo "[rtt] JLinkExe thoat som - kiem tra ket noi J-Link/nguon mach:" >&2
        cat "$TMP/jlink.log" >&2
        exit 1
    fi
    sleep 0.2
done

echo "[rtt] ELF=$ELF"
echo "[rtt] _SEGGER_RTT=$RTT_ADDR  telnet=localhost:$RTT_PORT  (Ctrl+C de thoat)"
if [ -n "$LOGFILE" ]; then
    JLinkRTTClient -LocalEcho Off -RTTTelnetPort "$RTT_PORT" | tee -a "$LOGFILE"
else
    JLinkRTTClient -LocalEcho Off -RTTTelnetPort "$RTT_PORT"
fi
