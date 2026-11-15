#!/usr/bin/env bash
#
# mz0380-signal-hunt.sh — locate the HDMI signal / lock register empirically by
# diffing register captures with the HDMI source UNPLUGGED vs PLUGGED.
#
# Milestone B of the frame-capture bring-up. Takes several samples per cable
# state to filter free-running counters, then reports which registers actually
# track plug state (LOCK-BIT vs ACTIVITY candidates).
#
# Surfaces (RE 2026-07-04, in recommended order):
#   EVENTS=1  (RECOMMENDED) — live event watcher. The card pushes signal/format
#             changes as edge events on BAR0 EVENT(0x30)+payloads(0x40..0x4c);
#             ep.ko has no signal register and no signal-getter command. This is
#             the real mechanism. Loads dma_handshake=1; guided plug/unplug.
#   SCAN_BAR=1 (default)    — BAR5/CFG 4K plug/unplug diff. Fully-backed, fast.
#   SCAN_BAR=0              — BAR0/MMIO. >0x5c is un-backed post-boot; no signal
#             regs. Kept for completeness.
#   PERIPH=1               — bridge-chip REG_READ scan. NOTE: opcode 0x1a is NOT
#             a real card command (returns a constant 0x48); kept only to
#             re-demonstrate that dead end. Do not trust its output.
#
# Usage:
#   sudo EVENTS=1 scripts/mz0380-signal-hunt.sh        # live event watch (recommended)
#   sudo scripts/mz0380-signal-hunt.sh                 # BAR5 register diff
#   sudo scripts/mz0380-signal-hunt.sh --no-reload     # reuse the already-loaded module
#   sudo scripts/mz0380-signal-hunt.sh --analyze DIR   # re-run analysis on an earlier capture
#
# Env overrides:
#   SAMPLES=6  INTERVAL=0.3  WORK=/tmp/mz0380-signal-hunt  READ_TIMEOUT
#   PERIPH=1 PERIPH_CHIP=0x90 PERIPH_START=0x00 PERIPH_COUNT=0x40
#   SCAN_BAR=1|0  SCAN_UNSAFE=0|1  WIN_START  WIN_LEN
#
set -u
cd "$(dirname "$0")/.."

MOD=./mz0380.ko
SCAN=/proc/mz0380-scan
STATE=/proc/mz0380-state
PARAM=/sys/module/mz0380/parameters

SAMPLES="${SAMPLES:-6}"
INTERVAL="${INTERVAL:-0.3}"
WORK="${WORK:-/tmp/mz0380-signal-hunt}"

# PERIPH=1 walks a chip's registers through the mailbox REG_READ (0x1a) command
# path instead of raw BAR MMIO. This is the RE-preferred signal surface: BAR0
# exposes no signal regs (see the SCAN_BAR note), while the HDMI bridge chip
# (0x90) is the real front-end. The command path needs bus mastering, so PERIPH
# mode loads with dma_handshake=1.
# EVENTS=1 runs the live event watcher instead of a register scan: the card
# pushes signal/format changes as edge events on BAR0 EVENT(0x30) with payloads
# at 0x40..0x4c (RE 2026-07-04 — there is no pollable signal register). This is
# the correct milestone-B path. Needs the command/event path up (dma_handshake=1).
EVENTS="${EVENTS:-0}"

PERIPH="${PERIPH:-0}"
PERIPH_PROBE="${PERIPH_PROBE:-0}"      # 1 = one-shot full mailbox-slot dump, no plug toggle
[[ "$PERIPH_PROBE" == "1" ]] && PERIPH=1
PERIPH_CHIP="${PERIPH_CHIP:-0x90}"
PERIPH_START="${PERIPH_START:-0x00}"
PERIPH_COUNT="${PERIPH_COUNT:-0x40}"
if [[ "$PERIPH" == "1" ]]; then
	SCAN=/proc/mz0380-periph-scan
	READ_TIMEOUT="${READ_TIMEOUT:-20}"
else
	READ_TIMEOUT="${READ_TIMEOUT:-6}"
fi

# Which BAR to diff.
#
# RE result 2026-07-04: BAR0/MMIO above 0x5c is UN-BACKED post-boot. Every readl
# there eats a ~200ms PCIe completion timeout and returns 0xffffffff, so the
# sc0710-style HDMI status window (0xa8..0xe4) shows nothing on plug/unplug — the
# sc0710 register model does not apply to this card. Only 0x04..0x5c (the command
# mailbox aperture) is backed. BAR5/CFG is a fully-backed, fast 4K window that
# already mirrors encoder state, so it is the default surface for the hunt.
#
#   SCAN_BAR=1  BAR5/CFG  (default) full 4K sweep, backed + fast
#   SCAN_BAR=0  BAR0/MMIO           0x00..0x60 backed region only (safe/fast);
#                                   SCAN_UNSAFE=1 widens to 0x0..0x400 but each
#                                   read costs ~200ms — set READ_TIMEOUT=90.
SCAN_BAR="${SCAN_BAR:-1}"
SCAN_UNSAFE="${SCAN_UNSAFE:-0}"

