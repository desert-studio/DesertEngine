#!/usr/bin/env python3
"""Stop hook: when the lead's context passes THRESHOLD, type `/compact` into this session's Terminal tab.

Owner 11-24 (2): 150k was too late; THRESHOLD 130k (a fresh compact already starts at ~75k) and the trigger goes out
together with lead_watch's «/COMPACT» reminder: lead_watch touches auto_compact.now when it shows the reminder, and
this hook then types /compact at the end of that turn regardless of THRESHOLD and COOLDOWN.
`python3 auto_compact.py --now` sets the same flag by hand.

Owner 11-24: autoCompactWindow 100k thrashed; window back to 200k, and /compact must come without the owner.
Claude cannot run a slash command itself, so this hook finds the Terminal.app tab whose tty is this claude
process's tty and types the command there (synthetic input is allowed, owner 09-30). It fires at most once per
COOLDOWN seconds and only after the turn has ended (Stop), a few seconds later so the prompt is idle.
Log: ~/.claude/tools/auto_compact.log.  Off switch: touch ~/.claude/tools/auto_compact.off
macOS only (Terminal.app + osascript); on other platforms it logs once per call and does nothing — the Windows
machine adds its own keystroke path (console window of the claude process, e.g. WScript.Shell AppActivate+SendKeys).
/clear is never typed by this hook: it needs a written handover first (LEAD_PROTOCOL «КОНТЕКСТ ТИМЛИДА»).
"""
import json, os, subprocess, sys, time

THRESHOLD = 130_000
COOLDOWN = 900
DELAY = 6
HOME = os.path.expanduser("~/.claude/tools")
os.makedirs(HOME, exist_ok=True)
STAMP = os.path.join(HOME, "auto_compact.stamp")
LOG = os.path.join(HOME, "auto_compact.log")
NOW = os.path.join(HOME, "auto_compact.now")


def log(msg):
    with open(LOG, "a") as f:
        f.write(time.strftime("%m-%d %H:%M:%S ") + msg + "\n")


def context_tokens(path):
    # The last assistant message's usage = what the next request re-reads.
    with open(path, "rb") as f:
        f.seek(0, 2)
        f.seek(max(0, f.tell() - 2_000_000))
        lines = f.read().decode("utf-8", "replace").splitlines()
    for line in reversed(lines):
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        if rec.get("isSidechain"):
            continue
        u = (rec.get("message") or {}).get("usage")
        if rec.get("type") == "assistant" and u:
            return u.get("input_tokens", 0) + u.get("cache_read_input_tokens", 0) + u.get("cache_creation_input_tokens", 0)
    return 0


def claude_tty():
    pid = os.getppid()
    for _ in range(6):
        out = subprocess.run(["ps", "-o", "ppid=,tty=,comm=", "-p", str(pid)], capture_output=True, text=True).stdout.split(None, 2)
        if len(out) < 3:
            return None
        ppid, tty, comm = out
        if os.path.basename(comm.strip()) == "claude" and tty not in ("??", "-"):
            return "/dev/" + tty
        pid = int(ppid)
    return None


def main():
    if "--now" in sys.argv:
        open(NOW, "w").close()
        return
    if os.path.exists(os.path.join(HOME, "auto_compact.off")):
        return
    if sys.platform != "darwin":
        return
    data = json.load(sys.stdin)
    if data.get("stop_hook_active"):
        return
    ctx = context_tokens(data.get("transcript_path", ""))
    forced = os.path.exists(NOW)
    if not forced:
        if ctx < THRESHOLD:
            return
        try:
            if time.time() - os.path.getmtime(STAMP) < COOLDOWN:
                return
        except OSError:
            pass
    tty = claude_tty()
    if not tty:
        log(f"context {ctx//1000}k but no claude tty found")
        return
    open(STAMP, "w").close()
    if forced:
        os.remove(NOW)
    script = f'''
delay {DELAY}
tell application "Terminal"
  repeat with w in windows
    repeat with t in tabs of w
      if tty of t is "{tty}" then
        set selected of t to true
        set index of w to 1
        activate
        delay 0.5
        tell application "System Events" to keystroke "/compact"
        delay 0.3
        tell application "System Events" to key code 36
        return "sent"
      end if
    end repeat
  end repeat
end tell
return "tab not found"
'''
    subprocess.Popen(["/bin/sh", "-c", f"osascript -e '{script.replace(chr(39), chr(39) + chr(92) + chr(39) + chr(39))}' >> '{LOG}' 2>&1"],
                     start_new_session=True, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    why = "with the /COMPACT reminder" if forced else f">= {THRESHOLD//1000}k"
    log(f"context {ctx//1000}k {why}: /compact scheduled into {tty}")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # a hook must never block the session
        log(f"error: {e!r}")
