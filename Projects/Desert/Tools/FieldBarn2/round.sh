#!/bin/bash
# round.sh <tag> — tiles + bale import + scatter + three frames
D=/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-scratch/FIELD-BARN-2; cd $D
python3 tiles.py
[ -n "$IMPORT" ] && bash imp2.sh
python3 scatter.py | tail -2
bash shoot3.sh $1
for c in wide mid low; do sips -s format jpeg -Z 1600 shots/$1_$c.png --out shots/$1_$c.jpg >/dev/null; done
