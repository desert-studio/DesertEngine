#!/bin/bash
# import.sh <gltf under Assets...> — live editor (live.sh) on FB2_Probe: drop each mesh, click Import, then save + quit.
D=/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-scratch/FIELD-BARN-2; C=$D/ctl.sh
for g in "$@"; do
  $C run Assets "Drop into the viewport: $g" >/dev/null; sleep 6
  for a in 1 2 3; do
    $C shot-window $D/win.png >/dev/null
    python3 $D/click.py 619 535 >/dev/null; sleep 10
    n=$(grep -c "\[Import\] $(basename $g)" $D/live.log); [ "$n" -gt 0 ] && break
  done
  # wait until the import's cooks settle (log quiet for TextureImporter/Import lines)
  for i in $(seq 60); do sleep 4; c=$($C --wait 200 state | python3 -c "import json,sys;print(json.load(sys.stdin)['scene']['modified'])"); [ "$c" = True ] && break; done
  echo "$g imported=$n modified=$c"
done
$C run Action "Save Scene" >/dev/null; sleep 5; $C quit 0 >/dev/null
