#include <Common/Settings/Scalability.hpp>

#include <Common/Core/Logger.hpp>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <format>
#include <iterator>

namespace Common::Scalability
{
    namespace
    {
        using P  = Parameter;
        using G  = Group;
        using CL = CatalogList;
        constexpr ParameterValue kRtMax = static_cast<ParameterValue>( RayTracingMode::RayTracingPipeline );

        // THE spec list (Scalability.hpp: one row per Parameter, in enum order - the census asserts it).
        // The three shadow rows are the values that used to be the renderer constant kSceneShadowQuality
        // (ShadowCascades.hpp); S2 moves the reader onto ResolvedQuality.
        constexpr std::array<ParameterSpec, kParameterCount> kParameterSpecs{ {
             { P::AntiAliasingMethod, G::AntiAliasing, "AntiAliasing.Method", 0,
               static_cast<ParameterValue>( AntiAliasingMethod::DLAA ), CL::AntiAliasingMethods,
               "SceneRenderer framebuffer setup + post AA pass" },
             { P::AntiAliasingSamples, G::AntiAliasing, "AntiAliasing.Samples", 1, 8, CL::MSAACounts,
               "SceneRenderer framebuffer sample count (MSAA)" },
             { P::RenderScalePercent, G::ResolutionScale, "Resolution.Percent", 50, 200, CL::RenderScale,
               "RDG view-target size + upscaler pass" },
             { P::Upscaler, G::ResolutionScale, "Resolution.Upscaler", 0,
               static_cast<ParameterValue>( Upscaler::MetalFX ), CL::Upscalers, "upscaler pass" },
             { P::TextureFilter, G::Filtering, "Filtering.Texture", 0, 3, CL::None,
               "sampler cache (RenderConfig push)" },
             { P::Anisotropy, G::Filtering, "Filtering.Anisotropy", 1, 16, CL::AnisotropyLevels,
               "sampler cache (RenderConfig push)" },
             { P::MeshLOD, G::ViewDistance, "ViewDistance.MeshLOD", 0, 1, CL::None, "mesh LOD selection" },
             { P::CloudQuality, G::Effects, "Effects.CloudQuality", 0, 2, CL::None, "Graphic::CloudQualityScale" },
             { P::ShadowCascades, G::Shadows, "Shadows.Cascades", 1, 4, CL::None,
               "MeshRenderer scene-view ShadowQuality::CascadeCount" },
             { P::ShadowMapSize, G::Shadows, "Shadows.MapSize", 512, 4096, CL::None,
               "MeshRenderer scene-view ShadowQuality::ShadowMapSize" },
             { P::ShadowDistance, G::Shadows, "Shadows.Distance", 1000, 100000, CL::None,
               "MeshRenderer scene-view ShadowQuality::MaxDistance (cm)" },
             { P::TextureMipBias, G::Textures, "Textures.MipBias", -200, 400, CL::None, std::nullopt },
             { P::TextureStreamingPoolMiB, G::Textures, "Textures.StreamingPoolMiB", 256, 16384, CL::None,
               std::nullopt },
             { P::ShadowRayTracing, G::Shadows, "Shadows.RayTracing", 0, kRtMax, CL::RayTracingModes, std::nullopt },
             { P::GlobalIlluminationRayTracing, G::GlobalIllumination, "GlobalIllumination.RayTracing", 0, kRtMax,
               CL::RayTracingModes, std::nullopt },
             { P::ReflectionRayTracing, G::Reflections, "Reflections.RayTracing", 0, kRtMax, CL::RayTracingModes,
               std::nullopt },
             { P::AmbientOcclusionQuality, G::PostProcess, "PostProcess.AmbientOcclusion", 0, 3, CL::None,
               std::nullopt },
             { P::BloomQuality, G::PostProcess, "PostProcess.Bloom", 0, 3, CL::None, std::nullopt },
             { P::TemporalAAQuality, G::AntiAliasing, "AntiAliasing.TemporalQuality", 0, 3, CL::None, std::nullopt },
             { P::UpscalerSharpness, G::ResolutionScale, "Resolution.Sharpness", 0, 100, CL::None, std::nullopt },
        } };
    } // namespace

    namespace
    {
        // Enum-valued parameters are written by NAME in Scalability.yaml; this is the one place a parameter is
        // tied to its name list (CapabilityCatalog.hpp owns the lists).
        std::span<const std::string_view> ValueNames( Parameter parameter )
        {
            switch ( SpecOf( parameter ).NarrowedBy )
            {
                case CatalogList::AntiAliasingMethods:
                    return kAntiAliasingMethodNames;
                case CatalogList::Upscalers:
                    return kUpscalerNames;
                case CatalogList::RayTracingModes:
                    return kRayTracingModeNames;
                case CatalogList::None:
                case CatalogList::MSAACounts:
                case CatalogList::RenderScale:
                case CatalogList::AnisotropyLevels:
                    break;
            }
            return {};
        }