if [[ "$SCAN_BAR" == "1" ]]; then
	WIN_START="${WIN_START:-0x0}"
	WIN_LEN="${WIN_LEN:-0x1000}"
elif [[ "$SCAN_UNSAFE" == "1" ]]; then
	WIN_START="${WIN_START:-0x0}"
	WIN_LEN="${WIN_LEN:-0x400}"
else
	WIN_START="${WIN_START:-0x0}"
	WIN_LEN="${WIN_LEN:-0x60}"
fi

need_root() { [[ $EUID -eq 0 ]] || { echo "run as root (sudo $0 $*)" >&2; exit 1; }; }

load_deps() {
	modprobe -a videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc \
		v4l2-dv-timings snd snd-pcm 2>/dev/null || true
}

reload_with_fw() {
	local extra=""
	[[ "$PERIPH" == "1" || "$EVENTS" == "1" ]] && extra="dma_handshake=1"   # command/event path needs bus master
	echo "[hunt] building..."; make -s || exit 1
	rmmod mz0380 2>/dev/null || true
	load_deps
	echo "[hunt] loading with $extra (boots the card, ~20s)..."
	insmod "$MOD" procfs_verbosity=2 enable_video=1 $extra \
		|| { echo "[hunt] insmod failed"; dmesg | tail -20; exit 1; }
}

wait_ready() {
	local i
	echo -n "[hunt] waiting for firmware ready"
	for i in $(seq 1 45); do
		if grep -q 'fw state   : ready' "$STATE" 2>/dev/null; then
			echo " — ready."
			grep -E 'fw version|fw state' "$STATE" | sed 's/^/[hunt]   /'
			return 0
		fi
		echo -n "."; sleep 1
	done
	echo
	echo "[hunt] WARNING: firmware never reported ready. The BAR0 signal window"
	echo "       may read 0xffffffff. Continuing anyway (results may be noise)."
	return 1
}

set_window() {
	[[ -e "$SCAN" ]] || { echo "[hunt] $SCAN missing — module not loaded, or built without this scan proc. Rebuild+reload."; exit 1; }
	if [[ "$PERIPH" == "1" ]]; then
		[[ -e "$PARAM/periph_chip" ]] || { echo "[hunt] loaded module lacks periph-scan params (old build) — reload: drop --no-reload."; exit 1; }
		echo "$PERIPH_CHIP"  > "$PARAM/periph_chip"  2>/dev/null || true
		echo "$PERIPH_START" > "$PARAM/periph_start" 2>/dev/null || true
		echo "$PERIPH_COUNT" > "$PARAM/periph_count" 2>/dev/null || true
		echo "$PERIPH_PROBE" > "$PARAM/periph_probe" 2>/dev/null || true
		echo "[hunt] periph scan: chip $PERIPH_CHIP regs $PERIPH_START count $PERIPH_COUNT probe=$PERIPH_PROBE (via mailbox REG_READ 0x1a)"
		grep -q 'fw state   : ready' "$STATE" 2>/dev/null \
			|| echo "[hunt] WARNING: firmware not ready — periph reads will fail."
		return
	fi
	[[ -e "$PARAM/scan_unsafe" ]] || { echo "[hunt] loaded module is an OLD build (no scan_unsafe guard) — reload: drop --no-reload."; exit 1; }
	echo "$SCAN_BAR"    > "$PARAM/scan_bar"    2>/dev/null || true
	echo "$WIN_START"  > "$PARAM/scan_start"  2>/dev/null || true
	echo "$WIN_LEN"    > "$PARAM/scan_len"    2>/dev/null || true
	echo "$SCAN_UNSAFE" > "$PARAM/scan_unsafe" 2>/dev/null || true
	local barname="BAR5/CFG"; [[ "$SCAN_BAR" == "0" ]] && barname="BAR0/MMIO"
	echo "[hunt] scan window: $barname $WIN_START len $WIN_LEN (unsafe=$SCAN_UNSAFE)"
	if [[ "$SCAN_UNSAFE" == "1" ]]; then
		echo "[hunt] UNSAFE mode: BAR0 above 0x5c is un-backed — each read eats a"
		echo "[hunt]   ~200ms PCIe completion timeout (returns 0xffffffff), so a full"
		echo "[hunt]   0x400 sweep takes ~50s. Not a hard hang, but set READ_TIMEOUT=90"
		echo "[hunt]   or it will abort. Per-offset trace: sudo dmesg | grep 'scan probing'."
	fi
}

