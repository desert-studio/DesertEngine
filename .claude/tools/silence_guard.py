"""Stop hook: a turn the owner did not start (an agent's notification, a background job, a hook) ends without text.

Owner 11-27: «ты вообще не будешь ничего писать, пока я тебя не попрошу» — every word the lead writes is re-read on
every later request. A turn started by the owner may answer; any other turn may write at most limits.SILENT_TEXT_MAX
characters (one line for a decision only the owner can make). Never blocks: the text is already shown, and a block
costs one more request and makes the harness demand visible output — a break is only recorded in the studio log.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import limits

NOT_OWNER = ("<task-notification", "Stop hook feedback", "<system-reminder>", "[SYSTEM NOTIFICATION", "<local-command",
             "<command-name>", "continue (auto after /compact)")


def text_of(content):
    if isinstance(content, str):
        return content
    return "".join(c.get("text", "") for c in content or [] if isinstance(c, dict) and c.get("type") == "text")


def is_tool_result(content):
    return isinstance(content, list) and any(isinstance(c, dict) and c.get("type") == "tool_result" for c in content)


def written_this_turn(path):
    """(owner started the turn, characters of text the lead wrote since the turn began, the turn's first row id)."""
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            try:
                rows.append(json.loads(line))
            except ValueError:
                continue
    chars = 0
    for row in reversed(rows):
        msg = row.get("message") or {}
        if row.get("type") == "assistant":
            chars += len(text_of(msg.get("content")).strip())
        elif row.get("type") == "user" and not is_tool_result(msg.get("content")):
            said = text_of(msg.get("content")).lstrip()
            if not said:
                continue
            return not said.startswith(NOT_OWNER), chars, row.get("uuid", "")
    return True, chars, ""


def remind(data, owner, turn):
    """PostToolUse: once per turn the owner did not start, say before the final text that it is at most two words."""
    state = limits.STUDIO_HOME + "/silence-remind.state"
    try:
        with open(state, encoding="utf-8") as f:
            if f.read() == turn:
                return
    except OSError:
        pass
    with open(state, "w", encoding="utf-8") as f:
        f.write(turn)
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext":
          f"[silence_guard] этот ход начал не владелец: в конце хода не больше limits.SILENT_TEXT_MAX знаков "
          f"(два слова, например «Жду.»), без итогов и статусов — владелец 11-27."}}))


def main():
    data = json.load(sys.stdin)
    if data.get("stop_hook_active") or data.get("agent_id") or not data.get("transcript_path"):
        return
    try:
        owner, chars, turn = written_this_turn(data["transcript_path"])
    except OSError:
        return
    if "--remind" in sys.argv:
        if not owner:
            remind(data, owner, turn)
        return
    if not owner and chars > limits.SILENT_TEXT_MAX:
        with open(limits.STUDIO_HOME + "/silence-breaks.log", "a", encoding="utf-8") as log:
            log.write(f"{data.get('session_id', '?')} {chars}\n")


if __name__ == "__main__":
    main()
