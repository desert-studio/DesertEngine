#pragma once

#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Desert::Graphic
{
    // THE LAYOUT A HANDLE WAS RESOLVED AGAINST (UE: an FShaderParameter is bound once per shader map, and a
    // recompile makes a new map). Shader::GetSerial with Shader::GetCodeGeneration names exactly one
    // MaterialLayout (Shader.hpp: the layout is replaced only by a build that bumps the generation), so a handle
    // whose stamp equals the material's current one indexes the current layout; any other stamp - a hot reload, a
    // material moved onto another program, a handle never resolved - is re-resolved by name before the write.
    // {0, 0} is "no program": a handle resolved there misses until the material has one.
    struct MaterialLayoutStamp
    {
        uint64_t Serial     = 0;
        uint64_t Generation = 0;

        [[nodiscard]] bool operator==( const MaterialLayoutStamp& ) const noexcept = default;
    };

    // A row parameter of a DataDrivenMaterial, resolved once per layout (DataDrivenMaterial::ResolveParam) and
    // written by slot (DataDrivenMaterial::SetParam( MaterialParamHandle&, ... )): the per-frame writer holds
    // one per parameter it writes and pays a stamp compare, not a walk of the layout's names.
    struct MaterialParamHandle
    {
        static constexpr uint32_t kNone = ~0u;

        MaterialLayoutStamp Stamp;
        uint32_t            Slot  = kNone; // row slot (Offset / kMaterialParamSlotSize); kNone = not a row value
        uint32_t            Param = kNone; // index in MaterialLayout::Params (its declared type)

        [[nodiscard]] bool IsValue() const noexcept
        {
            return Slot != kNone;
        }
    };

    // WHAT A SET OF PARAMETER OVERRIDES WAS LAST WRITTEN AS (UE: FMaterialRenderProxy's uniform expression cache,
    // rebuilt when a parameter moves and reused otherwise), kept with the overrides it describes
    // (MaterialOverrides::Resolved) and maintained by DataDrivenMaterial::SyncOverrideRevision:
    // - Handles: one per Params entry, resolved once per layout stamp, so a row write indexes the layout;
    // - Written: the values the revision below stands for, compared bitwise each frame (no name is read);
    // - ParamsData / ParamsCount: the Params storage they were taken from - another vector (a rebuilt override
    //   set, a reallocation) or another count drops the handles and takes a new revision;
    // - Revision: process-unique (NextMaterialRevision), 0 = not taken yet. With the material's base revision it
    //   is the scene row's MaterialRowRevision: an unchanged record reuses its row without writing it.
    // A COPY OR A MOVE STARTS EMPTY: the handles describe one Params vector, never the one a copy holds.
    // Not thread-safe: written by the one walk that builds a frame's rows (MeshRenderer::RegisterGenericRows).
    struct MaterialOverrideHandles
    {
        std::vector<MaterialParamHandle> Handles;
        std::vector<glm::vec4>           Written;
        const void*                      ParamsData  = nullptr;
        std::size_t                      ParamsCount = 0;
        uint64_t                         Revision    = 0;

        MaterialOverrideHandles() noexcept = default;
        MaterialOverrideHandles( const MaterialOverrideHandles& /*other*/ ) noexcept
        {
        }
        MaterialOverrideHandles( MaterialOverrideHandles&& /*other*/ ) noexcept
        {
        }
        MaterialOverrideHandles& operator=( const MaterialOverrideHandles& other ) noexcept
        {
            if ( this != &other )
                Reset();
            return *this;
        }
        MaterialOverrideHandles& operator=( MaterialOverrideHandles&& other ) noexcept
        {
            if ( this != &other )
                Reset();
            return *this;
        }
        ~MaterialOverrideHandles() = default;

        void Reset() noexcept
        {
            Handles.clear();
            Written.clear();
            ParamsData  = nullptr;
            ParamsCount = 0;
            Revision    = 0;
        }
    };
} // namespace Desert::Graphic
