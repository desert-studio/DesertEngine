"""Rewrites FIELD_Barn's 16 .dlht tiles from terrain.py (heights unchanged, the path weights re-derived)."""
import sys; sys.path.insert(0, "/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-scratch/FIELD-BARN-2")
import terrain as t
P = "/Users/daniilsavcenko/Desktop/Programming/C++/DesertEngine-SSR/Projects/Desert/Content/Scenes/FIELD_Barn_Landscape"
for tz in range(t.TILES):
    for tx in range(t.TILES):
        t.write_tile(f"{P}/{8180000000000100 + tz * t.TILES + tx}.dlht", tx, tz)
print("tiles written")
