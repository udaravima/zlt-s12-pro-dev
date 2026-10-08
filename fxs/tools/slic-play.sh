#!/bin/sh
# slic-play FILE.s16 [...]  - play raw S16LE 8 kHz mono audio to the handset.
# Needs the slic daemon (/etc/init.d/slic). Pick up the handset, then run this
# (or run it first: the line switches to TALK, lift within the clip's length).
# Convert a clip on a PC: ffmpeg -i in.mp3 -ac 1 -ar 8000 -f s16le out.s16
CTL=/var/run/slic.ctl
[ $# -gt 0 ] || { echo "usage: slic-play FILE.s16 [...]"; exit 1; }
[ -p "$CTL" ] || { echo "slic daemon not running: /etc/init.d/slic start"; exit 1; }

echo talk > "$CTL"
for f in "$@"; do
	echo "playing $f"
	cat "$f" > /dev/slicpcm || break
done
sleep 1				# let the driver's 0.5 s queue drain
echo release > "$CTL"
