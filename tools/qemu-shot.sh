#!/bin/sh
# qemu-shot.sh NAME: a screenshot of the headless wbde as $WBDE_WORK/NAME.png
# (needs Python's PIL)
W=${WBDE_WORK:-/tmp/wbde-work}
echo "shot $W/$1.ppm" > "$W/ctl"; sleep 2
python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" \
  "$W/$1.ppm" "$W/$1.png"
