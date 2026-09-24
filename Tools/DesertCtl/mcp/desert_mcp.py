#!/usr/bin/env python3
"""A thin MCP server over the Desert editor's control channel.

THE CHANNEL IS PRIMARY AND THIS IS A SHIM. Everything below is a translation of an MCP tool call into one
`desertctl` invocation and back; there is no state here, no retry policy, and no vocabulary of its own. That
is deliberate, and it is the decision the channel was designed around: the channel has to be useful to CI
and to headless verification runs that have never heard of MCP, so nothing in it may be shaped by this file.
If a tool here ever wants the channel to behave differently, the answer is to say so rather than to bend the
channel — the channel is the contract, this is one of its clients.

WHAT THE TOOLS ARE. They are not a curated list of editor features: `run_command` executes whatever the
editor's own COMMAND PALETTE offers this instant, and `list_commands` is how an agent finds out what that
is. So a capability added to the editor for a person appears here with nobody touching this file, which is
the same property that made the channel worth building in the first place.

THE SAME IS TRUE OF THE PROPERTIES, and for the same reason. `list_properties` is not a table of material
parameters kept here — it is whatever the focused document derives from its own declaration, which for a
material is its shader's `Properties` block. A shader author who adds a parameter gets it in this list
without anybody editing this file, an editor panel, or the channel. That is the half of "everything a
person can do" that has no name: a slider cannot be a palette entry, so it is a second CATEGORY of request
rather than a second way of executing one. `set_property` lands in the same setter the window's own control
calls.

WHY THE SHOTS ARE TWO TOOLS AND NOT ONE WITH A FLAG. `capture_window` reads the presented frame and contains
the panels, the menus and the dialogs; `capture_viewport` reads the scene's own image and contains none of
them. Before this channel existed only the second was possible at all, which is why no picture this engine
ever took held a pixel of its interface. A single tool that silently answered one with the other would
deliver a picture of the wrong subject under the right name.

Usage:
    desert_mcp.py --socket /tmp/desert-editor.sock [--desertctl path/to/DesertCtl]

Speaks MCP over stdio: one JSON-RPC message per line.
"""

import argparse
import json
import os
import subprocess
import sys

PROTOCOL_VERSION = "2024-11-05"


class ChannelError(RuntimeError):
    """The editor refused, or could not be reached. Carried separately from a tool's own bad arguments,
    because the two need different fixes: one is the editor's answer, the other is this client's mistake."""


class Channel:
    """One `desertctl` invocation per call. No connection is held between calls, and that is not a
    simplification — the editor answers a command only after a frame that already reflects it, so the reply
    IS the synchronisation, and a held connection would buy nothing but a state to get wrong.

    AND THERE IS NO RETRY LOOP HERE EITHER, which used to be impossible. A request that arrives before the
    editor has finished coming up is HELD by the editor until a presented frame proves it has, then run —
    so one call is already the wait. Before that, `list_commands` was answered from a half-built editor:
    measured at 0, then 106, then 130 of one project's openable assets as the startup stages filled the
    asset cache, with nothing in the reply to say which it was. Every client had to guess how long to wait
    and ask again. `get_state` is the exception and deliberately so — it answers throughout the boot,
    because its `quiescence` section is how readiness is OBSERVED."""

    def __init__(self, binary: str, socket_path: str):
        self.binary = binary
        self.socket_path = socket_path

    def call(self, *args: str, wait: float = 0.0, subject: str = "") -> dict:
        command = [self.binary, "--socket", self.socket_path]
        if wait:
            command += ["--wait", str(wait)]
        # Passed through unvalidated, exactly as the tool passes it to the editor: the closed set of
        # subjects belongs to the protocol, and a copy of it here would be the list that falls behind.
        if subject:
            command += ["--subject", subject]
        command += list(args)

        try:
            done = subprocess.run(command, capture_output=True, text=True, timeout=300)
        except FileNotFoundError as exc:
            raise ChannelError(
                f"{self.binary} is not there. Build it: make DesertCtl config=debug"
            ) from exc
        except subprocess.TimeoutExpired as exc:
            raise ChannelError(
                "the editor did not answer within 300 s. It is wedged, or a command is waiting for a "
                "frame that will never settle."
            ) from exc

        # Exit code 2 means the tool never reached an editor — a different thing from a refusal, and worth
        # saying so rather than reporting an empty reply.
        if done.returncode == 2:
            raise ChannelError(done.stderr.strip() or "no editor is listening on that socket.")

        line = done.stdout.strip()
        if not line:
            raise ChannelError(done.stderr.strip() or "the editor answered with nothing at all.")

        try:
            reply = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ChannelError(f"the editor's reply is not JSON: {line!r}") from exc

        # The outcome is read from the parsed reply, never guessed from the exit code alone: a reply that
        # carries no outcome is a defect in the channel and must not be reported as success.
        if "ok" not in reply:
            raise ChannelError("the reply carries no outcome; that is a defect in the channel.")
        if not reply["ok"]:
            raise ChannelError(reply.get("error", "refused, with no reason given."))
        return reply


