#pragma once

#include <Engine/Assets/MaterialData.hpp>

#include <string>
#include <vector>

namespace Desert::Assets
{
    // "FOUND BY NAME" AND "FOUND WHAT YOU ASKED FOR" ARE TWO DIFFERENT ANSWERS.
    //
    // This header exists because one function in this engine gave one answer to both questions, in
    // silence, and it made a defect permanent. Editor::MaterialAssetUtils' find-or-create looks a
    // material up by NAME and, when it finds one, hands the caller that material's handle without ever
    // comparing it to the parameters the caller asked for. So the values written at the call site — the
    // only statement anywhere of what a demo material is supposed to be — stop being reachable the
    // moment a file exists under that name, and nothing anywhere says so.
    //
    // It was not hypothetical. `Projects/Desert/Content/Materials/CB_Red.demat` sat in the repository
    // carrying MetallicFactor 1.0 and RoughnessFactor 0.0 while its only author asked for roughness 0.9
    // and no metalness at all; the left wall of the Cornell box rendered as a black chrome mirror, and
    // the builder could not have corrected it on any launch, ever, because it never looked.
    //
    // The remedy the contract prescribes for a silent substitution is to say what was substituted, with
    // the names and the actual values (§1.4). That is what this computes. It is PURE — a request and a
    // MaterialData in, a list of disagreements out — so the reporting can be asserted by a test with no
    // AssetManager, no GPU and no editor, and so a content census can ask the same question offline.

    // One parameter a caller asked for, in the shape the authoring helpers pass them around.
    struct MaterialParamRequest
    {
        std::string Name;
        glm::vec4   Value{ 0.0f };
    };

    // One disagreement between what was asked for and what the material on disk actually says.
    struct MaterialParamDivergence
    {
        std::string Name;
        glm::vec4   Requested{ 0.0f };
        glm::vec4   Found{ 0.0f };
        // The material does not mention this parameter at all. Kept distinct from "mentions it with a
        // different value" because the two have different causes and different fixes: a missing param
        // resolves to the SHADER's declared default (MaterialData::RemoveParam explains why the engine
        // deliberately does not freeze that default into the file), while a present one is somebody's
        // authored answer. Reporting them as one would tell a reader that a file states something it
        // does not.
        bool Absent = false;
    };

    // Every requested parameter the material disagrees with, in request order.
    //
    // Deliberately ONE-DIRECTIONAL: parameters the material carries and the caller did not ask about are
    // NOT divergences. A find-or-create asks "is what I would have written still what is there", and a
    // material legitimately carries more than any one caller mentions — the extra params of a material
    // somebody extended in the editor are not a disagreement with a request that never mentioned them.
    [[nodiscard]] inline std::vector<MaterialParamDivergence>
    DiffRequestedParams( const std::vector<MaterialParamRequest>& requested, const MaterialData& found )
    {
        std::vector<MaterialParamDivergence> divergences;
        for ( const auto& want : requested )
        {
            const glm::vec4* have = found.FindParam( want.Name );
            if ( !have )
            {
                divergences.push_back( { want.Name, want.Value, glm::vec4( 0.0f ), true } );
                continue;
            }
            if ( *have != want.Value )
                divergences.push_back( { want.Name, want.Value, *have, false } );
        }
        return divergences;
    }

    // The divergences as one human-readable clause, with BOTH values for every field — the form §1.4
    // asks for ("log it with the reason and the actual numbers"). A message that said only which fields
    // disagreed would still leave the reader launching a debugger to learn by how much.
    [[nodiscard]] inline std::string DescribeDivergences( const std::vector<MaterialParamDivergence>& divergences )
    {
        std::string text;
        for ( const auto& d : divergences )
        {
            if ( !text.empty() )
                text += "; ";
            text += d.Name;
            if ( d.Absent )
            {
                text += " asked for (" + std::to_string( d.Requested.x ) + ", " + std::to_string( d.Requested.y ) +
                        ", " + std::to_string( d.Requested.z ) + ", " + std::to_string( d.Requested.w ) +
                        ") but the material does not state it";
                continue;
            }
            text += " asked for (" + std::to_string( d.Requested.x ) + ", " + std::to_string( d.Requested.y ) +
                    ", " + std::to_string( d.Requested.z ) + ", " + std::to_string( d.Requested.w ) +
                    ") but the material says (" + std::to_string( d.Found.x ) + ", " +
                    std::to_string( d.Found.y ) + ", " + std::to_string( d.Found.z ) + ", " +
                    std::to_string( d.Found.w ) + ")";
        }
        return text;
    }
} // namespace Desert::Assets