        const ParameterSpec* FindSpec( std::string_view key )
        {
            for ( const ParameterSpec& spec : ParameterSpecs() )
                if ( spec.Key == key )
                    return &spec;
            return nullptr;
        }

        bool GroupHasParameters( Group group )
        {
            return std::any_of( ParameterSpecs().begin(), ParameterSpecs().end(),
                                [group]( const ParameterSpec& spec ) { return spec.Owner == group; } );
        }

        std::optional<Group> FindGroup( std::string_view key )
        {
            for ( std::size_t g = 0; g < kGroupCount; ++g )
                if ( GroupKey( static_cast<Group>( g ) ) == key )
                    return static_cast<Group>( g );
            return std::nullopt;
        }

        std::optional<Level> FindLevel( std::string_view key )
        {
            for ( std::size_t l = 0; l < kLevelCount; ++l )
                if ( LevelKey( static_cast<Level>( l ) ) == key )
                    return static_cast<Level>( l );
            return std::nullopt;
        }

        // Every loader error names the line and the key, and all of them are reported together.
        class Errors
        {
        public:
            template <typename... Args>
            void Add( const YAML::Node& at, std::format_string<Args...> format, Args&&... args )
            {
                std::format_to( std::back_inserter( m_Text ), "{}line {}: ", m_Text.empty() ? "" : "\n",
                                at.Mark().line + 1 );
                std::format_to( std::back_inserter( m_Text ), format, std::forward<Args>( args )... );
            }
            [[nodiscard]] bool Empty() const
            {
                return m_Text.empty();
            }
            [[nodiscard]] const std::string& Text() const
            {
                return m_Text;
            }

        private:
            std::string m_Text;
        };

        std::optional<ParameterValue> ParseValue( const ParameterSpec& spec, const YAML::Node& node,
                                                  Errors& errors )
        {
            if ( !node.IsScalar() )
            {
                errors.Add( node, "{}: expected a value", spec.Key );
                return std::nullopt;
            }
            const std::string                       text  = node.Scalar();
            const std::span<const std::string_view> names = ValueNames( spec.Id );
            ParameterValue                          value = 0;
            if ( !names.empty() )
            {
                const auto it = std::find( names.begin(), names.end(), text );
                if ( it == names.end() )
                {
                    errors.Add( node, "{}: '{}' is not a value of this parameter", spec.Key, text );
                    return std::nullopt;
                }
                value = static_cast<ParameterValue>( it - names.begin() );
            }
            else if ( !YAML::convert<ParameterValue>::decode( node, value ) )
            {
                errors.Add( node, "{}: '{}' is not an integer", spec.Key, text );
                return std::nullopt;
            }
            if ( value < spec.Min || value > spec.Max )
            {
                errors.Add( node, "{}: {} is outside [{}, {}]", spec.Key, value, spec.Min, spec.Max );
                return std::nullopt;
            }
            return value;
        }

        void ParseGroups( const YAML::Node& groups, ScalabilityTable& table, Errors& errors )
        {
            std::array<bool, kGroupCount> seen{};
            for ( const auto& groupEntry : groups )
            {
                const std::string          groupKey = groupEntry.first.Scalar();
                const std::optional<Group> group    = FindGroup( groupKey );
                if ( !group )
                {
                    errors.Add( groupEntry.first, "unknown group '{}'", groupKey );
                    continue;
                }
                const std::size_t g = static_cast<std::size_t>( *group );
                seen[g]             = true;
                if ( !GroupHasParameters( *group ) )
                {
                    errors.Add( groupEntry.first, "group '{}' has no parameters; levels for it would move nothing",
                                groupKey );
                    continue;
                }
                std::array<bool, kLevelCount> levelSeen{};
                for ( const auto& levelEntry : groupEntry.second )
                {
                    const std::string          levelKey = levelEntry.first.Scalar();
                    const std::optional<Level> level    = FindLevel( levelKey );
                    if ( !level )
                    {
                        errors.Add( levelEntry.first, "{}: unknown level '{}'", groupKey, levelKey );
                        continue;
                    }
                    const std::size_t l = static_cast<std::size_t>( *level );
                    levelSeen[l]        = true;
                    std::array<bool, kParameterCount> set{};
                    for ( const auto& valueEntry : levelEntry.second )
                    {
                        const std::string    key  = valueEntry.first.Scalar();
                        const ParameterSpec* spec = FindSpec( key );
                        if ( !spec )
                        {
                            errors.Add( valueEntry.first, "{}.{}: unknown parameter '{}'", groupKey, levelKey,
                                        key );
                            continue;
                        }
                        if ( spec->Owner != *group )
                        {
                            errors.Add( valueEntry.first, "{}.{}: '{}' belongs to group '{}'", groupKey, levelKey,
                                        key, GroupKey( spec->Owner ) );
                            continue;
                        }
                        const std::size_t p = static_cast<std::size_t>( spec->Id );
                        set[p]              = true;
                        if ( const auto value = ParseValue( *spec, valueEntry.second, errors ) )
                            table.Values[g][l][p] = *value;
                    }
                    for ( const ParameterSpec& spec : ParameterSpecs() )
                        if ( spec.Owner == *group && !set[static_cast<std::size_t>( spec.Id )] )
                            errors.Add( levelEntry.first, "{}.{}: '{}' is not set", groupKey, levelKey, spec.Key );
                }
                for ( std::size_t l = 0; l < kLevelCount; ++l )
                    if ( !levelSeen[l] )
                        errors.Add( groupEntry.first, "{}: level '{}' is missing", groupKey,
                                    LevelKey( static_cast<Level>( l ) ) );
            }
            for ( std::size_t g = 0; g < kGroupCount; ++g )
                if ( !seen[g] && GroupHasParameters( static_cast<Group>( g ) ) )
                    errors.Add( groups, "group '{}' has parameters but no levels",
                                GroupKey( static_cast<Group>( g ) ) );
        }

