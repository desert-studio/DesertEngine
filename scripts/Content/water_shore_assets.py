#!/usr/bin/env python3
"""The shore's assets from Poly Haven (CC0, https://polyhaven.com/license): ground textures, rocks, layers, grass.

Textures: sand_03, coast_sand_03, seaside_rock (2k diff / nor_gl / arm / disp jpg). Models: namaqualand_boulder_03,
namaqualand_boulder_04, namaqualand_boulder_06 (2k gltf). The dune grass layer uses FieldBarn's leafy_grass (Poly Haven
CC0) already in Content, its blades are FieldBarn's grass_bermuda_01.

Usage: python -I scripts/Content/water_shore_assets.py <download dir>   (<dir>/<asset id>/... as Poly Haven ships them)
"""
import json, os, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from content_files import content, dump_text, envelope_guid, guid_of, load_text, write_texture

FIELD = "Environments/FieldBarn/"
SHORE = "Environments/Shore/"
LIKE_LEAFY = FIELD + "Textures/leafy_grass/leafy_grass_%s_2k.detex"
LIKE = {"diff": LIKE_LEAFY % "Diffuse", "nor_gl": LIKE_LEAFY % "nor_gl", "arm": LIKE_LEAFY % "arm",
        "disp": LIKE_LEAFY % "disp"}

# The layer's name, its textures, the material's tint / tile / normal strength, its debug colour.
# Measured mean linear albedo: sand_03 (.153 .118 .076) -> dry sand, luminance 0.38; coast_sand_03 (.217 .186 .130);
# seaside_rock (.058 .049 .035) -> basalt-grey 0.10; leafy_grass (.321 .236 .101) -> a dry dune sward.
LAYERS = [
    ("ShoreSand", "sand_03", (2.9, 3.15, 3.6), 200.0, 1.0, (0.45, 0.38, 0.27)),
    ("ShorePebble", "coast_sand_03", (1.2, 1.25, 1.35), 300.0, 1.0, (0.3, 0.27, 0.22)),
    ("ShoreRock", "seaside_rock", (2.0, 2.0, 2.1), 250.0, 1.2, (0.1, 0.1, 0.1)),
    ("ShoreGrass", None, (0.45, 0.52, 0.4), 250.0, 1.4, (0.11, 0.17, 0.045)),
]
BOULDERS = {
    "namaqualand_boulder_03": ("boulder_03", (-1.0548, -0.0147, -1.3896), (1.3523, 1.4585, 1.6758)),
    "namaqualand_boulder_04": ("boulder_04", (-1.3403, -0.0294, -1.0310), (1.1819, 1.8711, 1.4654)),
    "namaqualand_boulder_06": ("namaqualand_boulder_06", (-0.3207, -0.0185, -0.6608), (0.3543, 0.6332, 0.5511)),
}


def read(path):
    with open(path, "rb") as f:
        return f.read()


def layer_file(name):
    return "Landscape/Layers/Shore/%s.delayerinfo" % name


def ground_textures(downloads):
    result = {}
    for _, asset, *_ in LAYERS:
        for kind in LIKE:
            if asset is None:
                path = LIKE[kind]
                result[asset, kind] = {"Guid": envelope_guid(content(path)), "Path": "assets:" + path}
            else:
                name = "%s_%s_2k" % (asset, kind)
                result[asset, kind] = write_texture("%sTextures/%s/%s.detex" % (SHORE, asset, name),
                                                    read(os.path.join(downloads, asset, name + ".jpg")), ".jpg", LIKE[kind])
    return result


def landscape_material(textures, layers):
    path = content(SHORE + "Materials/M_Shore_Landscape.demat")
    material = load_text(path)
    material["Params"] = [p for p in material["Params"] if not p["Name"].startswith("Layer")]
    for p in material["Params"]:
        if p["Name"] == "DisplacementMagnitude":
            p["Value"] = [6.0, 0.0, 0.0, 0.0]
    material["Textures"], material["CloudAssets"] = [], []
    for i, (name, asset, tint, tile, normal, _) in enumerate(LAYERS):
        def param(suffix, value):
            material["Params"].append({"Name": "Layer%d%s" % (i, suffix), "Value": value})
        param("Blend", [1.0, 0.0, 0.0, 0.0])
        param("TileSize", [tile, 0.0, 0.0, 0.0])
        param("Tint", [*tint, 1.0])
        param("NormalStrength", [normal, 0.0, 0.0, 0.0])
        for kind, slot in (("diff", "Albedo"), ("nor_gl", "Normal"), ("arm", "Arm"), ("disp", "Height")):
            material["Textures"].append({"Name": "Layer%d%sTexture" % (i, slot), **textures[asset, kind]})
        material["CloudAssets"].append({"Name": "Layer%dLayer" % i, **layers[name]})
    deps = [material["Shader"]["Guid"]] + [t["Guid"] for t in material["Textures"]] + [
        l["Guid"] for l in material["CloudAssets"]]
    material["Header"]["Dependencies"] = list(dict.fromkeys(deps))
    dump_text(path, material)


