#!/usr/bin/env python3
"""net_scenario.py <TREE> <SCENARIO.scn> — play a NETWORK scenario: one controller, a server role and client roles.

UE Gauntlet's shape (UnrealTestNode + UnrealSession): the controller starts a dedicated-server role and N client roles,
each with its own command line and its own simulated link (PIE PktLag / PktLoss), drives the run by waiting on events
in a role's log, collects every role's log and frames into ONE run folder, and ends every role when the test ends.
Before NET-SCENARIO-PLAY (11-25) each network card wrote its own shell runner (NET-PREDICT run_all.sh, NET-REMOTE
run_net5b.sh): nothing replayable, and accept.sh saw "scenario: NONE". play_scenario.py hands a .scn with a `Роль`
line to this file; the output layout is play_scenario's, so accept.sh reads it the same way.

Line format (one step per line in order, `#` comments; a step's REGEX is the rest of the line, taken verbatim):
  Проект|Project <x.deproj>                   optional, before any step (default Projects/Desert/Desert.deproj)
  Мир|World <scene.desce>                     the world every role plays, relative to the .deproj folder; cooked with
                                              the tree's WorldCook to <scene>.dwworld before the first role starts
  Роль|Role <name> server|editor|bot [account=N] [lag=MS] [loss=PCT] [jitter=MS] <the process's own args…>
                                              starts the role at this point of the run. The controller adds what it
                                              owns: project, world, port, auth (an issuer key pair per run; a token per
                                              client account), the cooked world, the link flags, and for an editor the
                                              frame sequence (`--shot-sequence`). lag = ms each way, loss = % dropped.
                                              editor = the Editor as a headless --play client through run_capped (the
                                              machine's GPU slot); bot = DesertNetProbe --bot; account defaults to the
                                              client's order (1, 2, …).
                                              A server names its `--role all|coordinator|store|sim|gateway` (NET-ARCH
                                              §2M): a split role gets the run's dev mesh CA (`--mesh-ca`, made once by
                                              `DesertServer --mesh-dev-ca`); coordinator, store and sim get their own
                                              `--mesh-port`; store, sim and gateway get `--coordinator` (the coordinator
                                              role starts first); all, sim and gateway get the world. The client port
                                              (`--port --audience --issuer-key`, the link flags) is the role clients
                                              connect to: all or gateway, one per run.
  Роль|Role <name> probe <args…>              a HOSTILE MESH PEER: DesertNetProbe --mesh-peer with the run's mesh CA
                                              and `--store` (the store role's mesh address; the store role starts
                                              first); its console (stdin) takes «Консоль» lines as a server's does.
  Ждать|Wait <role> <seconds> [<N>x] <REGEX>  the role writes a line matching REGEX (N of them) after the previous
                                              event; refused after <seconds> or when the role has exited. An event is
                                              a Ждать match or a Консоль line: lines written before it never satisfy a
                                              later Ждать (UE Gauntlet: wait for the NEXT log event).
  Консоль|Console <role> <line>               write the line to the role's console (stdin); {var} placeholders
  Сигнал|Signal <role> STOP|CONT|TERM         send the signal to the server role's process group (a mesh peer that
                                              stops answering, resumes, or dies); an event like a Консоль line
  Запомнить|Read <var> <role> <REGEX>         the role's LAST line matching REGEX so far: its numeric groups become
                                              {var1}, {var2}, … ({var1-20000} adds, for a console line)
  Сверить|Compare <var> <var> <cm>            every component of the two equal within <cm>
  Нет|Never <role> <REGEX>                    the role's whole log so far has no line matching REGEX
  Тогда|Then <role> <frame> <what it shows…>  the next frame the editor role draws, as <frame>.png (index.txt)
Output: <scratch>/<CODE>/frames/scn-<time>/ (scn-latest points at it): <role>.log per role, the frames, index.txt,
result.txt, trace.txt (each step's time and what it read), the scenario, cook.log. Every role is ended when the steps end or one fails.
"""
import os
import re
import shlex
import signal
import socket
import subprocess
import sys
import time

