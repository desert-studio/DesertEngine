#!/usr/bin/env python3
"""Stop hook: when the lead's context passes THRESHOLD, type `/compact` into this session's Terminal tab.

Owner 11-24 (2): 150k was too late; THRESHOLD 130k (a fresh compact already starts at ~75k) and the trigger goes out
together with lead_watch's «/COMPACT» reminder: lead_watch touches auto_compact.now when it shows the reminder, and
this hook then types /compact at the end of that turn regardless of THRESHOLD and COOLDOWN.
`python3 auto_compact.py --now` sets the same flag by hand.
Owner 11-24 (3): «clear тоже делай ты… мониторь сам что и когда запустить». `--clear` sets auto_compact.clear: at the end of
the turn the hook types /clear instead (the lead decides: task boundary, slots full, handover written). After either
command the hook types a continuation prompt, queued while the command runs, so the lead resumes without the owner.

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
CLEAR = os.path.join(HOME, "auto_compact.clear")
RESUME = {"/compact": "continue (auto after /compact): pick up the work where it stopped",
          "/clear": "continue (auto after /clear): read the newest HANDOVER in memory, then fill the agent slots"}


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
    if "--now" in sys.argv or "--clear" in sys.argv:
        open(CLEAR if "--clear" in sys.argv else NOW, "w").close()
        return
    if os.path.exists(os.path.join(HOME, "auto_compact.off")):
        return
    if sys.platform != "darwin":
        return
    data = json.load(sys.stdin)
    if data.get("stop_hook_active"):
        return
    ctx = context_tokens(data.get("transcript_path", ""))
    clear = os.path.exists(CLEAR)
    forced = clear or os.path.exists(NOW)
    cmd = "/clear" if clear else "/compact"
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
    for flag in (NOW, CLEAR):
        if os.path.exists(flag):
            os.remove(flag)
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
        tell application "System Events" to keystroke "{cmd}"
        delay 0.3
        tell application "System Events" to key code 36
        delay 2
        tell application "System Events" to keystroke "{RESUME[cmd]}"
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
    why = "by the lead's flag" if forced else f">= {THRESHOLD//1000}k"
    log(f"context {ctx//1000}k {why}: {cmd} scheduled into {tty}")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # a hook must never block the session
        log(f"error: {e!r}")
