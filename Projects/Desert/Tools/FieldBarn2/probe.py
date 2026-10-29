"""Writes Scenes/FB2_Probe.desce: Sun + Sky only — the scene the import session drops meshes into (deleted after)."""
import json
P = "/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-SSR/Projects/Desert/Content/Scenes/"
s = json.load(open(P + "Clouds_Sunset.desce"))
json.dump({"Header": {"Kind": "Scene", "Guid": "fb2fb2fb2fb2fb2fb2fb2fb2fb2fb2f0", "Versions": {"SCNE": 64, "UNIT": 1}, "Dependencies": []},
           "SceneName": "FB2_Probe", "Entities": [e for e in s["Entities"] if e["Tag"] in ("Sun", "Sky")],
           "Settings": s["Settings"]}, open(P + "FB2_Probe.desce", "w"), indent=1)