        // A per-group list under Recommend (Thresholds / MinVideoMemoryMiB): known groups that have parameters,
        // exactly N numbers each.
        template <typename T, std::size_t N>
        void ParsePerGroupList( const YAML::Node& node, std::string_view section,
                                std::array<std::array<T, N>, kGroupCount>& out,
                                std::array<bool, kGroupCount>& seen, Errors& errors )
        {
            for ( const auto& entry : node )
            {
                const std::string          key   = entry.first.Scalar();
                const std::optional<Group> group = FindGroup( key );
                if ( !group || !GroupHasParameters( *group ) )
                {
                    errors.Add( entry.first, "Recommend.{}: '{}' is not a group with parameters", section, key );
                    continue;
                }
                const std::size_t g = static_cast<std::size_t>( *group );
                seen[g]             = true;
                if ( !entry.second.IsSequence() || entry.second.size() != N )
                {
                    errors.Add( entry.second, "Recommend.{}.{}: expected {} numbers", section, key, N );
                    continue;
                }
                for ( std::size_t i = 0; i < N; ++i )
                    if ( !YAML::convert<T>::decode( entry.second[i], out[g][i] ) )
                        errors.Add( entry.second[i], "Recommend.{}.{}[{}]: not a number", section, key, i );
            }
        }

        void ParseRecommend( const YAML::Node& recommend, ScalabilityTable& table, Errors& errors )
        {
            std::array<bool, kGroupCount> thresholdsSeen{};
            std::array<bool, kGroupCount> memorySeen{};
            bool                          deviceClassSeen = false;
            for ( const auto& entry : recommend )
            {
                const std::string key = entry.first.Scalar();
                if ( key == "Thresholds" )
                    ParsePerGroupList( entry.second, key, table.RecommendThresholds, thresholdsSeen, errors );
                else if ( key == "MinVideoMemoryMiB" )
                    ParsePerGroupList( entry.second, key, table.MinVideoMemoryMiB, memorySeen, errors );
                else if ( key == "DeviceClass" )
                {
                    deviceClassSeen = true;
                    constexpr std::array<std::string_view, 4> kClasses{ "Unknown", "Integrated", "AppleUnified",
                                                                        "Discrete" };
                    for ( std::size_t c = 0; c < kClasses.size(); ++c )
                    {
                        const YAML::Node value = entry.second[std::string( kClasses[c] )];
                        if ( !value || !YAML::convert<float>::decode( value, table.DeviceClassPerfIndex[c] ) )
                            errors.Add( entry.second, "Recommend.DeviceClass: '{}' is missing or not a number",
                                        kClasses[c] );
                    }
                    if ( entry.second.size() != kClasses.size() )
                        errors.Add( entry.second, "Recommend.DeviceClass: expected exactly {} entries",
                                    kClasses.size() );
                }
                else
                    errors.Add( entry.first, "Recommend: unknown key '{}'", key );
            }
            if ( !deviceClassSeen )
                errors.Add( recommend, "Recommend.DeviceClass is missing" );
            for ( std::size_t g = 0; g < kGroupCount; ++g )
            {
                if ( !GroupHasParameters( static_cast<Group>( g ) ) )
                    continue;
                if ( !thresholdsSeen[g] )
                    errors.Add( recommend, "Recommend.Thresholds.{} is missing",
                                GroupKey( static_cast<Group>( g ) ) );
                const auto& t = table.RecommendThresholds[g];
                for ( std::size_t i = 1; i < t.size(); ++i )
                    if ( !( t[i - 1] < t[i] ) )
                        errors.Add( recommend, "Recommend.Thresholds.{} is not strictly ascending",
                                    GroupKey( static_cast<Group>( g ) ) );
            }
        }

