#!/bin/bash
# M215: does mask bit 0 turn the 16-byte stub into a whole frame?
#
# M214 established, statically, that tinyvenc7's vcap_handler writes the raw
# channel on EVERY frame at one of two lengths - ALIGN16(W)*H*3/2 when the
# preview scheduler selects the frame, and exactly 16 bytes when it does not -
# and that the selection bitmap is only ever written behind SET_PREVIEW_PARAMS
# mask bit 0. With post_mask=0 the bitmap stays zero, nothing is selected, and
# every frame gets the stub. That is the whole of the "16 bytes per frame"
# observation, from M128d through M212.
#
# This run tests the prediction directly: with post_mask bit 0 set, selected
# frames should arrive as 0x2f7600-byte transfers.
#
# WHY THIS SCRIPT DOES NOT LOAD THE MODULE
#
# Every other harness in this tree rmmods and reinsmods, which spends a spawn
# and replaces the operator's parameters with its own. This one deliberately
# rides an already-loaded driver: the configuration under test is a load-time
# parameter set, so re-loading it here would either duplicate the operator's
# spawn or silently test something else. Load first, with:
#
#   sudo env VICFW=7 H264PROBE=1 POLLDRAIN=0 WINSEQ=1 OP6=1 \
#        POSTMASK=0x01 POSTSKIP=29 FASTKILL=0 H264DIVISOR=0 PERSIST=1 \
#        ./mz0380-live.sh load
#
# RAWOBS=1 must be part of that load line. The raw banks are allocated in
# mz0380_dma_setup(), which runs at PCI probe, so raw_bank_observe is read at
# INSMOD - setting it through sysfs on a loaded module reaches SET_BUF with no
# banks behind it and stream start dies with -ENODEV. The first M215 attempt
# did exactly that and produced a run with no frames at all.
#
# THE MODULE IS UNLOADED AT THE END, ON PURPOSE.
#
# The extent dump - the only thing that distinguishes a 16-byte stub from a
# whole frame - is emitted by mz0380_raw_probe_bufs_dump(dev, "stop") in the
# stream-stop path. With persistent_h264=1 (PERSIST=1, part of the known-good
# load) a V4L2 STREAMOFF only detaches VB2; the pipeline stays up and that path
# never runs. Attempt 2 streamed 900 frames perfectly and still produced no
# extents for exactly this reason. So this script forces the final stop itself
# and reads the dump from the unload. Re-load before running it again.
#
# BANDWIDTH, AND WHY POSTSKIP MATTERS
#
# The bitmap step is skip+1, so POSTSKIP=0 selects every frame: 3110400 * 60 =
# ~186 MB/s, on a link this card negotiates as PCIe x1 Gen1. POSTSKIP=29 asks
# for every 30th frame, ~2 full frames/s, which proves the mechanism at ~6
# MB/s. Start there and walk down only if it works.
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

SECS=${SECS:-15}
PARAM=/sys/module/mz0380/parameters

die() { echo "M215 ABORT: $*" >&2; exit 1; }
get() { cat "$PARAM/$1" 2>/dev/null; }

[ -d "$PARAM" ] || die "mz0380 is not loaded - see the header for the load line"

# --- preconditions -----------------------------------------------------
# Check rather than set: post_mask and post_skip are read when the stream
# starts, but the operator's whole point is that THIS load is the experiment.
# Silently correcting them here would hide a mis-load.
MASK=$(get post_mask)
SKIP=$(get post_skip)
[ $(( MASK & 1 )) -eq 1 ] || die "post_mask=$MASK has bit 0 clear - nothing will change; reload with POSTMASK=0x01"
echo "M215: post_mask=$MASK post_skip=$SKIP post_avg=$(get post_avg)"
echo "      -> preview bitmap step = skip+1 = $(( SKIP + 1 )), i.e. every $(( SKIP + 1 ))th frame"

for p in h264_probe dma_iova_remap; do
	[ "$(get $p)" = "Y" ] || die "$p is not set - raw_bank_observe requires it"
done
[ "$(get raw_bank_probe)" = "N" ] || die "raw_bank_probe must be 0 - M215 rides the encoded path, it does not replace it"

NODE=$(for n in /sys/class/video4linux/video*/name; do
		case "$(cat "$n" 2>/dev/null)" in
		mz0380*) echo "/dev/$(basename "$(dirname "$n")")"; break;;
		esac
	done)
[ -n "$NODE" ] || die "no mz0380 V4L2 node"

[ "$(get raw_bank_observe)" = "Y" ] ||
	die "raw_bank_observe=N. It is a LOAD-TIME parameter - do not set it through
       sysfs, the banks are allocated at probe and stream start will fail with
       -ENODEV. Reload with RAWOBS=1 added to the load line in this header."
echo "M215: raw_bank_observe=Y on $NODE, capturing ${SECS}s"

FRAMES=$(( SECS * 60 ))
LOG=docs/m215-preview-enable-$(date +%Y-%m-%d).log