def grass_type():
    """The dune grass: bermuda tufts in patches, thinning with the layer's weight."""
    path = "Landscape/Grass/Shore/DuneGrass.degrasstype"
    varieties = []
    for blade, density, coverage, low, high in (("medium_a", 260.0, 0.6, 1.6, 2.4), ("medium_c", 260.0, 0.6, 1.6, 2.4),
                                                ("medium_e", 200.0, 0.5, 1.4, 2.2), ("dead_a", 120.0, 0.45, 1.4, 2.2),
                                                ("flattened_a", 90.0, 0.35, 1.4, 2.0)):
        mesh = "%sMeshes/grass_bermuda_01/grass_bermuda_01_grass_bermuda_01_%s.stmesh" % (FIELD, blade)
        scale = {"Min": low, "Max": high}
        varieties.append({
            "Kind": "Mesh", "Mesh": {"Guid": envelope_guid(content(mesh)), "Path": mesh}, "Density": density,
            "PlacementJitter": 1.0, "StartCullDistance": 2400.0, "EndCullDistance": 8000.0,
            "AllowedDensityRange": {"Min": 0.0, "Max": 1.0},
            "Patches": {"Coverage": coverage, "Size": 700.0, "Softness": 0.35, "Group": 1},
            "Scaling": "Uniform", "ScaleX": scale, "ScaleY": scale, "ScaleZ": scale,
            "WeightAttenuatesMaxScale": True, "MaxScaleWeightAttenuation": 0.5, "RandomRotation": True,
            "AlignToSurface": True, "CastDynamicShadow": True,
            "Wind": {"Strength": 1.0, "Speed": 0.5, "Height": 70.0}})
    dump_text(content(path), {
        "Header": {"Kind": "LandscapeGrassType", "Guid": guid_of(path), "Versions": {"LGRT": 3},
                   "Dependencies": [v["Mesh"]["Guid"] for v in varieties]},
        "GrassVarieties": varieties})
    return {"Guid": guid_of(path), "Path": path}


def layer_infos():
    grass = grass_type()
    result = {}
    for name, _, _, _, _, colour in LAYERS:
        path = layer_file(name)
        guid = load_text(content(path))["Header"]["Guid"] if os.path.exists(content(path)) else guid_of(path)
        grasses = [grass] if name == "ShoreGrass" else []
        info = {"Header": {"Kind": "LandscapeLayerInfo", "Guid": guid, "Versions": {"LLYI": 4},
                           "Dependencies": [g["Guid"] for g in grasses]},
                "LayerName": name, "Hardness": 0.5, "NoWeightBlend": False, "LayerUsageDebugColor": list(colour)}
        if grasses:
            info["GrassTypes"] = grasses
        dump_text(content(path), info)
        result[name] = {"Guid": guid, "Path": "assets:" + path}
    return result


def boulders(downloads):
    for name, (gltf_material, low, high) in BOULDERS.items():
        folder = SHORE + "Meshes/" + name
        os.makedirs(content(folder + "/textures"), exist_ok=True)
        for file in (name + ".gltf", name + ".bin"):
            shutil.copyfile(os.path.join(downloads, name, file), content(folder + "/" + file))
        textures = {}
        for kind, suffix in (("diff", ""), ("arm", "_ORMTexture"), ("nor_gl", "")):
            file = "%s_%s_2k" % (name, kind)
            source = read(os.path.join(downloads, name, "textures", file + ".jpg"))
            with open(content("%s/textures/%s.jpg" % (folder, file)), "wb") as f:
                f.write(source)
            like = "%sMeshes/boulder_01/textures/boulder_01_%s_2k%s.detex" % (FIELD, kind, suffix)
            textures[kind] = write_texture("%s/textures/%s%s.detex" % (folder, file, suffix), source, ".jpg", like)
        dump_text(content(folder + "/" + name + ".gltf.deimport"), {
            "Header": {"Kind": "StaticMesh", "Guid": guid_of(folder + "/" + name + ".stmesh"), "Versions": {"DIMP": 2},
                       "Dependencies": []},
            "Source": name + ".gltf",
            "Bounds": {"Min": [100.0 * c for c in low], "Max": [100.0 * c for c in high]},
            "Settings": {"CombineMeshes": False, "UniformScale": 1.0, "UpAxis": "FromFile", "LodPolicy": "Generate",
                         "VirtualGeometry": True}})
        material = load_text(content("Materials/%sMeshes/boulder_01/boulder_01/boulder_01.demat" % FIELD))
        path = "Materials/%s/%s/%s.demat" % (folder, name, gltf_material)
        for slot, kind in (("AlbedoTexture", "diff"), ("OrmTexture", "arm"), ("NormalTexture", "nor_gl")):
            for t in material["Textures"]:
                if t["Name"] == slot:
                    t.update(textures[kind])
        material["Header"]["Guid"] = guid_of(path)
        material["Header"]["Dependencies"] = [material["Shader"]["Guid"]] + [t["Guid"] for t in material["Textures"]]
        dump_text(content(path), material)


def main():
    downloads = sys.argv[1]
    landscape_material(ground_textures(downloads), layer_infos())
    boulders(downloads)


if __name__ == "__main__":
    main()
