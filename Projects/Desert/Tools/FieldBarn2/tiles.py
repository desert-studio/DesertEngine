"""Rewrites FIELD_Barn's 16 .dlht tiles from terrain.py (heights unchanged, the path and dry weights re-derived)."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import terrain as t
P = os.path.join(HERE, "..", "..", "Content", "Scenes", "FIELD_Barn_Landscape")
for tz in range(t.TILES):
    for tx in range(t.TILES):
        t.write_tile(f"{P}/{8180000000000100 + tz * t.TILES + tx}.dlht", tx, tz)
print("tiles written")