WIN = os.name == "nt"
EXE = ".exe" if WIN else ""
AUDIENCE = "7"  # the world id the server answers to and every token is minted for (one world per run)
KEYWORDS = {
    "проект": "project", "project": "project", "мир": "world", "world": "world", "роль": "role", "role": "role",
    "ждать": "wait", "wait": "wait", "консоль": "console", "console": "console", "запомнить": "read",
    "read": "read", "сверить": "compare", "compare": "compare", "нет": "never", "never": "never",
    "тогда": "then", "then": "then", "сигнал": "signal", "signal": "signal",
}
SERVER_ROLES = ("all", "coordinator", "store", "sim", "gateway")
SIGNALS = {"STOP": "SIGSTOP", "CONT": "SIGCONT", "TERM": "SIGTERM"}
PLACEHOLDER = re.compile(r"\{([A-Za-z_]+)(\d+)([+-]\d+(?:\.\d+)?)?\}")
SHOT = re.compile(r"\[Shot\] wrote -> (\S+\.png)")
# The editor counts a capture frame when its tick records it (ShotDirector::FrameRecorded) and writes frame_N.png
# from that frame's own copy later, at its announcement: the frame a Тогда judges is named by this line.
RECORDED = re.compile(r"\[Shot\] recorded frame_(\d+) \(game frame (\d+)\)")
SEQNUM = re.compile(r"frame_(\d+)\.png$")


class Fail(Exception):
    pass


def dev_bindir(tree):
    """The tree's dev configuration (scripts/Dev/dev_config.txt, the row _common.sh builds) -> its build/Bin folder."""
    host = "Windows" if WIN else "Darwin"
    path = os.path.join(tree, "scripts", "Dev", "dev_config.txt")
    for line in open(path, encoding="utf-8"):
        row = line.split()
        if row[:1] == [host] and len(row) > 1:
            return os.path.join(tree, "build", "Bin", row[1])
    sys.exit(f"NETSCN: {path} has no row for {host}")


class Role:
    def __init__(self, name, kind, proc, logpath, seqdir=None):
        self.name, self.kind, self.proc, self.logpath, self.seqdir = name, kind, proc, logpath, seqdir
        self.lines, self.cursor, self.partial, self.drawn = [], 0, "", []
        self.hold = False  # a Тогда is choosing its frame: nothing announced is deleted meanwhile
        self.claimed = {}  # sequence frame number -> the «Тогда» statement that took it
        self.reader = open(logpath, encoding="utf-8", errors="replace")

    def pump(self):
        """Read what the role wrote since the last pump (whole lines only). A Тогда takes a frame recorded AFTER the
        line it follows, so every sequence frame already announced by an earlier pump is deleted first (a dense
        sequence stays a few MB) - except while a Тогда holds them (its frame may have been announced already)."""
        if not self.hold:
            for path in self.drawn:
                if os.path.exists(path):
                    os.remove(path)
            self.drawn = []
        chunk = self.reader.read()
        if chunk:
            text = self.partial + chunk
            parts = text.split("\n")
            self.partial = parts.pop()
            self.lines.extend(parts)
            if self.seqdir:
                self.drawn += [m.group(1) for m in map(SHOT.search, parts)
                               if m and os.path.dirname(m.group(1)) == self.seqdir]


