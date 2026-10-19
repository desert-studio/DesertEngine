#!/bin/bash
# live.sh <scene abs> — editor with control socket /tmp/fb2.sock (waits for any other GPU process first)
T=/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-SSR
for i in $(seq 27); do pgrep -x Editor >/dev/null || pgrep -x Runtime >/dev/null || break; sleep 10; done
export HOME=/private/tmp/claude-501/fieldbarn2/home-$(date +%s); mkdir -p $HOME
rm -f $T/Editor/Resources/Assets/Scenes/Autosave/.session.lock $T/Projects/Desert/Saved/Autosaves/.session.lock /tmp/fb2.sock
cd $T/Editor && /Users/daniilsavcenko/.claude/tools/run_capped.sh $T/build/Bin/Debug/Editor --project $T/Projects/Desert/Desert.deproj \
  --scene "$1" --control-socket /tmp/fb2.sock > /Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-scratch/FIELD-BARN-2/live.log 2>&1
