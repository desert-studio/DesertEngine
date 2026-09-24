#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/UUID.hpp>

namespace Desert::Assets
{
    // One parameter of a material, keyed by the owning shader's schema name (DSL Properties /
    // #pragma param). Plain aggregate — serialized by reflect-cpp as part of the .demat.
    struct MaterialShaderParam
    {
        std::string Name;
        glm::vec4   Value = glm::vec4( 0.0f );
    };

    // One texture binding of a material (sampler name -> TextureAsset handle).
    struct MaterialShaderTexture
    {
        std::string Name;
        uint64_t    TextureHandle = 0;
    };

    // THE material asset payload (.demat) — the single protocol for every material.
    //
    // A material is a shader + parameter values, nothing else (Unity model). The shader's schema
    // (declared in the .shader file) defines which params exist, their types, ranges and UI; this
    // struct only stores the values by name. The optimized PBR backend consumes a typed VIEW of
    // these values (PBRSurfaceParams) — an implementation detail, not part of the protocol.
    struct MaterialData
    {
        // First member: the text header (kind Material, GUID, MATL schema version). Stamped and checked
        // only through MaterialFormat.hpp.
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        // Shader driving this material. Absent/empty -> "StaticMeshPBR" (the standard surface
        // shader with the batched backend).
        std::optional<std::string> ShaderName;

        std::vector<MaterialShaderParam>   Params;
        std::vector<MaterialShaderTexture> Textures;

        // MATERIAL INSTANCE (UE model): when set, this asset is a CHILD of the material whose header GUID this
        // names (32 hex digits, AssetGuidToText), and Params/Textures hold ONLY the overridden values - the
        // shader and every non-overridden parameter come from the parent chain. The same GUID is the header's
        // Dependency (StampMaterialHeader), so a registry sees the edge without reading the body.
        //
        // There is no second identity in the payload: the material IS its header GUID, and its runtime
        // handle is that GUID through the one fold (Handle()). MATL v1 carried a u64 MaterialId beside the
        // GUID - two statements of one identity - and Tools/SceneMigrator removes it (MATL 1 -> 2).
        std::optional<std::string> Parent;

        bool IsInstance() const
        {
            return ParentText() != nullptr;
        }

        /// This material's GUID, from its header; null when there is no header or its GUID is malformed.
        /// ParseMaterialJson refuses both for a file, so a null here is a material never written or read.
        [[nodiscard]] Common::Content::AssetGuid Guid() const
        {
            return Header ? GuidFromText( Header->Guid ) : Common::Content::AssetGuid{};
        }

        /// THE MATERIAL'S HANDLE: its GUID through Common::Content::HandleForGuid, the one fold every
        /// consumer shares (asset handle, MaterialService's external id, a mesh slot's reference).
        [[nodiscard]] Common::UUID Handle() const
        {
            return HandleOf( Guid() );
        }

        /// `Parent`'s text when this is an instance, null otherwise. ONE call answers both halves, so a caller
        /// never dereferences `Parent` on the word of a separate IsInstance().
        [[nodiscard]] const std::string* ParentText() const
        {
            if ( !Parent.has_value() || Parent->empty() )
                return nullptr;
            return &*Parent;
        }

        /// The GUID `Parent` names; null when this is not an instance or `Parent` is malformed.
        [[nodiscard]] Common::Content::AssetGuid ParentGuid() const
        {
            const std::string* parent = ParentText();
            return parent != nullptr ? GuidFromText( *parent ) : Common::Content::AssetGuid{};
        }

        /// Makes this an instance of `parent`, or a base material when `parent` is null.
        void SetParent( const Common::Content::AssetGuid& parent )
        {
            if ( parent.IsNull() )
                Parent.reset();
            else
                Parent = Common::Content::AssetGuidToText( parent );
        }

        /// THE PARENT THIS INSTANCE OVERRIDES, as the handle it is registered under, or nothing when this
        /// asset is not an instance. ONE call answers both halves, so the guard and the value cannot be
        /// asked of two different copies of this struct.
        [[nodiscard]] std::optional<Common::UUID> InstanceParentId() const
        {
            if ( !IsInstance() )
                return std::nullopt;
            return HandleOf( ParentGuid() );
        }

        static Common::UUID HandleOf( const Common::Content::AssetGuid& guid )
        {
            return Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) );
        }

        static Common::Content::AssetGuid GuidFromText( std::string_view text )
        {
            const auto parsed = Common::Content::AssetGuidFromText( text );
            return parsed ? parsed.GetValue() : Common::Content::AssetGuid{};
        }

        // ── Queries ────────────────────────────────────────────────────────────────
        std::string EffectiveShaderName() const
        {
            return ( ShaderName && !ShaderName->empty() ) ? *ShaderName : "StaticMeshPBR";
        }

        bool UsesCustomShader() const
        {
            const auto name = EffectiveShaderName();
            return name != "StaticMeshPBR" && name != "SkinnedMeshPBR";
        }

        const glm::vec4* FindParam( std::string_view name ) const
        {
            for ( const auto& p : Params )
                if ( p.Name == name )
                    return &p.Value;
            return nullptr;
        }

        glm::vec4 GetParam( std::string_view name, const glm::vec4& fallback = glm::vec4( 0.0f ) ) const
        {
            const auto* v = FindParam( name );
            return v ? *v : fallback;
        }

        float GetFloat( std::string_view name, float fallback = 0.0f ) const
        {
            const auto* v = FindParam( name );
            return v ? v->x : fallback;
        }

        void SetParam( std::string_view name, const glm::vec4& value )
        {
            for ( auto& p : Params )
                if ( p.Name == name )
                {
                    p.Value = value;
                    return;
                }
            Params.push_back( { std::string( name ), value } );
        }

        // STOP SAYING ANYTHING ABOUT @p name. Returns whether an entry was actually removed.
        //
        // THE PAIR OF SetParam, and erasing is not the same as writing the default in. What this material
        // is silent about is answered by whoever reads it — the shader's `Properties … = 0.45` for a base
        // material, the parent chain for an instance (MaterialService::ResolveOverrides) — and that answer
        // is resolved at READ time. Writing the default in would freeze today's answer into the file, so a
        // later edit to the parent, or to the shader, would stop reaching this material. That is a pin, and
        // it is the opposite of a reset.
        //
        // Order of the surviving entries is preserved: MaterialData::Params is compared by NAME
        // (MaterialEdit::AuthoredValuesEqual), so order carries no meaning — but a reorder here would show
        // up as a spurious diff in every `.demat` the editor rewrites.
        bool RemoveParam( std::string_view name )
        {
            for ( auto it = Params.begin(); it != Params.end(); ++it )
                if ( it->Name == name )
                {
                    Params.erase( it );
                    return true;
                }
            return false;
        }

        uint64_t GetTexture( std::string_view name ) const
        {
            for ( const auto& t : Textures )
                if ( t.Name == name )
                    return t.TextureHandle;
            return 0;
        }

        void SetTexture( std::string_view name, uint64_t handle )
        {
            for ( auto& t : Textures )
                if ( t.Name == name )
                {
                    t.TextureHandle = handle;
                    return;
                }
            Textures.push_back( { std::string( name ), handle } );
        }
    };
} // namespace Desert::Assets