dmesg -C
# Keep v4l2-ctl's output: when the first attempt produced nothing, discarding
# this is what cost us the diagnostic.
CAPOUT=$(v4l2-ctl -d "$NODE" --stream-mmap --stream-count="$FRAMES" \
	--stream-to=/dev/null 2>&1)
RC=$?
sleep 1

# Force the final pipeline stop so the extent dump is emitted. Under
# persistent_h264 the STREAMOFF above did not stop anything card-side.
PERSIST=$(get persistent_h264)
if [ "$PERSIST" = "Y" ]; then
	echo "M215: persistent_h264=Y, so STREAMOFF did not stop the pipeline;"
	echo "      unloading to force the stop-time extent dump"
fi
./mz0380-live.sh unload >/dev/null 2>&1
sleep 1
dmesg > "$LOG"

# --- scoring -----------------------------------------------------------
# The verdict is the extent dump at stream stop. 0x10 means we are still on
# the stub arm; 0x2f7600 means a whole frame landed.
echo
echo "=== raw bank extents at stop ==="
grep -o 'raw bank[0-9] op0x[0-9a-f]* buf\[[0-9]\] .*extent=0x[0-9a-f]*[^,]*' "$LOG" |
	sed 's/@[^ ]* //' | sed 's/^/  /'

FULL=$(grep -c 'extent=0x2f7600' "$LOG")
STUB=$(grep -c 'extent=0x10 ' "$LOG")
SLOTS=$(grep -c 'raw bank[0-9] op0x' "$LOG")
FAILED=$(grep -cE 'SET_BUF failed|dma_start failed|alloc failed' "$LOG")
# pr_info_ratelimited drops lines, so the number of LINES understates the real
# count badly (attempt 2: 40 lines, but the counters in them reached ~230 per
# slot). Report the highest counter any slot reached, not the line count.
HEADS=$(grep -o 'head changed (#[0-9]*)' "$LOG" | grep -o '[0-9]*' |
	sort -n | tail -1)
HEADLINES=$(grep -c 'M212 raw slot' "$LOG")
# "h264 frames: N delivered" - match the field, not the digits in "h264".
H264=$(awk -F'h264 frames:' '/h264 frames:/{print $2; exit}' /proc/mz0380-state |
	awk '{print $1}')

echo
echo "=== M215 totals ==="
echo "  slots at full frame (0x2f7600) : $FULL"
echo "  slots still at the stub (0x10) : $STUB"
echo "  max head changes seen on a slot: ${HEADS:-0} (from $HEADLINES logged lines)"
echo "  encoded frames delivered       : ${H264:-0}"
echo "  v4l2-ctl rc                    : $RC"
echo "  log                            : $LOG"

# Three outcomes, not two. A run that never streamed and a run that streamed
# but produced no extent dump are both "not tested", and neither is evidence
# against the hypothesis - but they have different causes and different fixes.
if [ "$FAILED" -gt 0 ] || [ "${H264:-0}" -eq 0 ]; then
	echo "M215 NULL RUN - nothing streamed, so the question was not tested."
	echo "  raw bank dumps seen: $SLOTS, encoded frames: ${H264:-0}"
	grep -E 'failed|error' "$LOG" | sed 's/^.*mz0380[^ ]* /    /' | head -5
	[ -n "$CAPOUT" ] && { echo "  v4l2-ctl said:"; echo "$CAPOUT" | sed 's/^/    /' | head -5; }
	echo
	echo "  Do NOT read this as bit 0 failing. Fix the run and repeat."
	./scripts/mz0380-spawns.sh
	exit 2
fi

if [ "$SLOTS" -eq 0 ]; then
	echo "M215 INCONCLUSIVE - ${H264:-0} frames streamed, but no extent dump was"
	echo "  emitted, so stub-vs-whole-frame is still unmeasured. The dump comes"
	echo "  from the stream-stop path; something kept the pipeline up. Check that"
	echo "  the unload above actually ran."
	echo "  Do NOT read this as bit 0 failing."
	./scripts/mz0380-spawns.sh
	exit 3
fi

echo
if [ "$FULL" -gt 0 ]; then
	echo "M215 POSITIVE: mask bit 0 turns the stub into whole frames."
	echo "  The preview writer is enabled by the selection bitmap, exactly as"
	echo "  M214 predicted. Next: walk POSTSKIP down (9, 4, 1) and watch for"
	echo "  the link or the ring to give out."
else
	echo "M215 NEGATIVE: every slot is still at the 16-byte stub."
	echo "  Bit 0 alone did not populate the bitmap. Re-read the handler at"
	echo "  0xeb54: the bit-0 path also requires a non-zero fps byte, and"
	echo "  tiny_calculate_skip_fps early-outs when fps == 0. Check the"
	echo "  SET_PREVIEW_PARAMS line in the log for the fps actually sent."
	grep -o 'SET_PREVIEW_PARAMS(.*)' "$LOG" | sed 's/^/    /' | head -3
fi

# The module is left loaded on purpose, so commit the spawn here rather than
# in an unload path that is not going to run.
echo
./scripts/mz0380-spawns.sh commit