        // ---- Resolution helpers -----------------------------------------------------------------------------

        // The working state of one Resolve: the requested value per parameter and the first reason each one
        // moved, so the result carries ONE fallback per parameter (requested -> final effective).
        struct Resolution
        {
            ParameterValues                               Requested{};
            ParameterValues                               Values{};
            std::array<std::string_view, kParameterCount> Reasons{};

            ParameterValue Get( Parameter p ) const
            {
                return Values[static_cast<std::size_t>( p )];
            }
            void Set( Parameter p, ParameterValue value, std::string_view reason )
            {
                const std::size_t i = static_cast<std::size_t>( p );
                if ( Values[i] == value )
                    return;
                Values[i] = value;
                if ( Reasons[i].empty() )
                    Reasons[i] = reason;
            }
        };

        constexpr std::string_view kNotOffered = "not offered by this device";

        // Largest offered <= value; the list's front when none is (MSAACounts / AnisotropyLevels are ascending
        // and start at 1, RayTracingModes ascending from None).
        template <typename T>
        ParameterValue LargestOfferedAtMost( const std::vector<T>& list, ParameterValue value )
        {
            std::optional<ParameterValue> best;
            for ( const T& item : list )
            {
                const auto v = static_cast<ParameterValue>( item );
                if ( v <= value && ( !best || v > *best ) )
                    best = v;
            }
            return best ? *best : static_cast<ParameterValue>( list.front() );
        }

        void NarrowByCatalog( const ParameterSpec& spec, const CapabilityCatalog& catalog, Resolution& r )
        {
            const ParameterValue value = r.Get( spec.Id );
            switch ( spec.NarrowedBy )
            {
                case CatalogList::None:
                    return;
                case CatalogList::AntiAliasingMethods:
                {
                    const auto method = static_cast<AntiAliasingMethod>( value );
                    if ( CapabilityCatalog::Offers( catalog.AntiAliasingMethods, method ) )
                        return;
                    // The upscalers' native passes fall back to our temporal AA, MSAA (no count > 1) to FXAA.
                    const AntiAliasingMethod to =
                         method == AntiAliasingMethod::MSAA ? AntiAliasingMethod::FXAA : AntiAliasingMethod::TAA;
                    r.Set( spec.Id, static_cast<ParameterValue>( to ), kNotOffered );
                    return;
                }
                case CatalogList::MSAACounts:
                    r.Set( spec.Id, LargestOfferedAtMost( catalog.MSAACounts, value ), kNotOffered );
                    return;
                case CatalogList::AnisotropyLevels:
                    r.Set( spec.Id, LargestOfferedAtMost( catalog.AnisotropyLevels, value ), kNotOffered );
                    return;
                case CatalogList::RayTracingModes:
                    r.Set( spec.Id, LargestOfferedAtMost( catalog.RayTracingModes, value ), kNotOffered );
                    return;
                case CatalogList::Upscalers:
                {
                    const auto upscaler = static_cast<Upscaler>( value );
                    if ( !CapabilityCatalog::Offers( catalog.Upscalers, upscaler ) )
                        r.Set( spec.Id, static_cast<ParameterValue>( Upscaler::TAAU ), kNotOffered );
                    return;
                }
                case CatalogList::RenderScale:
                    r.Set( spec.Id,
                           std::clamp( value, catalog.RenderScale.MinPercent, catalog.RenderScale.MaxPercent ),
                           kNotOffered );
                    return;
            }
        }

        // ---- QualityState storage --------------------------------------------------------------------------
        struct State
        {
            ScalabilityTable                                      Table;
            CapabilityCatalog                                     Catalog;
            QualitySelection                                      Selection;
            ResolvedQuality                                       Resolved;
            QualityState::Saver                                   Save = nullptr;
            std::vector<std::pair<QualityState::Listener, void*>> Listeners;
            bool                                                  Initialized = false;
        };
        State& S()
        {
            static State state;
            return state;
        }

        void LogUnknownOverrides( const QualitySelection& selection, const QualitySelection* previous )
        {
            for ( const ParameterOverride& o : selection.Overrides )
            {
                if ( FindSpec( o.Key ) )
                    continue;
                const bool known =
                     previous && std::any_of( previous->Overrides.begin(), previous->Overrides.end(),
                                              [&]( const ParameterOverride& p ) { return p.Key == o.Key; } );
                if ( !known )
                    LOG_WARN(
                         "[Scalability] override '{}' = {} names no parameter of this build; it is not applied",
                         o.Key, o.Value );
            }
        }

