#!/bin/bash
# shoot3.sh <tag> [which...] — wide/mid/low frames of FIELD_Barn into shots/<tag>_<cam>.png
D="/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-scratch/FIELD-BARN-2"; T=$1; shift
W=${@:-wide mid low}
for c in $W; do
  case $c in
    wide) bash "$D/shot.sh" "$D/shots/${T}_wide.png" 120 -2600,-150,6500 0.4,0.02,-0.92 ;;
    mid)  bash "$D/shot.sh" "$D/shots/${T}_mid.png" 120 300,330,800 0.33,0.06,-0.94 ;;
    low)  bash "$D/shot.sh" "$D/shots/${T}_low.png" 120 -2000,-115,4200 0.3,0.03,-0.95 ;;
  esac | head -1
done
