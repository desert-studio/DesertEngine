#pragma once

#include <Engine/Graphic/Materials/MaterialBinder.hpp>
#include <Engine/Graphic/Materials/MaterialLayoutStamp.hpp>

#include <cstdint>
#include <string>

namespace Desert::Graphic
{
    class Texture2DProperty;

    // A Texture2D property of a DataDrivenMaterial, found by its sampler name once per layout and executor
    // (DataDrivenMaterial::ResolveTexture) and bound through the pointer (DataDrivenMaterial::SetTexture(
    // MaterialTextureHandle&, ... )). The property is the executor's for the executor's whole life
    // (InitializeProperties runs once, in its constructor), so a handle whose stamp and executor are the
    // material's current ones points at a live property. Name is the sampler it was resolved for: a handle kept
    // by position is re-resolved when the name at that position is another one.
    struct MaterialTextureHandle
    {
        MaterialLayoutStamp Stamp;
        const void*         Executor = nullptr;
        Texture2DProperty*  Property = nullptr; // null after resolving: the shader declares no such texture
        std::string         Name;
        bool                Resolved = false;
    };

    // A push-constant field of a Material, resolved once per layout (MaterialBinder::ResolvePush) and written at
    // its offset (Material::WritePushField( MaterialPushHandle&, ... )). Status is the resolution's verdict for
    // the size this call site writes; a size mismatch keeps being reported per write, as before.
    struct MaterialPushHandle
    {
        MaterialLayoutStamp       Stamp;
        MaterialBinder::PushWrite Status   = MaterialBinder::PushWrite::Absent;
        uint32_t                  Offset   = 0;
        bool                      Resolved = false;
    };

    // The push fields Material's own per-draw setters write (SetMaterialIndex, SetPushMatrix, ...): one handle
    // each, held by the material, so a draw's push writes index the layout instead of searching it by name.
    struct MaterialPushHandles
    {
        MaterialPushHandle MaterialIndex;
        MaterialPushHandle Transform;
        MaterialPushHandle PrimitiveIndex;
        MaterialPushHandle WindA;
        MaterialPushHandle WindB;
        MaterialPushHandle PrimitiveFlags;
        MaterialPushHandle ShadowSlope;
        MaterialPushHandle BoneOffset;
        // The skin cache's fields (Vertex_Skinned.glslh / Silhouette_Skinned.shader), written per skinned draw by
        // MaterialShadowSkinned::SetSkinCache and MaterialSilhouetteSkinned::SetSkinCache.
        MaterialPushHandle SkinCacheVertexCount;
        MaterialPushHandle SkinCacheAddressLo;
        MaterialPushHandle SkinCacheAddressHi;
    };
} // namespace Desert::Graphic
