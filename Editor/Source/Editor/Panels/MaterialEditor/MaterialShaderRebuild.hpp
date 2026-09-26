#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Desert::Editor
{
    // "The shader behind a material was rebuilt" — the one thing a Material Editor window has to hear about
    // from outside, published by the Node Graph's Compile.
    //
    // WHY THE WINDOW HAS TO HEAR IT AT ALL. A recompile is only half published. The Shader object is shared
    // and reloads itself, but pipelines are cached PER SceneRenderer, and AssetHotReload::PollShaders
    // invalidates only the cache of the scene handed to Tick — the main one. Every Material Editor window
    // owns its own renderer and therefore its own cache, so a window that never heard about the rebuild goes
    // on drawing the modules from BEFORE the compile while the viewport draws the ones from after: a preview
    // that disagrees with the game, which is the stage-1 defect in new clothes
    // (Docs/MaterialEditor/STAGE1_END_TO_END.md).
    //
    // WHY A COUNTER PER SHADER AND NOT A PENDING VALUE. The panel this replaces was a singleton, so a single
    // "one pending request, last one wins" slot was enough. There can now be several windows open on several
    // materials, and a pending value is consumed by whichever reads it first — every other window would keep
    // its stale pipelines. A monotonic count per shader name has no consumer and no clearing: each window
    // remembers the count it last acted on and compares. A window opened after a rebuild starts from the
    // current count and correctly does nothing, because it has no pipelines from before it to drop.
    class MaterialShaderRebuild
    {
    public:
        // @p shaderName as the material names it (SurfaceMaterialAsset::GetShaderName).
        static void Publish( const std::string& shaderName )
        {
            ++Counts()[shaderName];
        }

        // How many times @p shaderName has been rebuilt this session. Never resets, so the comparison a
        // window makes cannot be spoiled by another window reading first.
        [[nodiscard]] static uint64_t CountFor( const std::string& shaderName )
        {
            const auto it = Counts().find( shaderName );
            return it == Counts().end() ? 0ull : it->second;
        }

    private:
        static std::unordered_map<std::string, uint64_t>& Counts()
        {
            static std::unordered_map<std::string, uint64_t> s_Counts;
            return s_Counts;
        }
    };
} // namespace Desert::Editor