capture_state() {
	local label="$1" k rc
	echo "[hunt] capturing '$label': $SAMPLES samples @ ${INTERVAL}s ..."
	for k in $(seq 1 "$SAMPLES"); do
		timeout "$READ_TIMEOUT" cat "$SCAN" > "$WORK/$label.$k"
		rc=$?
		if [[ $rc -eq 124 ]]; then
			echo "[hunt] read timed out after ${READ_TIMEOUT}s — an offset stalled the link."
			echo "[hunt] (a truly hung readl is uninterruptible; if the box is wedged,"
			echo "[hunt]  reboot, then sudo dmesg | grep 'scan probing' | tail -1 for the offset.)"
			exit 1
		fi
		[[ $rc -eq 0 ]] || { echo "[hunt] read $SCAN failed (rc=$rc)"; exit 1; }
		sleep "$INTERVAL"
	done
}

analyze() {
	local dir="$1"
	python3 - "$dir" "$SAMPLES" <<'PY'
import sys, glob, re, os
dir_, samples = sys.argv[1], int(sys.argv[2])
# Matches both raw-BAR lines  "bar0[0x00a8] = 12345678"
# and periph lines           "periph[0x90][0x12] = 00000001".
rx = re.compile(r'([a-z0-9]+(?:\[0x[0-9a-fA-F]+\])+)\s*=\s*([0-9a-fA-F]{8})')

def reg_of(label):
    """Last 0x.. bracket in a label = the register/offset (for sort + cross-check)."""
    hexes = re.findall(r'0x([0-9a-fA-F]+)', label)
    return int(hexes[-1], 16) if hexes else 0

def load(state):
    vals = {}                          # label -> list of observed values
    files = sorted(glob.glob(os.path.join(dir_, f'{state}.*')))
    for f in files:
        for line in open(f):
            m = rx.search(line)
            if m:
                vals.setdefault(m.group(1), []).append(int(m.group(2), 16))
    return vals, len(files)

u, un = load('unplugged')
p, pn = load('plugged')
if not u or not p:
    print("  no captures found in", dir_); sys.exit(0)

labels = sorted(set(u) | set(p), key=reg_of)
lock, activity = [], []
for lab in labels:
    us, ps = set(u.get(lab, [])), set(p.get(lab, []))
    if not us or not ps or us == ps:
        continue                       # absent in a state, or identical -> skip
    row = (lab, sorted(us), sorted(ps))
    (lock if len(us) == 1 and len(ps) == 1 else activity).append(row)

def fmt(vs):
    return ",".join(f"{v:08x}" for v in vs) if len(vs) <= 3 else \
           f"{min(vs):08x}..{max(vs):08x}({len(vs)} vals)"

w = max([len(l) for l in labels] + [12]) + 1
print(f"\n===== signal-register analysis ({un} unplugged + {pn} plugged samples) =====")
print("A LOCK-BIT candidate holds one steady value with the source unplugged and")
print("a different steady value when plugged in.\n")

if lock:
    print("  LOCK-BIT candidates (steady in each state, differ between):")
    print(f"    {'reg':<{w}} {'unplugged':<26} {'plugged':<26} changed-bits")
    for lab, us, ps in lock:
        xor = us[0] ^ ps[0]
        bits = ",".join(str(b) for b in range(32) if xor & (1 << b)) or "-"
        print(f"    {lab:<{w}} {fmt(us):<26} {fmt(ps):<26} {xor:08x} [bit {bits}]")
else:
    print("  LOCK-BIT candidates: none (no register was steady-but-different).")

if activity:
    print("\n  ACTIVITY candidates (varies within a state — counters / running flags):")
    print(f"    {'reg':<{w}} {'unplugged':<26} {'plugged':<26}")
    for lab, us, ps in activity:
        print(f"    {lab:<{w}} {fmt(us):<26} {fmt(ps):<26}")

changed = {reg_of(r[0]) for r in lock + activity}
if any(l.startswith('bar0[') for l in labels):
    print("\n  cross-check vs sc0710-ported guesses in mz0380_signal_from_bar0():")
    for name, off in [("HDMI_WIDTH",0x00a8),("HDMI_DETECT_A",0x00ac),("HDMI_CONTROL",0x00c4),
                      ("HDMI_HEIGHT",0x00c8),("HDMI_STATUS_A",0x00d0),("HDMI_STREAMING",0x00e4)]:
        print(f"    0x{off:04x} {name:<16} {'CHANGED' if off in changed else 'unchanged'}")
if any(l.startswith('periph[0x90]') for l in labels):
    print("\n  cross-check vs the old bridge-0x90 signal guess (MZ0380_BRIDGE_SIGNAL):")
    print(f"    reg 0x12 bit0   {'CHANGED' if 0x12 in changed else 'unchanged'}")
PY
}