        // Publish a new resolution: log what is new, bump the generation, notify. Shared by Apply and
        // ReplaceCatalog — the selection does not change on a new device, the resolution may.
        QualityState::ApplyReport Publish( ResolvedQuality resolved )
        {
            State&                    s = S();
            QualityState::ApplyReport report;
            for ( const Fallback& f : resolved.Fallbacks )
                if ( std::find( s.Resolved.Fallbacks.begin(), s.Resolved.Fallbacks.end(), f ) ==
                     s.Resolved.Fallbacks.end() )
                {
                    LOG_WARN( "{}", FormatFallback( f ) );
                    report.NewFallbacks.push_back( f );
                }
            report.ValuesChanged = resolved.Values != s.Resolved.Values || resolved.Scale != s.Resolved.Scale;
            resolved.Generation  = s.Resolved.Generation + ( report.ValuesChanged ? 1 : 0 );
            s.Resolved           = std::move( resolved );
            for ( const auto& [listener, user] : s.Listeners )
                listener( s.Resolved, user );
            return report;
        }

        void EraseOverride( QualitySelection& selection, std::string_view key )
        {
            std::erase_if( selection.Overrides, [key]( const ParameterOverride& o ) { return o.Key == key; } );
        }
    } // namespace

    // ---- Specs -------------------------------------------------------------------------------------------

    std::span<const ParameterSpec> ParameterSpecs()
    {
        return kParameterSpecs;
    }

    const ParameterSpec& SpecOf( Parameter parameter )
    {
        return kParameterSpecs[static_cast<std::size_t>( parameter )];
    }

    bool IsGroupListed( Group group )
    {
        return std::any_of( kParameterSpecs.begin(), kParameterSpecs.end(), [group]( const ParameterSpec& spec )
                            { return spec.Owner == group && !IsPlaceholder( spec ); } );
    }

    std::vector<Parameter> ListedParameters()
    {
        std::vector<Parameter> listed;
        for ( const ParameterSpec& spec : kParameterSpecs )
            if ( !IsPlaceholder( spec ) )
                listed.push_back( spec.Id );
        return listed;
    }

    std::string_view GroupKey( Group group )
    {
        constexpr std::array<std::string_view, kGroupCount> kKeys{
             "Textures",    "Filtering", "Shadows",      "GlobalIllumination", "Reflections",
             "PostProcess", "Effects",   "ViewDistance", "AntiAliasing",       "ResolutionScale" };
        return kKeys[static_cast<std::size_t>( group )];
    }

    std::string_view LevelKey( Level level )
    {
        constexpr std::array<std::string_view, kLevelCount> kKeys{ "Low", "Medium", "High", "Epic", "Cinematic" };
        return kKeys[static_cast<std::size_t>( level )];
    }

    // ---- Table -------------------------------------------------------------------------------------------

    Common::ResultStr<ScalabilityTable> ScalabilityTable::Parse( std::string_view yamlText )
    {
        YAML::Node root;
        try
        {
            root = YAML::Load( std::string( yamlText ) );
        }
        catch ( const YAML::Exception& e )
        {
            return Common::MakeError<ScalabilityTable>(
                 std::format( "Scalability.yaml is not YAML: {}", e.what() ) );
        }

        ScalabilityTable table;
        // Slots a group does not own keep the spec Min (never read: Resolve takes a parameter from its own group).
        for ( auto& group : table.Values )
            for ( auto& level : group )
                for ( const ParameterSpec& spec : kParameterSpecs )
                    level[static_cast<std::size_t>( spec.Id )] = spec.Min;

        Errors errors;
        if ( !root.IsMap() )
            return Common::MakeError<ScalabilityTable>( "Scalability.yaml: the document is not a map" );
        bool versionSeen = false, groupsSeen = false, recommendSeen = false;
        for ( const auto& entry : root )
        {
            const std::string key = entry.first.Scalar();
            if ( key == "Version" )
            {
                versionSeen = true;
                if ( !YAML::convert<uint32_t>::decode( entry.second, table.Version ) || table.Version == 0 )
                    errors.Add( entry.second, "Version: expected a positive integer" );
            }
            else if ( key == "Groups" )
            {
                groupsSeen = true;
                ParseGroups( entry.second, table, errors );
            }
            else if ( key == "Recommend" )
            {
                recommendSeen = true;
                ParseRecommend( entry.second, table, errors );
            }
            else
                errors.Add( entry.first, "unknown top-level key '{}'", key );
        }
        if ( !versionSeen )
            errors.Add( root, "Version is missing" );
        if ( !groupsSeen )
            errors.Add( root, "Groups is missing" );
        if ( !recommendSeen )
            errors.Add( root, "Recommend is missing" );

        if ( !errors.Empty() )
            return Common::MakeError<ScalabilityTable>( errors.Text() );
        return Common::ResultStr<ScalabilityTable>( std::move( table ) );
    }

    ParameterValue ScalabilityTable::ValueAt( Parameter parameter, Level level ) const
    {
        const ParameterSpec& spec = SpecOf( parameter );
        return Values[static_cast<std::size_t>( spec.Owner )][static_cast<std::size_t>( level )]
                     [static_cast<std::size_t>( parameter )];
    }

