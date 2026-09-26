#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Who else has the capture node open?
#
# M244: a run of the M238 test failed with VIDIOC_REQBUFS "Device or resource
# busy" and pixelformat stuck at H264, while the counter watch showed 2591 H.264
# frames delivered to a consumer this project never started. Something on the
# desktop - WirePlumber probes every new V4L2 node on udev add, and the portals
# sit behind it - opened /dev/video0 the moment the module created it.
#
# That is not a curiosity. It costs encoder spawns out of a budget of 8-18 per
# power cycle, it makes a capture fail for a reason that looks like a driver
# defect, and two consumers contending is a live candidate for M232's silent
# persistent encoder: 20 attachments with the pipeline running and VB2 detached
# is what contention looks like from inside the driver.
#
# Needs root to see other users' file descriptors. Without it, it can only see
# your own, so it says so rather than reporting a clean node it cannot verify.
set -u

NODE=${1:-}
if [ -z "$NODE" ]; then
	for n in /sys/class/video4linux/video*/name; do
		[ -r "$n" ] || continue
		case "$(cat "$n")" in
		mz0380*|"HD60 Pro HDMI capture"*)
			NODE=/dev/$(basename "$(dirname "$n")"); break;;
		esac
	done
fi
[ -n "$NODE" ] || { echo "no mz0380 node found"; exit 1; }

found=0
for p in /proc/[0-9]*; do
	pid=$(basename "$p")
	for fd in "$p"/fd/*; do
		[ -e "$fd" ] || continue
		[ "$(readlink "$fd" 2>/dev/null)" = "$NODE" ] || continue
		echo "  $NODE held by pid $pid ($(cat "$p/comm" 2>/dev/null)): $(tr '\0' ' ' < "$p/cmdline" 2>/dev/null | cut -c1-80)"
		found=1
		break
	done
done

if [ "$found" = 0 ]; then
	if [ "$(id -u)" = 0 ]; then
		echo "  $NODE: no other process has it open"
	else
		echo "  $NODE: none of MY processes hold it - run as root to see everyone's"
	fi
fi
exit 0
