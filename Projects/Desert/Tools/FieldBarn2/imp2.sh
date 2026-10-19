#!/bin/bash
D=/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-scratch/FIELD-BARN-2
bash $D/live.sh "/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-SSR/Projects/Desert/Content/Scenes/FB2_Probe.desce" &
sleep 20; $D/ctl.sh --wait 300 state >/dev/null; sleep 3
bash $D/import.sh Environments/FieldBarn/Meshes/HayBaleRound/HayBaleRound.gltf
wait
