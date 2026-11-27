#!/usr/bin/env python3
"""limits — the ONLY place where the wrapper's numbers and paths live.

Every hook and tool imports this module; every document names a constant (`limits.MAX_AGENTS`), never its value.
The values are the owner's: change one only on the owner's word, with the date in the comment.
`python .claude/tools/limits.py` prints the table (the lead's and an agent's way to read a limit).
"""
import os

# ---- people -------------------------------------------------------------------------------------------------------
MAX_AGENTS = 4            # owner 10-30: live agents at once (Explore and board are not counted)
NIGHT_MAX_AGENTS = 3      # owner 10-22: the ceiling while night mode is on
NIGHT_MIN_AGENTS = 3      # owner 10-22: the floor while night mode is on
DEFAULT_MIN_AGENTS = 1    # owner 11-24: the floor when FLOOR_FILE is absent; the owner's pause writes 0 into it
RUNNING_MAX_AGE_H = 4     # a run with no stop older than this died with its session

# ---- one run ------------------------------------------------------------------------------------------------------
FIX_CALLS = (72, 80, 100)        # warn, soft limit, hard cap of a one-defect fix (owner 10-22, 10-23)
FEATURE_CALLS = (270, 300, 320)  # the same for engineer / architect / reviewer (owner 11-25)
FIX_READ_LINES = 150             # code is read by ranges of at most this many lines
FEATURE_READ_LINES = 400
FIX_BRIEF = (2, 900)             # «## Сделать»: numbered items, characters (owner 10-05)
FEATURE_BRIEF = (10, 5000)
REPORT_MAX_LINES = 20            # non-empty lines of an agent's final message (owner 09-24)
MAX_SLEEP = 270                  # seconds; the prompt cache lives 5 minutes
WAIT_SLICE = 240                 # seconds one `studio.py wait` call blocks
CONTINUE_MAX = 50                # owner 11-23: SendMessage to an agent past this many calls is refused
LEAD_OUTPUT_CAP = 6000           # characters one lead command may print (owner 11-25)

# ---- one feature --------------------------------------------------------------------------------------------------
MAX_PACKAGES = 4          # owner 11-26: a design is 1..4 PARALLEL engineer runs over disjoint files
FEATURES_IN_FLIGHT = 3    # features between `plan` and `landed`
FEATURE_MAX_AGE_H = 24    # owner 11-26: nothing is stretched over days; an older unlanded feature stops new plans
STALL_PASSES = 2          # owner 10-22: hand-ins without a new commit before «## Пересмотр» is demanded
FIXES_BEFORE_REVIEW = 2   # owner 11-26: BRIEF_FIX<N>.md above this needs the architect's DESIGN_REVIEW.md beside it
RUNS_BEFORE_BUILD = 2     # owner 11-25: hand-ins of one code before a build must have seen it
STALE_BRANCH_H = 24       # owner 10-29: no new step on a branch whose oldest commit not in dev is older
MAX_BEHIND = 40           # commits behind dev at which a day-old task branch may not be pushed

# ---- build (build_gate.py) ------------------------------------------------------------------------------------------
BUILD_MIN_MERGES = 2      # owner 11-05: merge commits since the last good build before the next one
BUILD_WHY_PER_DAY = 3     # builds a day that may be forced with BUILD_WHY="reason"
BUILD_RUNNING_S = 300     # a build log without .rc written within this many seconds = a build is running

# ---- board --------------------------------------------------------------------------------------------------------
BOARD_BATCH = 10          # owner 11-17: card updates go in batches; Stop is refused at this many queued
BOARD_MAX_AGE_H = 4       # ... or when the oldest queued update is older than this
RUNS_KEEP = 200           # finished runs kept in pipeline.json

# ---- documents (wrapper_lint.py) ------------------------------------------------------------------------------------
# A skill listed in an agent's `skills:` is loaded whole into every run of that agent, so its length is paid
# on every call of every run; a document that has no budget only grows (run 2 of the wrapper audit, 11-26).
DOC_BUDGET = {            # path: (lines, characters)
    "CLAUDE.md": (30, 3000),
    ".claude/LEAD_PROTOCOL.md": (160, 19000),
    ".claude/LESSONS.md": (120, 16000),
    ".claude/skills/desert-engine-architecture/SKILL.md": (40, 4000),
    ".claude/skills/desert-engine-look/SKILL.md": (30, 2500),
    ".claude/skills/desert-engine-feature/SKILL.md": (40, 4800),
    ".claude/skills/desert-engine-contract/SKILL.md": (90, 9600),
    ".claude/skills/desert-engine-dev/SKILL.md": (70, 7800),
    ".claude/skills/desert-engine-verify/SKILL.md": (70, 7800),
    ".claude/skills/desert-engine-gauntlet/SKILL.md": (55, 4800),
    "agent": (40, 4000),      # every .claude/agents/*.md
}
DUP_LINE_CHARS = 60       # a normalised line this long may appear in ONE document only
NOT_IN_DEV_LINES = 5      # branches not_in_dev prints at session start (the rest: --all)

# ---- machine ------------------------------------------------------------------------------------------------------
MIN_FREE_DISK_GB = 15     # free space on the disk of WORKTREE_ROOT below which nothing is launched or planned
BLANK_FRAME_BYTES = 30000

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LAND_TREE = os.environ.get("DESERT_LAND_TREE", REPO).replace("\\", "/")
WORKTREE_ROOT = os.environ.get("DESERT_WORKTREE_ROOT", "S:/wt" if os.name == "nt" else os.path.expanduser("~/wt"))
STUDIO_HOME = os.environ.get("DESERT_STUDIO_HOME", os.path.expanduser("~/.claude/studio")).replace("\\", "/")
FLOOR_FILE = os.path.expanduser("~/.claude/min_agents")
BUILD_LEDGER = os.environ.get("BUILD_GATE_LEDGER", "C:/aftmp/build_ledger.jsonl")
SCRATCH = os.environ.get("DESERT_SCRATCH", "F:/DesertEngine-scratch")
TRUNK = "origin/dev"

PIPELINE = STUDIO_HOME + "/pipeline.json"
BOARD_STATE = STUDIO_HOME + "/board.json"
EVENTS = STUDIO_HOME + "/events.csv"
LEDGER = STUDIO_HOME + "/ledger.csv"
GUARD_LOG = STUDIO_HOME + "/log.jsonl"
AGENT_STATE = STUDIO_HOME + "/agents"
BRIEFS = STUDIO_HOME + "/briefs"
ARCHIVE = STUDIO_HOME + "/archive"   # <date>-<CODE>/design.md + contract/: a landed feature's design, kept for the Wiki


def table():
    rows = []
    for name, value in sorted(globals().items()):
        if name.isupper() and isinstance(value, (int, tuple, str, dict)):
            rows.append(f"{name} = {value!r}")
    return "\n".join(rows)


if __name__ == "__main__":
    print(table())

# Owner 11-27 (a night = 6 % of the weekly limit, the lead ~61 % of it): the lead writes nothing until the owner asks
# for news; a turn the owner did not start ends without text (silence_guard.py). One line for a decision only the
# owner can make fits under this cap.
SILENT_TEXT_MAX = 20
