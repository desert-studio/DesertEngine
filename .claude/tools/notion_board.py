#!/usr/bin/env python3
"""notion_board.py - the task board through the Notion REST API, without an agent (zero model tokens).

  notion_board.py check                       token works, the board's property names and types
  notion_board.py counts                      open cards by «Статус»
  notion_board.py queue [STATUS]              Windows cards of a status (default «Готово к старту»): code, age
  notion_board.py apply FILE.json             batch of updates: [{"code":..,"status":..,"line":..,"props":{..}}]
                                              a missing «Код» is created as «КОД · line», «Кто ведёт» = Windows · лид

Token: env NOTION_TOKEN, else the file ~/.claude/notion_token (outside the repo, never committed).
The integration must be connected to the board database (••• -> Connections).
Errors are values: a refused call prints the reason and exits 2.
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request

DATA_SOURCE = "386f5e0e-e749-479b-83a7-59e03ef9b8b2"
API = "https://api.notion.com/v1"
VERSION = "2025-09-03"
OPEN = ("В работе", "Готово к старту", "На проверке", "Заблокировано")
WHO_WINDOWS = "Windows · лид"


def token():
    t = os.environ.get("NOTION_TOKEN", "").strip()
    if not t:
        p = os.path.expanduser("~/.claude/notion_token")
        if os.path.exists(p):
            t = open(p, encoding="utf-8").read().strip()
    if not t:
        fail("no token: set NOTION_TOKEN or write ~/.claude/notion_token")
    return t


def fail(msg):
    print("notion_board: " + msg)
    sys.exit(2)


def call(method, path, body=None):
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(API + path, data=data, method=method, headers={
        "Authorization": "Bearer " + token(),
        "Notion-Version": VERSION,
        "Content-Type": "application/json",
    })
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            if e.code == 429 and attempt < 3:
                time.sleep(float(e.headers.get("Retry-After", "1")))
                continue
            fail(f"{method} {path}: HTTP {e.code} {e.read().decode('utf-8', 'replace')[:300]}")
        except urllib.error.URLError as e:
            fail(f"{method} {path}: {e.reason}")
    fail(f"{method} {path}: rate limited")


def schema():
    return call("GET", f"/data_sources/{DATA_SOURCE}")["properties"]


def query(filt=None):
    out, cursor = [], None
    while True:
        body = {"page_size": 100}
        if filt:
            body["filter"] = filt
        if cursor:
            body["start_cursor"] = cursor
        r = call("POST", f"/data_sources/{DATA_SOURCE}/query", body)
        out += r["results"]
        if not r.get("has_more"):
            return out
        cursor = r["next_cursor"]


def text_of(prop):
    if prop is None:
        return ""
    t = prop["type"]
    v = prop.get(t)
    if t in ("title", "rich_text"):
        return "".join(x["plain_text"] for x in v)
    if t in ("select", "status"):
        return v["name"] if v else ""
    if t == "multi_select":
        return ", ".join(x["name"] for x in v)
    if t in ("people",):
        return ", ".join(x.get("name", "") for x in v)
    if t == "last_edited_time" or t == "created_time":
        return v
    if t == "date":
        return v["start"] if v else ""
    if t in ("url", "number", "checkbox", "email", "phone_number"):
        return "" if v is None else str(v)
    return ""


def value_for(ptype, val):
    """Build a property value from a plain string by the property's type in the schema."""
    if ptype == "title":
        return {"title": [{"text": {"content": val}}]}
    if ptype == "rich_text":
        return {"rich_text": [{"text": {"content": val[:2000]}}]}
    if ptype == "status":
        return {"status": {"name": val}}
    if ptype == "select":
        return {"select": {"name": val}}
    if ptype == "multi_select":
        return {"multi_select": [{"name": x.strip()} for x in val.split(",") if x.strip()]}
    if ptype == "url":
        return {"url": val or None}
    if ptype == "checkbox":
        return {"checkbox": val.lower() in ("1", "true", "да", "yes")}
    if ptype == "number":
        return {"number": float(val)}
    if ptype == "date":
        return {"date": {"start": val}}
    fail(f"property type {ptype} is not writable by this tool")


def props_of(sch, fields):
    out = {}
    for name, val in fields.items():
        if name not in sch:
            fail(f"no property «{name}» on the board (see: notion_board.py check)")
        out[name] = value_for(sch[name]["type"], str(val))
    return out


def title_name(sch):
    return next(n for n, p in sch.items() if p["type"] == "title")


def code_filter(sch, code):
    t = sch["Код"]["type"]
    key = "title" if t == "title" else "rich_text"
    return {"property": "Код", key: {"equals": code}}


def status_filter(sch, status):
    t = sch["Статус"]["type"]
    return {"property": "Статус", t: {"equals": status}}


def cmd_check():
    sch = schema()
    for n, p in sorted(sch.items()):
        print(f"{n}: {p['type']}")


def cmd_counts():
    sch = schema()
    for s in OPEN:
        print(f"{s}: {len(query(status_filter(sch, s)))}")


def cmd_queue(status):
    sch = schema()
    now = time.time()
    rows = []
    for pg in query(status_filter(sch, status)):
        pr = pg["properties"]
        if "Windows" not in text_of(pr.get("Кто ведёт")):
            continue
        t = time.mktime(time.strptime(pg["created_time"][:19], "%Y-%m-%dT%H:%M:%S"))
        rows.append(((now - t) / 86400, text_of(pr.get("Код")), text_of(pr.get(title_name(sch)))))
    for age, code, title in sorted(rows, reverse=True):
        print(f"{age:5.1f} d  {code}  {title[:70]}")
    print(f"{len(rows)} card(s)")


def cmd_apply(path):
    batch = json.load(open(path, encoding="utf-8"))
    sch = schema()
    tname = title_name(sch)
    for item in batch:
        code = item["code"]
        fields = dict(item.get("props", {}))
        if item.get("status"):
            fields["Статус"] = item["status"]
        found = query(code_filter(sch, code))
        line = item.get("line", "")
        if found:
            for pg in found:
                if fields:
                    call("PATCH", f"/pages/{pg['id']}", {"properties": props_of(sch, fields)})
                if line:
                    call("PATCH", f"/blocks/{pg['id']}/children", {"children": [{
                        "object": "block", "type": "paragraph",
                        "paragraph": {"rich_text": [{"text": {"content": time.strftime("%m-%d ") + line[:1900]}}]}}]})
            print(f"{code}: updated {len(found)}" + (f" -> {item['status']}" if item.get("status") else ""))
        else:
            fields.setdefault("Кто ведёт", WHO_WINDOWS)
            if tname != "Код":
                fields.setdefault(tname, f"{code} · {line[:80]}")
                fields["Код"] = code
            else:
                fields[tname] = code
            body = {"parent": {"type": "data_source_id", "data_source_id": DATA_SOURCE},
                    "properties": props_of(sch, fields)}
            if line:
                body["children"] = [{"object": "block", "type": "paragraph",
                                     "paragraph": {"rich_text": [{"text": {"content": line[:1900]}}]}}]
            call("POST", "/pages", body)
            print(f"{code}: created" + (f" -> {item['status']}" if item.get("status") else ""))


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    c = argv[0]
    if c == "check":
        cmd_check()
    elif c == "counts":
        cmd_counts()
    elif c == "queue":
        cmd_queue(argv[1] if len(argv) > 1 else "Готово к старту")
    elif c == "apply" and len(argv) == 2:
        cmd_apply(argv[1])
    else:
        fail("unknown command; see notion_board.py --help")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main(sys.argv[1:]))
