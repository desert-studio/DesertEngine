import json, sys, urllib.request, os, subprocess

ROOT = "/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-SSR/Projects/Desert/Content/Environments/FieldBarn"


def fetch(rel, url, out):
    p = os.path.join(out, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    if not os.path.exists(p):
        subprocess.run(["curl", "-sfL", "-A", "Mozilla/5.0", "-o", p, url], check=True)


# usage: dl.py model <ids...> | dl.py tex <ids...>
kind = sys.argv[1]
for a in sys.argv[2:]:
    d = json.loads(subprocess.run(["curl", "-sf", "-A", "Mozilla/5.0", "https://api.polyhaven.com/files/" + a], capture_output=True, check=True).stdout)
    if kind == "model":
        g = d["gltf"]["2k"]["gltf"]
        out = os.path.join(ROOT, "Meshes", a)
        fetch(a + ".gltf", g["url"], out)
        for k, v in g["include"].items():
            fetch(k, v["url"], out)
    else:
        out = os.path.join(ROOT, "Textures", a)
        for m in ("Diffuse", "nor_gl", "Rough", "AO", "arm"):
            if m in d and "2k" in d[m]:
                fetch(f"{a}_{m}_2k.jpg", d[m]["2k"]["jpg"]["url"], out)
    print("ok", a, flush=True)