class Run:
    def __init__(self, tree, scn):
        self.tree, self.scn = tree, scn
        self.code = os.path.splitext(os.path.basename(scn))[0]
        self.bindir = dev_bindir(tree)
        self.proj = os.path.join(tree, "Projects", "Desert", "Desert.deproj")
        self.world = self.cooked = None
        self.port = None  # the client port: the one `--role all` or gateway clients connect to
        self.coordinator = None  # "127.0.0.1:<mesh port>" once the coordinator role has started
        self.meshca = None
        self.store = None  # "127.0.0.1:<mesh port>" once the store role has started (a probe role's --store)
        self.roles, self.vars, self.accounts = {}, {}, 0
        scratch = os.environ.get("DESERT_SCRATCH") or os.path.join(os.path.dirname(tree), "DesertEngine-scratch")
        framesdir = os.path.join(scratch, self.code, "frames")
        self.out = os.path.join(framesdir, "scn-" + time.strftime("%m%d-%H%M%S"))
        os.makedirs(self.out)
        latest = os.path.join(framesdir, "scn-latest")
        try:
            if os.path.islink(latest):
                os.remove(latest)
            os.symlink(self.out, latest)
        except OSError:  # Windows without symlink rights: a pointer file instead (play_scenario's convention)
            with open(latest + ".txt", "w", encoding="utf-8") as f:
                f.write(self.out + "\n")

    def tool(self, name):
        path = os.path.join(self.bindir, name + EXE)
        if not os.path.isfile(path):
            raise Fail(f"{path} is not built (the tree's dev configuration)")
        return path

    def poll(self):
        for r in self.roles.values():
            r.pump()

    def event(self, role=None, index=None):
        """A Консоль line moves every role's cursor to now (what it causes comes after it); a Ждать match moves only
        its own role's cursor past the matched line. Roles' logs reach the disk with their own delays (a server line
        may be read after the client's reaction to it), so one role's match never says where another role is: run 4
        skipped A's 'correction 1' (170 ms after the server's 'teleported' line) when that match moved A's cursor."""
        if role is not None:
            role.cursor = index + 1
            return
        self.poll()
        for r in self.roles.values():
            r.cursor = len(r.lines)

    def role(self, name):
        if name not in self.roles:
            raise Fail(f"no role {name} started yet")
        return self.roles[name]

    # --- steps -------------------------------------------------------------------------------------------------
    def set_project(self, rest):
        cand = os.path.abspath(os.path.join(self.tree, rest.strip()))
        if not os.path.isfile(cand):
            raise Fail(f"project {rest.strip()} is not in {self.tree}")
        self.proj = cand

    def set_world(self, rest):
        self.world = rest.strip()
        scene = os.path.join(os.path.dirname(self.proj), self.world)
        if not os.path.isfile(scene):
            raise Fail(f"world {scene} does not exist")
        # Where every role reads it: Core::WorldCells::CookedWorldDirectory, <scene>.dwworld beside the scene (the
        # server derives it from --world; a derived product, git-ignored). Cooked per run (~3 s), so it is never stale.
        self.cooked = os.path.splitext(scene)[0] + ".dwworld"
        with open(os.path.join(self.out, "cook.log"), "w", encoding="utf-8") as log:
            rc = subprocess.call([self.tool("WorldCook"), scene, "--out", self.cooked, "--project", self.proj,
                                  "--project-registry"], cwd=self.tree, stdout=log, stderr=subprocess.STDOUT)
        if rc != 0:
            raise Fail(f"WorldCook {self.world} exited {rc} (cook.log)")

    def auth(self):
        """One issuer key pair per run (NET-AUTH-SPLIT): the server trusts its public half, tokens are minted per account."""
        keys = os.path.join(self.out, "auth")
        if os.path.isdir(keys):
            return keys
        os.makedirs(keys)
        out = subprocess.run([self.tool("DesertNetProbe"), "--gen-issuer-keys", "--issuer", "1", "--key-id", "1",
                              "--out-private", os.path.join(keys, "issuer.private"),
                              "--out-public", os.path.join(keys, "issuer.public")], capture_output=True, text=True)
        if out.returncode != 0:
            raise Fail(f"issuer keys: {out.stdout.strip()[-200:]} {out.stderr.strip()[-200:]}")
        return keys

    def token(self, account):
        out = subprocess.run([self.tool("DesertNetProbe"), "--mint-token", "--issuer-key",
                              os.path.join(self.auth(), "issuer.private"), "--claims", f"?account={account}",
                              "--audience", AUDIENCE, "--expires-in", "21600"], capture_output=True, text=True)
        lines = out.stdout.strip().splitlines()
        if out.returncode != 0 or not lines:
            raise Fail(f"token for account {account}: {out.stderr.strip()[-200:]}")
        return lines[-1].strip()

    def mesh_ca(self):
        """The run's dev mesh CA (NET-MESH-7 dev path): one root and a certificate per role process, made once."""
        if self.meshca:
            return self.meshca
        ca = os.path.join(self.out, "mesh-ca")
        out = subprocess.run([self.tool("DesertServer"), "--mesh-dev-ca", ca], capture_output=True, text=True)
        if out.returncode != 0:
            raise Fail(f"mesh dev CA: {out.stdout.strip()[-200:]} {out.stderr.strip()[-200:]}")
        self.meshca = ca
        return ca

    @staticmethod
    def free_port():
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:  # a free UDP port for this run
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]

    def server_argv(self, name, words):
        """The server's command line by its --role: what the controller owns for that role is added here."""
        given = [words[i + 1] for i, w in enumerate(words[:-1]) if w == "--role"]
        if len(given) != 1 or given[0] not in SERVER_ROLES:
            raise Fail(f"server role {name} names exactly one --role of {', '.join(SERVER_ROLES)}")
        mesh = given[0]
        for owned in ("--port", "--audience", "--issuer-key", "--mesh-port", "--mesh-ca", "--coordinator",
                      "--world", "--project"):
            if owned in words:
                raise Fail(f"server role {name}: {owned} is the controller's")
        argv = [self.tool("DesertServer"), "--project", self.proj, *words]
        if mesh in ("all", "sim", "gateway"):
            argv += ["--world", os.path.join(os.path.dirname(self.proj), self.world)]
        if mesh != "all":
            argv += ["--mesh-ca", self.mesh_ca()]
        if mesh in ("coordinator", "store", "sim"):
            port = self.free_port()
            argv += ["--mesh-port", str(port)]
            if mesh == "coordinator":
                if self.coordinator:
                    raise Fail("one coordinator role per run")
                self.coordinator = f"127.0.0.1:{port}"
            if mesh == "store":
                self.store = f"127.0.0.1:{port}"
        if mesh in ("store", "sim", "gateway"):
            if not self.coordinator:
                raise Fail(f"server role {name} (--role {mesh}) before the coordinator role")
            argv += ["--coordinator", self.coordinator]
        if mesh in ("all", "gateway"):
            if self.port:
                raise Fail("one client port per run (one --role all or gateway)")
            self.port = self.free_port()
            argv += ["--port", str(self.port), "--audience", AUDIENCE,
                     "--issuer-key", os.path.join(self.auth(), "issuer.public")]
        return argv, mesh in ("all", "gateway")

    def start_role(self, rest):
        words = shlex.split(rest)
        if len(words) < 2 or words[1] not in ("server", "editor", "bot", "probe"):
            raise Fail("«Роль» takes <name> server|editor|bot|probe [account=N] [lag=MS] [loss=PCT] [jitter=MS] "
                       "<args…>")
        name, kind, words = words[0], words[1], words[2:]
        if name in self.roles:
            raise Fail(f"role {name} is already started")
        if not self.cooked:
            raise Fail("«Мир» must come before the first «Роль»")
        opts = {}
        while words and re.fullmatch(r"(account|lag|loss|jitter)=[0-9.]+", words[0]):
            key, _, value = words.pop(0).partition("=")
            opts[key] = value
        logpath = os.path.join(self.out, name + ".log")
        seqdir = None
        env = dict(os.environ, DESERT_ROOT=self.tree)
        cwd = self.tree
        if kind == "server":
            argv, serves_clients = self.server_argv(name, words)
            link = [("--lag-ms", "lag"), ("--jitter-ms", "jitter"), ("--loss", "loss")]
            if not serves_clients and any(key in opts for _, key in link):
                raise Fail(f"lag/loss/jitter are the client port's link: role {name} has none")
        elif kind == "probe":
            if not self.store:
                raise Fail(f"probe role {name} before the store role (--role store)")
            if opts:
                raise Fail(f"probe role {name}: a mesh peer has no account or client link")
            argv = [self.tool("DesertNetProbe"), "--mesh-peer", "--mesh-ca", self.mesh_ca(), "--store", self.store,
                    *words]
            link = []
        else:
            if not self.port:
                raise Fail(f"client role {name} before the server role that serves clients (all or gateway)")
            self.accounts += 1
            token = self.token(opts.get("account", str(self.accounts)))
            if kind == "editor":
                seqdir = os.path.join(self.out, name + "_seq")
                os.makedirs(seqdir)
                argv = [sys.executable, os.path.join(self.tree, ".claude", "tools", "run_capped.py"),
                        self.tool("Editor"), "--project", self.proj, "--scene", self.world, *words,
                        "--shot-sequence", seqdir, "--shot", os.path.join(self.out, name + "_last.png"),
                        "--connect", f"127.0.0.1:{self.port}", "--net-cooked", self.cooked, "--net-token", token]
                link = [("--net-lag-ms", "lag"), ("--net-loss", "loss")]
                cwd = os.path.join(self.tree, "Editor")
            else:
                argv = [self.tool("DesertNetProbe"), "--bot", "--connect", f"127.0.0.1:{self.port}",
                        "--cooked", self.cooked, "--token", token, "--name", name, *words]
                link = [("--lag-ms", "lag"), ("--loss", "loss")]
        if "jitter" in opts and kind != "server":
            raise Fail(f"jitter is the server's send link only (role {name})")
        for flag, key in link:
            if key in opts:
                argv += [flag, opts[key]]
        log = open(logpath, "w", encoding="utf-8", errors="replace")
        proc = subprocess.Popen(argv, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT,
                                stdin=subprocess.PIPE if kind in ("server", "probe") else subprocess.DEVNULL,
                                text=True, start_new_session=not WIN)
        log.close()
        self.roles[name] = Role(name, kind, proc, logpath, seqdir)

    def wait(self, rest):
        parts = rest.split(None, 2)
        if len(parts) < 3 or not parts[1].isdigit():
            raise Fail("«Ждать» takes <role> <seconds> [fresh] [<N>x] <regex>")
        r, deadline = self.role(parts[0]), time.time() + int(parts[1])
        count, pattern = 1, parts[2]
        # `fresh`: match only lines read from now on. A Ждать on another role pumped this one and deleted the frames
        # after its old lines, so a Тогда following a stale match has no frame; a fresh match still has its frame.
        m = re.match(r"fresh\s+(.*)", pattern)
        if m:
            pattern = m.group(1)
            self.poll()
            r.cursor = len(r.lines)
        m = re.match(r"(\d+)x\s+(.*)", pattern)
        if m:
            count, pattern = int(m.group(1)), m.group(2)
        rx = re.compile(pattern)
        found = 0
        while True:
            self.poll()
            while r.cursor < len(r.lines):
                i = r.cursor
                r.cursor += 1
                if rx.search(r.lines[i]):
                    found += 1
                    if found == count:
                        self.event(r, i)
                        return
            if r.proc.poll() is not None:
                raise Fail(f"role {r.name} exited (rc {r.proc.returncode}) after {found}/{count} of «{pattern}»")
            if time.time() > deadline:
                raise Fail(f"{found}/{count} of «{pattern}» in {r.name}'s log within {parts[1]} s")
            time.sleep(0.05)

    def console(self, rest):
        name, _, line = rest.strip().partition(" ")
        r = self.role(name)
        if r.kind not in ("server", "probe"):
            raise Fail(f"«Консоль» writes to a server's or a probe's console, {name} is a {r.kind}")

        def value(m):
            if m.group(1) not in self.vars or int(m.group(2)) > len(self.vars[m.group(1)]):
                raise Fail(f"{m.group(0)}: nothing read into {m.group(1)}")
            v = self.vars[m.group(1)][int(m.group(2)) - 1] + float(m.group(3) or 0)
            return str(int(v)) if v == int(v) else f"{v:g}"  # a net id or a step >= 1e6 stays an integer
        text = PLACEHOLDER.sub(value, line.strip())
        self.event()
        r.proc.stdin.write(text + "\n")
        r.proc.stdin.flush()
        return text

    def signal(self, rest):
        """A mesh peer that stops answering (STOP), resumes with its backlog (CONT) or dies (TERM): the whole process
        group, as the role was started in its own session. An event: what it causes comes after it."""
        words = rest.split()
        if len(words) != 2 or words[1].upper() not in SIGNALS:
            raise Fail(f"«Сигнал» takes <role> {'|'.join(SIGNALS)}")
        r = self.role(words[0])
        if r.kind != "server":
            raise Fail(f"«Сигнал» is for a server role, {r.name} is a {r.kind}")
        if WIN:
            raise Fail("«Сигнал» needs POSIX process groups (SIGSTOP/SIGCONT): not on Windows")
        if r.proc.poll() is not None:
            raise Fail(f"role {r.name} already exited (rc {r.proc.returncode})")
        self.event()
        os.killpg(r.proc.pid, getattr(signal, SIGNALS[words[1].upper()]))
        return f"{words[1].upper()} -> {r.name}"

    def read(self, rest):
        parts = rest.split(None, 2)
        if len(parts) < 3:
            raise Fail("«Запомнить» takes <var> <role> <regex>")
        r, rx = self.role(parts[1]), re.compile(parts[2])
        self.poll()
        for line in reversed(r.lines):
            m = rx.search(line)
            if m:
                self.vars[parts[0]] = [float(g) for g in m.groups()]
                return f"{parts[0]} = {self.vars[parts[0]]}"
        raise Fail(f"no line of {r.name} matches «{parts[2]}»")

    def compare(self, rest):
        a, b, tol = rest.split()
        va, vb = self.vars.get(a), self.vars.get(b)
        if va is None or vb is None or len(va) != len(vb):
            raise Fail(f"«Сверить» {a} {b}: read both first, same number of values")
        worst = max(abs(x - y) for x, y in zip(va, vb))
        if worst > float(tol):
            raise Fail(f"{a} {va} vs {b} {vb}: {worst:g} cm apart > {tol}")
        return f"{a} {va} = {b} {vb} within {worst:g} cm"

    def never(self, rest):
        name, _, pattern = rest.strip().partition(" ")
        r, rx = self.role(name), re.compile(pattern)
        self.poll()
        hit = next((line for line in r.lines if rx.search(line)), None)
        if hit:
            raise Fail(f"{name}'s log has «{pattern}»: {hit.strip()[:200]}")

    def then(self, rest):
        parts = rest.split(None, 2)
        if len(parts) < 2:
            raise Fail("«Тогда» takes <role> <frame> <what it shows>")
        r = self.role(parts[0])
        if not r.seqdir:
            raise Fail(f"«Тогда» {r.name}: a {r.kind} draws no frames")
        # THE FRAME THE TICK RECORDED (NET-PAWN-TELEPORT-FRAME D1): the first `[Shot] recorded frame_N` after the line
        # the last Ждать matched (r.cursor) - frame N is the frame whose tick ran after that line - then the
        # `wrote ... frame_N.png` written from that frame's own copy. Frame N+1 is kept beside it as <name>_next.png
        # (D2: the next frame is judged, not assumed). Never "the first file written after the Тогда": with the
        # render pipelined a frame is written after the next tick, so that file was the frame BEFORE.
        # CONSUMED OFFSET (NET-SCN-FRAME-CLAIMED): once frames N/N+1 are taken, r.cursor moves past the line that
        # announced frame N+1, so the next «Тогда» on this role waits for a newer frame - never re-reads these.
        name, what = parts[1], parts[2] if len(parts) > 2 else ""
        stmt = f"«Тогда {r.name} {name}»"
        r.hold = True
        try:
            start, deadline, n, game, got, after = r.cursor, time.time() + 120, None, None, {}, None
            while time.time() < deadline:
                for index in range(start, len(r.lines)):
                    line = r.lines[index]
                    if n is None:
                        m = RECORDED.search(line)
                        if m:
                            n, game = int(m.group(1)), m.group(2)
                            if n in r.claimed:
                                raise Fail(f"{stmt} found {r.name}'s frame_{n:05d} already taken by {r.claimed[n]}")
                        continue
                    m = RECORDED.search(line)
                    if m and int(m.group(1)) == n + 1:
                        after = index + 1
                        continue
                    m = SHOT.search(line)
                    k = SEQNUM.search(m.group(1)) if m and os.path.dirname(m.group(1)) == r.seqdir else None
                    if k and int(k.group(1)) in (n, n + 1) and int(k.group(1)) not in got:
                        frame = int(k.group(1))
                        dst = os.path.join(self.out, name + ("" if frame == n else "_next") + ".png")
                        if frame in r.claimed:
                            raise Fail(f"{stmt} found {r.name}'s frame_{frame:05d} already taken by "
                                       f"{r.claimed[frame]}")
                        # A Ждать on another role may have pumped this one first (hold was off then) and deleted
                        # the announced file: the frame is gone, said by name - never another frame in its place.
                        if not os.path.exists(m.group(1)):
                            raise Fail(f"{r.name} wrote {os.path.basename(m.group(1))} for {name} but the file is gone "
                                       f"(deleted as an unclaimed sequence frame before this «Тогда» took it)")
                        os.replace(m.group(1), dst)
                        got[frame] = dst
                start = len(r.lines)
                if n is not None and len(got) == 2 and after is not None:
                    r.claimed[n] = r.claimed[n + 1] = stmt
                    r.cursor = max(r.cursor, after)
                    return (f"{name}.png\t(frame_{n:05d}, game frame {game}) {what}\n"
                            f"{name}_next.png\t(frame_{n + 1:05d}) the frame after {name}")
                if r.proc.poll() is not None:
                    raise Fail(f"role {r.name} exited (rc {r.proc.returncode}) before frame {name}")
                time.sleep(0.05)
                self.poll()
        finally:
            r.hold = False
        if n is None:
            raise Fail(f"{r.name} recorded no frame (no `[Shot] recorded frame_N`) within 120 s for {name}")
        raise Fail(f"{r.name} recorded frame_{n:05d} for {name} but wrote {sorted(got)} of it and the "
                   f"`recorded frame_{n + 1:05d}` line within 120 s")

    def end(self):
        """Gauntlet ends every role when the test ends: the server is asked first (SIGINT), then the rest."""
        for r in sorted(self.roles.values(), key=lambda r: r.kind == "server"):
            if r.proc.poll() is None:
                try:
                    if not WIN and r.kind == "server":  # a STOPped role answers no SIGINT until it runs again
                        os.killpg(r.proc.pid, signal.SIGCONT)
                    if WIN:
                        r.proc.terminate()
                    else:
                        os.killpg(r.proc.pid, signal.SIGINT if r.kind == "server" else signal.SIGTERM)
                    r.proc.wait(timeout=15)
                except (subprocess.TimeoutExpired, ProcessLookupError):
                    if not WIN:
                        try:
                            os.killpg(r.proc.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    r.proc.wait()
        self.poll()
        for r in self.roles.values():
            if r.seqdir:
                for f in os.listdir(r.seqdir):
                    os.remove(os.path.join(r.seqdir, f))
                os.rmdir(r.seqdir)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    tree, scn = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    if os.path.commonpath([tree, scn]) != tree:
        sys.exit(f"NETSCN refused: {scn} is not inside the tree {tree}")
    lines = open(scn, encoding="utf-8").read().splitlines()
    run = Run(tree, scn)
    with open(os.path.join(run.out, "scenario.scn"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    steps = frames = thens = 0
    index, trace, fail = [], [], ""
    for n, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        kw, _, rest = line.partition(" ")
        op = KEYWORDS.get(kw.lower())
        thens += op == "then"
        if fail:
            continue
        try:
            if op is None:
                raise Fail(f"unknown keyword «{kw}»")
            if op == "project":
                if steps:
                    raise Fail("«Проект» after the first step")
                run.set_project(rest)
                continue
            said = {"world": run.set_world, "role": run.start_role, "wait": run.wait, "console": run.console,
                    "read": run.read, "compare": run.compare, "never": run.never, "then": run.then,
                    "signal": run.signal}[op](rest)
            if op == "then":
                index.append(said)
                frames += 1
            steps += 1
            trace.append(f"{time.strftime('%H:%M:%S')} line {n} ok {said or ''}".rstrip())
            print(trace[-1], flush=True)
        except Fail as e:
            fail = f"line {n} ({kw} {rest.strip()[:80]}): {e}"
            trace.append(f"{time.strftime('%H:%M:%S')} FAIL {fail}")
            print(trace[-1], flush=True)
        except re.error as e:
            fail = f"line {n}: bad regex ({e})"
    run.end()
    errs = []
    for r in run.roles.values():
        errs.append(f"{r.name} {sum(1 for l in r.lines if '[error]' in l or 'assert' in l.lower())}")
    res = (f"FAIL {fail}" if fail else f"PASS {steps} steps, {frames}/{thens} frames") + \
          f"; log errors: {', '.join(errs) or 'no role'}"
    for path, text in (("index.txt", index), ("trace.txt", trace), ("result.txt", [res])):
        with open(os.path.join(run.out, path), "w", encoding="utf-8") as f:
            f.write("\n".join(text) + "\n")
    print(f"SCENARIO {run.code}: {res}")
    print(f"frames: {run.out}  (look at every one: index.txt says what each must show)")
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