TOOLS = [
    {
        "name": "list_commands",
        "description": (
            "Every command the editor offers RIGHT NOW, as group/label pairs. This is the editor's own "
            "command palette: panels, open documents, entities in the scene, the menu bar, openable "
            "assets, preview viewpoints, actions. Ask this before run_command — the list changes as "
            "documents open and close."
        ),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "run_command",
        "description": (
            "Run one command palette entry, addressed by its group and label exactly as list_commands "
            "reports them. The match is exact on both halves; a near miss is refused with suggestions "
            "rather than run. Returns once a rendered frame already reflects the command."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "group": {"type": "string", "description": "e.g. Panel, Document, Entity, Menu, Open, Preview, Action"},
                "label": {"type": "string", "description": "the label exactly as list_commands reports it"},
            },
            "required": ["group", "label"],
        },
    },
    {
        "name": "list_properties",
        "description": (
            "The properties a subject exposes, with the value each one is showing right now, its type, "
            "how many numbers it takes and any declared range. This is the other half of what a person "
            "can do: run_command covers everything with a name, this covers everything a mouse does by "
            "DRAGGING. The default subject is the FOCUSED DOCUMENT, whose list is derived from its own "
            "declaration — for a material, its shader's Properties block — so it is never out of date "
            "with the window. Rows that cannot be written (texture slots, asset references) are listed "
            "with a reason rather than omitted. Subject 'viewport' is the editor's own view: "
            "Camera.Position and Camera.Direction, which is how the camera is placed for a capture "
            "without any --shot flag."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "subject": {
                    "type": "string",
                    "description": "'document' (the focused one, the default), 'viewport', or 'modeling' (the Modeling panel's dragged values)",
                },
            },
        },
    },
    {
        "name": "set_property",
        "description": (
            "Write one property of the focused document — the drag a mouse would do. The value goes "
            "through the same setter the window's own control calls, so this and a person's drag are one "
            "code path. Send exactly as many numbers as list_properties says the property takes; a wrong "
            "count, an unknown name, a value outside the declared range and a texture slot are all "
            "refused with a reason rather than half-written. For a STAGED document (a material) the write "
            "lands in the working copy and the scene does not change until you run its Apply command. "
            "With subject 'viewport' this places the EDITOR CAMERA — Camera.Position and "
            "Camera.Direction, three numbers each, world units of one centimetre — through the editor's "
            "own view-axis-gizmo and F-focus, the same path a person's hands take. "
            "Returns once a rendered frame already reflects the write."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "subject": {
                    "type": "string",
                    "description": "'document' (the focused one, the default), 'viewport', or 'modeling' (the Modeling panel's dragged values)",
                },
                "name": {"type": "string", "description": "the property name exactly as list_properties reports it"},
                "value": {
                    "type": "array",
                    "items": {"type": "number"},
                    "description": "one to four numbers; the count must match the property's own shape",
                },
            },
            "required": ["name", "value"],
        },
    },
    {
        "name": "get_state",
        "description": (
            "The editor's state as JSON. Sections: scene, selection, documents (open in most-recently-"
            "used order, plus recently closed), panels, renderer_slots (live/pending of six), log "
            "(counts and tail), quiescence. Omit sections for all of them."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "sections": {"type": "array", "items": {"type": "string"}},
            },
        },
    },
    {
        "name": "capture_window",
        "description": (
            "Capture the WHOLE editor to a PNG — the scene AND the interface drawn over it: panels, "
            "menus, dialogs, the document tabs. Taken on a frame that already reflects every command "
            "before it. Use this to prove anything about the interface."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string", "description": "where to write the PNG"}},
            "required": ["path"],
        },
    },
    {
        "name": "capture_viewport",
        "description": (
            "Capture the 3D viewport ONLY, with no interface in it at all. Use this for rendering "
            "evidence. It is NOT a substitute for capture_window and does not contain any panel."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string", "description": "where to write the PNG"}},
            "required": ["path"],
        },
    },
]