    // ---- Resolve -----------------------------------------------------------------------------------------

    ResolvedQuality Resolve( const QualitySelection& selection, const ScalabilityTable& table,
                             const CapabilityCatalog& catalog )
    {
        Resolution r;
        // 1. Each parameter from its group's level.
        for ( const ParameterSpec& spec : kParameterSpecs )
            r.Requested[static_cast<std::size_t>( spec.Id )] =
                 table.ValueAt( spec.Id, selection.Levels[static_cast<std::size_t>( spec.Owner )] );
        // 2. Overrides on top; a key no spec has is not applied (QualityState::Apply logs it).
        for ( const ParameterOverride& o : selection.Overrides )
            if ( const ParameterSpec* spec = FindSpec( o.Key ) )
                r.Requested[static_cast<std::size_t>( spec->Id )] = o.Value;
        r.Values = r.Requested;

        // 3. The spec range. 4. The device's list.
        for ( const ParameterSpec& spec : kParameterSpecs )
        {
            r.Set( spec.Id, std::clamp( r.Get( spec.Id ), spec.Min, spec.Max ), "outside the parameter's range" );
            NarrowByCatalog( spec, catalog, r );
        }

        // 5. The two axes are coupled.
        const ParameterValue scale = r.Get( Parameter::RenderScalePercent );
        const auto           up    = static_cast<Upscaler>( r.Get( Parameter::Upscaler ) );
        const auto           aa    = static_cast<AntiAliasingMethod>( r.Get( Parameter::AntiAliasingMethod ) );
        ResolvedQuality      resolved;
        if ( scale < 100 )
        {
            resolved.Scale = ScaleMode::Upscale;
            if ( up == Upscaler::None )
                r.Set( Parameter::Upscaler, static_cast<ParameterValue>( Upscaler::TAAU ),
                       "an upscaler is required below 100 %" );
            const auto upscaler = static_cast<Upscaler>( r.Get( Parameter::Upscaler ) );
            const bool nativePassOfThisUpscaler =
                 ( aa == AntiAliasingMethod::DLAA && upscaler == Upscaler::DLSS ) ||
                 ( aa == AntiAliasingMethod::FSRNative && upscaler == Upscaler::FSR );
            if ( !nativePassOfThisUpscaler )
                r.Set( Parameter::AntiAliasingMethod, static_cast<ParameterValue>( AntiAliasingMethod::TAA ),
                       "the upscaler anti-aliases below 100 %" );
        }
        else
        {
            resolved.Scale = scale == 100 ? ScaleMode::Native : ScaleMode::Supersample;
            if ( up != Upscaler::None )
                r.Set( Parameter::Upscaler, static_cast<ParameterValue>( Upscaler::None ),
                       "upscaler unused at native/supersampled scale" );
            // An upscaler's native-AA pass is its network at exactly output size; supersampled input is not it.
            if ( scale > 100 && ( aa == AntiAliasingMethod::DLAA || aa == AntiAliasingMethod::FSRNative ) )
                r.Set( Parameter::AntiAliasingMethod, static_cast<ParameterValue>( AntiAliasingMethod::TAA ),
                       "an upscaler's native AA runs at 100 % only" );
        }
        if ( scale != 100 && aa == AntiAliasingMethod::MSAA )
            r.Set( Parameter::AntiAliasingMethod, static_cast<ParameterValue>( AntiAliasingMethod::FXAA ),
                   "MSAA renders at native scale only" );

        resolved.Values = r.Values;
        for ( std::size_t i = 0; i < kParameterCount; ++i )
            if ( r.Values[i] != r.Requested[i] )
                resolved.Fallbacks.push_back(
                     { static_cast<Parameter>( i ), r.Requested[i], r.Values[i], r.Reasons[i] } );
        return resolved;
    }

    PathAntiAliasing ResolveAntiAliasingForPath( const ResolvedQuality& resolved, bool pathSupportsMSAA )
    {
        const auto method = resolved.As<AntiAliasingMethod>( Parameter::AntiAliasingMethod );
        switch ( method )
        {
            case AntiAliasingMethod::MSAA:
                if ( pathSupportsMSAA )
                    return { AntiAliasingMethod::MSAA,
                             resolved.As<int>( Parameter::AntiAliasingSamples ),
                             AntiAliasingMethod::None,
                             {} };
                return { AntiAliasingMethod::FXAA, 1, AntiAliasingMethod::FXAA,
                         "this render path cannot multisample (deferred G-buffer)" };
            case AntiAliasingMethod::None:
            case AntiAliasingMethod::FXAA:
            case AntiAliasingMethod::SMAA:
                return { method, 1, method, {} };
            case AntiAliasingMethod::TAA:
            case AntiAliasingMethod::FSRNative:
            case AntiAliasingMethod::DLAA:
                return { method, 1, AntiAliasingMethod::None, {} };
        }
        return { AntiAliasingMethod::FXAA, 1, AntiAliasingMethod::FXAA, "unknown anti-aliasing method" };
    }

