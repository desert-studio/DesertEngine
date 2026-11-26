#!/usr/bin/env python3
"""Stop hook: when the lead's context passes THRESHOLD, type `/compact` into this session's Terminal tab.

Owner 11-24 (2): 150k was too late; THRESHOLD 130k (a fresh compact already starts at ~75k) and the trigger goes out
together with lead_watch's «/COMPACT» reminder: lead_watch touches auto_compact.now when it shows the reminder, and
this hook then types /compact at the end of that turn regardless of THRESHOLD and COOLDOWN.
`python3 auto_compact.py --now` sets the same flag by hand.
Owner 11-24 (3): «clear тоже делай ты… мониторь сам что и когда запустить». `--clear` sets auto_compact.clear: at the end of
the turn the hook types /clear instead (the lead decides: task boundary, slots full, handover written). After either
command the hook types a continuation prompt once the command has finished, so the lead resumes without the owner.
Owner 11-24 (4): leftover text in the prompt box went out as /compact's arguments; every typing first empties the box
(Ctrl+E, Ctrl+U, Backspace per line, CLEAR_LINES lines).

Owner 11-24: autoCompactWindow 100k thrashed; window back to 200k, and /compact must come without the owner.
Claude cannot run a slash command itself, so this hook finds the Terminal.app tab whose tty is this claude
process's tty and types the command there (synthetic input is allowed, owner 09-30). It fires at most once per
COOLDOWN seconds and only after the turn has ended (Stop), a few seconds later so the prompt is idle.
Log: limits.STUDIO_HOME/auto_compact.log.  Off switch: touch <that folder>/auto_compact.off
Windows: console_type.py writes into the input buffer of the claude.exe console (no focus needed, unlike SendKeys).
Only /compact there: typing /clear and the continue prompt waits for the owner's word on that machine.
/clear only when the lead asked for it with --clear, after writing the handover (LEAD_PROTOCOL «Контекст лида»).
"""
import json, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import limits

THRESHOLD = 130_000
COOLDOWN = 900
DELAY = 6
CLEAR_LINES = 8
HOME = limits.STUDIO_HOME
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
    if sys.platform == "win32":
        import console_type
        pid = console_type.ancestor(os.getpid(), "claude.exe")
        return str(pid) if pid else None
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


def type_into(tty, text, enters):
    """Select the Terminal tab on @p tty, type @p text and press Enter @p enters times."""
    if sys.platform == "win32":
        import console_type
        try:
            console_type.type_into(int(tty), text, enters, CLEAR_LINES)
            log(f"{text[:40]!r}: typed into pid {tty}")
        except OSError as e:
            log(f"{text[:40]!r}: {e}")
        return
    keys = "\n".join(['        delay 0.7\n        tell application "System Events" to key code 36'] * enters)
    # Empty the prompt box first: whatever is left there would be sent together with @p text.
    wipe = "\n".join(['        tell application "System Events"\n'
                      '          key code 14 using control down\n'
                      '          key code 32 using control down\n'
                      '          key code 51\n'
                      '        end tell'] * CLEAR_LINES)
    script = f'''
tell application "Terminal"
  repeat with w in windows
    repeat with t in tabs of w
      if tty of t is "{tty}" then
        set selected of t to true
        set index of w to 1
        activate
        delay 0.5
{wipe}
        delay 0.2
        -- Owner 11-26: `keystroke` types through the active layout, so with Russian on "/compact" arrived as
        -- "/ффффффф" and every garbled prompt started another lead turn. Paste by key code instead.
        set the clipboard to "{text}"
        delay 0.1
        tell application "System Events" to key code 9 using command down
{keys}
        return "typed"
      end if
    end repeat
  end repeat
end tell
return "tab not found"
'''
    out = subprocess.run(["osascript", "-e", script], capture_output=True, text=True)
    log(f"{text[:40]!r}: {(out.stdout or out.stderr).strip()}")


def compactions(path):
    try:
        with open(path, "rb") as f:
            return f.read().count(b'"compact_boundary"')
    except OSError:
        return 0


def worker(tty, cmd, transcript):
    """Type @p cmd, wait until it has run, then type the continue prompt (owner 11-24: no idle after it)."""
    before = compactions(transcript)
    time.sleep(DELAY)
    # Two Enters: the first may only accept the slash-command completion; on an empty prompt the second does nothing.
    type_into(tty, cmd, 2)
    if sys.platform == "win32":
        return
    if cmd == "/compact":
        deadline = time.time() + 900
        while compactions(transcript) <= before and time.time() < deadline:
            time.sleep(3)
        if compactions(transcript) <= before:
            log("no compact_boundary within 15 min: continue prompt not typed")
            return
    time.sleep(4)
    type_into(tty, RESUME[cmd], 2)


def main():
    if "--type" in sys.argv:
        k = sys.argv.index("--type")
        worker(*sys.argv[k + 1:k + 4])
        return
    if "--now" in sys.argv or "--clear" in sys.argv:
        open(CLEAR if "--clear" in sys.argv else NOW, "w").close()
        return
    if os.path.exists(os.path.join(HOME, "auto_compact.off")):
        return
    if sys.platform not in ("darwin", "win32"):
        return
    data = json.load(sys.stdin)
    if data.get("stop_hook_active"):
        return
    ctx = context_tokens(data.get("transcript_path", ""))
    clear = os.path.exists(CLEAR) and sys.platform == "darwin"
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
    # Detached worker: the hook must return at once, the typing happens after the turn has ended.
    subprocess.Popen([sys.executable, os.path.abspath(__file__), "--type", tty, cmd, data.get("transcript_path", "")],
                     start_new_session=True, creationflags=0x208 if sys.platform == "win32" else 0,  # detached, own group
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    why = "by the lead's flag" if forced else f">= {THRESHOLD//1000}k"
    log(f"context {ctx//1000}k {why}: {cmd} scheduled into {tty}")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # a hook must never block the session
        log(f"error: {e!r}")