def dispatch(channel: Channel, name: str, arguments: dict) -> dict:
    if name == "list_commands":
        return channel.call("commands", wait=120)
    if name == "run_command":
        group = arguments.get("group", "")
        label = arguments.get("label", "")
        if not group or not label:
            raise ValueError("run_command needs both a group and a label; ask list_commands for the pairs.")
        return channel.call("run", group, label)
    if name == "list_properties":
        return channel.call("properties", wait=120, subject=arguments.get("subject", ""))
    if name == "set_property":
        prop = arguments.get("name", "")
        value = arguments.get("value", [])
        if not prop:
            raise ValueError("set_property needs a name; ask list_properties for the ones on offer.")
        if not isinstance(value, list) or not value:
            raise ValueError(
                "set_property needs a value as a list of numbers, even for a single component ([0.2]) — "
                "the COUNT is part of the property's identity and the editor checks it."
            )
        # Formatted here rather than passed as a list, because the transport is one desertctl argument.
        # No padding and no truncation: a wrong count must reach the editor as a wrong count.
        return channel.call(
            "set", prop, ",".join(repr(float(v)) for v in value), subject=arguments.get("subject", "")
        )
    if name == "get_state":
        return channel.call("state", *arguments.get("sections", []), wait=120)
    if name == "capture_window":
        path = arguments.get("path", "")
        if not path:
            raise ValueError("capture_window needs a path; a capture with nowhere to go leaves no evidence.")
        return channel.call("shot-window", path)
    if name == "capture_viewport":
        path = arguments.get("path", "")
        if not path:
            raise ValueError("capture_viewport needs a path.")
        return channel.call("shot-viewport", path)
    raise ValueError(f"'{name}' is not a tool this server offers.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, help="the editor's --control-socket path")
    parser.add_argument(
        "--desertctl",
        default=os.environ.get("DESERTCTL", "build/Bin/Debug/DesertCtl"),
        help="path to the DesertCtl binary",
    )
    options = parser.parse_args()
    channel = Channel(options.desertctl, options.socket)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue  # not a message; a parse error with no id has nowhere to be reported to

        method = message.get("method")
        request_id = message.get("id")

        def reply(result=None, error=None):
            # A notification (no id) gets no answer — that is the JSON-RPC rule, and answering one would
            # put an unexpected message in front of the next real reply.
            if request_id is None:
                return
            body = {"jsonrpc": "2.0", "id": request_id}
            if error is not None:
                body["error"] = error
            else:
                body["result"] = result
            sys.stdout.write(json.dumps(body) + "\n")
            sys.stdout.flush()

        if method == "initialize":
            reply(
                {
                    "protocolVersion": PROTOCOL_VERSION,
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "desert-editor", "version": "1"},
                }
            )
        elif method == "tools/list":
            reply({"tools": TOOLS})
        elif method == "tools/call":
            params = message.get("params", {})
            try:
                answer = dispatch(channel, params.get("name", ""), params.get("arguments", {}) or {})
                reply({"content": [{"type": "text", "text": json.dumps(answer)}]})
            except (ChannelError, ValueError) as exc:
                # REPORTED AS A FAILED TOOL CALL, not as a protocol error. The editor refusing is a fact
                # about the world that the model needs to read and act on; a JSON-RPC error would be
                # swallowed as a transport fault and the reason would never reach it.
                reply({"content": [{"type": "text", "text": str(exc)}], "isError": True})
        elif method is not None and request_id is not None:
            reply(error={"code": -32601, "message": f"no method '{method}'"})

    return 0


if __name__ == "__main__":
    sys.exit(main())