    std::string FormatFallback( const Fallback& fallback )
    {
        const ParameterSpec&                    spec  = SpecOf( fallback.Id );
        const std::span<const std::string_view> names = ValueNames( fallback.Id );
        if ( !names.empty() )
            return std::format( "[Scalability] {}: {} -> {} ({})", spec.Key, names[fallback.Requested],
                                names[fallback.Effective], fallback.Reason );
        return std::format( "[Scalability] {}: {} -> {} ({})", spec.Key, fallback.Requested, fallback.Effective,
                            fallback.Reason );
    }

    // ---- Presets -----------------------------------------------------------------------------------------

    std::span<const AntiAliasingPreset> AntiAliasingPresets()
    {
        using M = AntiAliasingMethod;
        using U = Upscaler;
        // Scale figures are the vendors' published mode ratios (Quality 1/1.5, Balanced 1/1.7, Performance 1/2).
        static constexpr AntiAliasingPreset kPresets[] = {
             { "Native.TAA", M::TAA, 1, 100, U::None },
             { "Native.SMAA", M::SMAA, 1, 100, U::None },
             { "Native.FXAA", M::FXAA, 1, 100, U::None },
             { "Native.None", M::None, 1, 100, U::None },
             { "MSAA.2x", M::MSAA, 2, 100, U::None },
             { "MSAA.4x", M::MSAA, 4, 100, U::None },
             { "MSAA.8x", M::MSAA, 8, 100, U::None },
             { "SSAA.150", M::TAA, 1, 150, U::None },
             { "SSAA.200", M::TAA, 1, 200, U::None },
             { "TAAU.Quality", M::TAA, 1, 67, U::TAAU },
             { "TAAU.Balanced", M::TAA, 1, 58, U::TAAU },
             { "TAAU.Performance", M::TAA, 1, 50, U::TAAU },
             { "FSR.NativeAA", M::FSRNative, 1, 100, U::None },
             { "FSR.Quality", M::FSRNative, 1, 67, U::FSR },
             { "FSR.Balanced", M::FSRNative, 1, 58, U::FSR },
             { "FSR.Performance", M::FSRNative, 1, 50, U::FSR },
             { "DLSS.DLAA", M::DLAA, 1, 100, U::None },
             { "DLSS.Quality", M::DLAA, 1, 67, U::DLSS },
             { "DLSS.Balanced", M::DLAA, 1, 58, U::DLSS },
             { "DLSS.Performance", M::DLAA, 1, 50, U::DLSS },
             { "XeSS.Quality", M::TAA, 1, 67, U::XeSS },
             { "XeSS.Balanced", M::TAA, 1, 58, U::XeSS },
             { "XeSS.Performance", M::TAA, 1, 50, U::XeSS },
             { "MetalFX.Quality", M::TAA, 1, 67, U::MetalFX },
             { "MetalFX.Performance", M::TAA, 1, 50, U::MetalFX },
        };
        return kPresets;
    }

    std::vector<ParameterOverride> Decompose( const AntiAliasingPreset& preset )
    {
        return {
             { std::string( SpecOf( Parameter::AntiAliasingMethod ).Key ),
               static_cast<ParameterValue>( preset.Method ) },
             { std::string( SpecOf( Parameter::AntiAliasingSamples ).Key ), preset.Samples },
             { std::string( SpecOf( Parameter::RenderScalePercent ).Key ), preset.RenderScalePercent },
             { std::string( SpecOf( Parameter::Upscaler ).Key ), static_cast<ParameterValue>( preset.Upscale ) } };
    }

    std::vector<AntiAliasingPreset> OfferedPresets( const CapabilityCatalog& catalog )
    {
        // Asked of Resolve itself, so "offered" can never disagree with what the preset resolves to. The four
        // overridden parameters do not read the table, so an empty one serves.
        const ScalabilityTable          anyTable;
        std::vector<AntiAliasingPreset> offered;
        for ( const AntiAliasingPreset& preset : AntiAliasingPresets() )
        {
            QualitySelection selection;
            selection.Overrides       = Decompose( preset );
            const ResolvedQuality r   = Resolve( selection, anyTable, catalog );
            const bool            any = std::any_of( r.Fallbacks.begin(), r.Fallbacks.end(),
                                                     []( const Fallback& f )
                                                     {
                                              return f.Id == Parameter::AntiAliasingMethod ||
                                                     f.Id == Parameter::AntiAliasingSamples ||
                                                     f.Id == Parameter::RenderScalePercent ||
                                                     f.Id == Parameter::Upscaler;
                                          } );
            if ( !any )
                offered.push_back( preset );
        }
        return offered;
    }

