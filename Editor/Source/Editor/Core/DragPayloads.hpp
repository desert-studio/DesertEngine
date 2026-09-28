#pragma once

namespace Desert::Editor::DragPayloads
{
    // Central ImGui drag-drop payload IDs. The emit site (SetDragDropPayload) and the accept site
    // (AcceptDragDropPayload) MUST use the same string, and a mismatch fails SILENTLY — so they live here as
    // one source of truth instead of being retyped as literals across every panel.
    inline constexpr const char* MeshAsset          = "MESH_ASSET";
    inline constexpr const char* MaterialAsset       = "MATERIAL_ASSET";
    inline constexpr const char* TextureAsset        = "TEXTURE_ASSET";
    inline constexpr const char* SkyboxAsset         = "SKYBOX_ASSET";
    inline constexpr const char* FontFile            = "FONT_FILE";
    inline constexpr const char* PrefabFile          = "PREFAB_FILE";
    inline constexpr const char* SceneFile           = "SCENE_FILE";
    inline constexpr const char* AssetFile           = "AssetFile";
    inline constexpr const char* EntityRelationship  = "ENTITY_RELATIONSHIP";
    // A whole collection (the payload is its collection.json path); the foliage palette turns it into types.
    inline constexpr const char* Collection = "COLLECTION";
} // namespace Desert::Editor::DragPayloads
