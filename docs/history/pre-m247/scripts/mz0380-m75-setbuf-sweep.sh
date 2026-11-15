#!/bin/bash
# M75: which mailbox opcode programs the encoder's DMA destination buffers?
#
# We have always sent 0x02. The Windows driver builds the SAME 12-word command
# (doorbell, opcode, channel, stride, then {hi,lo} address pairs) with opcodes
# 0x04 and 0x05 - win64.txt 0x140279219 and 0x140279663, r8d=0xc words, payload
# pulled from an address array in dword pairs.
#
# 0x02 provably reaches a channel: the card's synthetic NV12 path DMAs into
# those buffers. But the H.264 bitstream may come from a channel whose
# addresses 0x02 never programs - leaving the encoder running with nowhere to
# write, which is exactly what we see (receiver locked 59s, enc_stat 0, every
# host page untouched).
#
# Each pass costs ONE encoder spawn, so the whole sweep stays well under the
# ~8-18 spawn wedge budget.
#
# Decision: any pass reporting NONZERO "pages touched" identifies the opcode.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

OPS=${OPS:-"2 4 5 8"}

for OP in $OPS; do
	echo
	echo "############################################################"
	echo "###  set_buf_opcode = $OP"
	echo "############################################################"
	SETBUF="$OP" WATCH=0 scripts/mz0380-m55-real-capture.sh 2>&1 |
		grep -E "captured [0-9]+ bytes|pages touched|SET_BUF\(op|MST3367 signal|CARD WEDGED|No coherent"
	echo "--- (power-cycle the source again before the next pass) ---"
	sleep 3
done

echo
echo "=== sweep done. A pass with NONZERO 'pages touched' is the answer. ==="
echo "=== all zero => the destination opcode is not the blocker.        ==="