    // ---- QualityState ------------------------------------------------------------------------------------

    void QualityState::Initialize( ScalabilityTable table, CapabilityCatalog catalog, QualitySelection saved,
                                   Saver save )
    {
        State& s      = S();
        s.Table       = std::move( table );
        s.Catalog     = std::move( catalog );
        s.Selection   = std::move( saved );
        s.Save        = save;
        s.Resolved    = {};
        s.Initialized = true;
        LogUnknownOverrides( s.Selection, nullptr );
        Publish( Resolve( s.Selection, s.Table, s.Catalog ) );
    }

    void QualityState::ReplaceCatalog( CapabilityCatalog catalog )
    {
        State& s  = S();
        s.Catalog = std::move( catalog );
        Publish( Resolve( s.Selection, s.Table, s.Catalog ) );
    }

    QualityState::ApplyReport QualityState::Apply( const QualitySelection& selection )
    {
        State& s = S();
        if ( !s.Initialized )
        {
            LOG_ERROR(
                 "[Scalability] a quality change arrived before QualityState::Initialize; it was not applied" );
            ApplyReport refused;
            refused.Refused = "QualityState is not initialized";
            return refused;
        }
        LogUnknownOverrides( selection, &s.Selection );
        s.Selection        = selection;
        ApplyReport report = Publish( Resolve( s.Selection, s.Table, s.Catalog ) );
        if ( !s.Save )
        {
            LOG_ERROR( "[Scalability] QualityState was initialized without a saver; the selection applies to this "
                       "session only" );
            return report;
        }
        const auto saved = s.Save( s.Selection );
        report.Saved       = saved.IsSuccess();
        if ( !report.Saved )
            LOG_ERROR( "[Scalability] the quality selection was not saved: {} — it applies to this session only",
                       saved.GetError() );
        return report;
    }

    QualityState::ApplyReport QualityState::SetGroupLevel( Group group, Level level )
    {
        QualitySelection selection                          = S().Selection;
        selection.Levels[static_cast<std::size_t>( group )] = level;
        std::erase_if( selection.Overrides,
                       [group]( const ParameterOverride& o )
                       {
                           const ParameterSpec* spec = FindSpec( o.Key );
                           return spec && spec->Owner == group;
                       } );
        return Apply( selection );
    }

    QualityState::ApplyReport QualityState::SetAllGroups( Level level )
    {
        QualitySelection selection;
        selection.Levels.fill( level );
        return Apply( selection );
    }

    QualityState::ApplyReport QualityState::SetOverride( Parameter parameter, ParameterValue value )
    {
        const ParameterSpec& spec = SpecOf( parameter );
        if ( IsPlaceholder( spec ) )
        {
            // A placeholder has no reader: setting it would be a knob that moves nothing.
            LOG_ERROR( "[Scalability] {} is reserved for a feature this build does not have; not set", spec.Key );
            ApplyReport refused;
            refused.Refused = "the parameter has no reader in this build";
            return refused;
        }
        QualitySelection selection = S().Selection;
        EraseOverride( selection, spec.Key );
        selection.Overrides.push_back( { std::string( spec.Key ), value } );
        return Apply( selection );
    }

    QualityState::ApplyReport QualityState::ClearOverride( Parameter parameter )
    {
        QualitySelection selection = S().Selection;
        EraseOverride( selection, SpecOf( parameter ).Key );
        return Apply( selection );
    }

    QualityState::ApplyReport QualityState::ApplyPreset( const AntiAliasingPreset& preset )
    {
        QualitySelection selection = S().Selection;
        for ( ParameterOverride& o : Decompose( preset ) )
        {
            EraseOverride( selection, o.Key );
            selection.Overrides.push_back( std::move( o ) );
        }
        return Apply( selection );
    }

    QualityState::ApplyReport QualityState::ApplyRecommended( const std::array<Level, kGroupCount>& levels )
    {
        // UE's benchmark sets the group levels and nothing else; overrides belong to a choice the player made,
        // and the host calls this only when there is none (GpuBenchmark.hpp).
        QualitySelection selection;
        selection.Levels = levels;
        return Apply( selection );
    }

    const QualitySelection& QualityState::Selection()
    {
        return S().Selection;
    }
    const ResolvedQuality& QualityState::Resolved()
    {
        return S().Resolved;
    }
    const CapabilityCatalog& QualityState::Catalog()
    {
        return S().Catalog;
    }
    const ScalabilityTable& QualityState::Table()
    {
        return S().Table;
    }

    void QualityState::Subscribe( Listener listener, void* user )
    {
        S().Listeners.emplace_back( listener, user );
    }

    void QualityState::Unsubscribe( Listener listener, void* user )
    {
        std::erase( S().Listeners, std::pair<Listener, void*>{ listener, user } );
    }
} // namespace Common::Scalability