run_event_capture() {
	local EV=/proc/mz0380-events
	[[ -e "$EV" ]] || { echo "[hunt] $EV missing — old build without the event watcher. Reload (drop --no-reload)."; exit 1; }
	grep -q 'fw state   : ready' "$STATE" 2>/dev/null \
		|| echo "[hunt] WARNING: firmware not ready — events may not fire."
	echo stop  > "$EV" 2>/dev/null || true
	echo clear > "$EV" 2>/dev/null || true
	echo start > "$EV" 2>/dev/null || { echo "[hunt] could not start watcher"; exit 1; }
	cat <<EOF

[hunt] EVENT WATCHER running (kthread sampling BAR0 EVENT + payloads).
[hunt] The card pushes signal changes as edge events — no cable-state register.
[hunt]
[hunt] Now toggle the HDMI SOURCE a few times so we catch both edges:
[hunt]    1. UNPLUG the source,  wait ~3s
[hunt]    2. PLUG it back in,    wait ~5s for it to sync
[hunt]    3. repeat once or twice
EOF
	read -r -p "[hunt] Press Enter when you have finished toggling... " _
	echo stop > "$EV" 2>/dev/null || true
	echo
	echo "[hunt] ===================== /proc/mz0380-events ====================="
	cat "$EV"
	cat <<EOF

[hunt] Each '+<us> EVENT=...' line is one card->host edge. Read the payload
[hunt] words p=w0 w1 w2 w3: the word(s) that flip between your unplug and plug
[hunt] actions carry the signal state (and likely width/height/fps). That is the
[hunt] signal notification to decode next (map to the pciep_isr path that writes
[hunt] BAR0+0x40..0x4c). If NO events appeared at all, the card may not raise
[hunt] host interrupts without DMA fully armed — note it and we RE the event path.
EOF
}

# ---- entry ----------------------------------------------------------------
if [[ "${1:-}" == "--analyze" ]]; then
	[[ -n "${2:-}" ]] || { echo "usage: $0 --analyze DIR" >&2; exit 1; }
	analyze "$2"; exit 0
fi

need_root "$@"
mkdir -p "$WORK"; rm -f "$WORK"/unplugged.* "$WORK"/plugged.* 2>/dev/null || true

if [[ "${1:-}" == "--no-reload" ]]; then
	echo "[hunt] --no-reload: reusing the already-loaded module"
	grep -q 'fw state   : ready' "$STATE" 2>/dev/null \
		|| { echo "[hunt] firmware not ready — drop --no-reload to boot the card"; exit 1; }
else
	reload_with_fw
	wait_ready || true
fi

if [[ "$EVENTS" == "1" ]]; then
	run_event_capture
	exit 0
fi

set_window

if [[ "$PERIPH_PROBE" == "1" ]]; then
	echo
	echo "[hunt] PROBE mode — single mailbox-slot dump (no plug toggle needed):"
	echo
	timeout "$READ_TIMEOUT" cat "$SCAN"
	echo
	echo "[hunt] Read the dump above: find the slot whose value is non-zero AND differs"
	echo "[hunt] per register — that is where REG_READ returns data (fix periph_read to"
	echo "[hunt] read it). If every slot is 0 with STATUS=0xaaaaaaaa, the command completes"
	echo "[hunt] but returns nothing (wrong opcode framing, or the front-end is powered"
	echo "[hunt] down until an input is selected / streaming starts — milestone C)."
	exit 0
fi

cat <<EOF

[hunt] Two-state capture. Toggle only the HDMI SOURCE cable when asked; leave
       everything else (this PC, the card) untouched between the two captures.

EOF
read -r -p "[hunt] 1/2 — ensure the HDMI source is UNPLUGGED, then press Enter... " _
capture_state unplugged
read -r -p "[hunt] 2/2 — now PLUG IN the source, wait ~5s for it to sync, then press Enter... " _
capture_state plugged

analyze "$WORK"
echo
echo "[hunt] raw samples kept in $WORK (re-analyze: sudo $0 --analyze $WORK)"
