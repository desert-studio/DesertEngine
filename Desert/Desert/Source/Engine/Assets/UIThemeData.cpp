#include <Engine/Assets/UIThemeData.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets
{
    namespace
    {
        // A colour that is not a number is a quad the batcher fills with nothing and a log with nothing in
        // it. Naming the token and the channel is the whole difference between a two-line fix and an
        // afternoon.
        Common::BoolResultStr FiniteColor( const std::string& name, const glm::vec3& value )
        {
            for ( int c = 0; c < 3; ++c )
            {
                if ( !std::isfinite( value[c] ) )
                    return Common::MakeFormattedError<bool>( "colour '{}' has a non-finite component {} ({})",
                                                             name, c, value[c] );
                // The ceiling is generous rather than 1: a UI glow or a bloom-feeding accent is legitimately
                // above white, and the components' own colour fields have never been clamped either. What is
                // refused is the order of magnitude that can only be a unit mistake.
                if ( value[c] < 0.0f || value[c] > 16.0f )
                    return Common::MakeFormattedError<bool>(
                         "colour '{}' component {} is {}, outside the legal range [0, 16]", name, c, value[c] );
            }
            return BOOLSUCCESS;
        }

        // Duplicate names are refused rather than last-one-wins, because a theme with two `Accent` rows
        // renders as one of them and a reader cannot tell which — the silent-winner defect class, in a
        // table this time.
        Common::BoolResultStr NamesAreUnique( const char* what, const std::vector<std::string>& names )
        {
            std::unordered_set<std::string> seen;
            for ( const auto& n : names )
            {
                if ( n.empty() )
                    return Common::MakeFormattedError<bool>( "a {} has an empty name", what );
                if ( !seen.insert( n ).second )
                    return Common::MakeFormattedError<bool>( "the {} '{}' is declared more than once", what, n );
            }
            return BOOLSUCCESS;
        }

        template <class TRow>
        std::vector<std::string> NamesOf( const std::vector<TRow>& rows )
        {
            std::vector<std::string> names;
            names.reserve( rows.size() );
            for ( const auto& r : rows )
                names.push_back( r.Name );
            return names;
        }

        // The index of @p name in @p names, or npos. Linear because these tables are tens of rows and the
        // lookup happens once per theme load, never in the walk.
        std::size_t IndexOf( const std::vector<std::string>& names, const std::string& name )
        {
            const auto it = std::find( names.begin(), names.end(), name );
            return it == names.end() ? std::string::npos
                                     : static_cast<std::size_t>( std::distance( names.begin(), it ) );
        }
    } // namespace

    Common::BoolResultStr ValidateUIThemeData( const UIThemeData& data )
    {
        const std::vector<std::string> colorNames  = NamesOf( data.Colors );
        const std::vector<std::string> metricNames = NamesOf( data.Metrics );
        const std::vector<std::string> fontNames   = NamesOf( data.Fonts );
        const std::vector<std::string> styleNames  = NamesOf( data.Styles );

        if ( auto r = NamesAreUnique( "colour", colorNames ); !r )
            return r;
        if ( auto r = NamesAreUnique( "metric", metricNames ); !r )
            return r;
        if ( auto r = NamesAreUnique( "font", fontNames ); !r )
            return r;
        if ( auto r = NamesAreUnique( "style", styleNames ); !r )
            return r;

        for ( const auto& c : data.Colors )
        {
            if ( auto r = FiniteColor( c.Name, c.Value ); !r )
                return r;
        }

        // THE OVERLAY MUST OVERLAY SOMETHING. A high-contrast row naming a token the palette does not have
        // is a value that can never be reached — the accessibility switch would appear to do nothing for
        // that one token and there would be no way to see why.
        if ( auto r = NamesAreUnique( "high-contrast colour", NamesOf( data.HighContrastColors ) ); !r )
            return r;
        for ( const auto& c : data.HighContrastColors )
        {
            if ( IndexOf( colorNames, c.Name ) == std::string::npos )
                return Common::MakeFormattedError<bool>(
                     "the high-contrast override '{}' names a colour this theme does not declare", c.Name );
            if ( auto r = FiniteColor( c.Name, c.Value ); !r )
                return r;
        }

        for ( const auto& m : data.Metrics )
        {
            if ( !std::isfinite( m.Value ) )
                return Common::MakeFormattedError<bool>( "metric '{}' is {}, which is not a finite number", m.Name,
                                                         m.Value );
            // Negative is refused rather than clamped: every consumer of a metric is a radius, a width, a
            // padding or a spacing, and all four are lengths. A negative one is answered downstream by a
            // max() nobody can see, which is a value the clamp chose standing in for one somebody typed.
            if ( m.Value < 0.0f || m.Value > 4096.0f )
                return Common::MakeFormattedError<bool>(
                     "metric '{}' is {}, outside the legal range [0, 4096] design px", m.Name, m.Value );
        }

        for ( const auto& f : data.Fonts )
        {
            if ( !std::isfinite( f.Size ) || f.Size < 1.0f || f.Size > 512.0f )
                return Common::MakeFormattedError<bool>(
                     "font '{}' has size {}, outside the legal range [1, 512] design px", f.Name, f.Size );
        }

        for ( const auto& style : data.Styles )
        {
            std::unordered_set<std::string> boundSlots;
            for ( const auto& b : style.Slots )
            {
                const UI::StyleSlot slot = UI::StyleSlotFromName( b.Slot );
                if ( slot == UI::StyleSlot::Count )
                    return Common::MakeFormattedError<bool>(
                         "the style '{}' binds '{}', which is not a slot this engine draws", style.Name, b.Slot );

                // The same slot twice in one style is the duplicate-name defect one level down, and it is
                // worth its own message because the two rows may be far apart in the file.
                if ( !boundSlots.insert( b.Slot ).second )
                    return Common::MakeFormattedError<bool>( "the style '{}' binds '{}' more than once",
                                                             style.Name, b.Slot );

                if ( b.Token.empty() )
                    return Common::MakeFormattedError<bool>( "the style '{}' binds '{}' to an empty token",
                                                             style.Name, b.Slot );

                // THE TOKEN MUST BE IN THE TABLE THE SLOT'S KIND NAMES. A colour bound to a metric name
                // would otherwise resolve to nothing, and the element would draw its local colour while
                // the file says it is themed — the picture and the file disagreeing with nobody told.
                const char*                     table = nullptr;
                const std::vector<std::string>* names = nullptr;
                switch ( UI::StyleSlotKindOf( slot ) )
                {
                    case UI::StyleSlotKind::Color:
                        table = "colour";
                        names = &colorNames;
                        break;
                    case UI::StyleSlotKind::Metric:
                        table = "metric";
                        names = &metricNames;
                        break;
                    case UI::StyleSlotKind::Font:
                        table = "font";
                        names = &fontNames;
                        break;
                }

                if ( IndexOf( *names, b.Token ) == std::string::npos )
                    return Common::MakeFormattedError<bool>(
                         "the style '{}' binds '{}' to the {} '{}', which this theme does not declare", style.Name,
                         b.Slot, table, b.Token );
            }
        }

        return BOOLSUCCESS;
    }

    Common::ResultStr<UIThemeData> ParseUITheme( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<UIThemeData>( "the file is empty" );

        // THE VERSION IS READ FIRST, ON ITS OWN. A file from another format fails a full parse with a
        // message about whichever field happens to be missing — true, and useless: what the reader needs to
        // be told is that the FORMAT moved. Read as an untyped tree rather than into a header struct,
        // because a struct imposes the rest of the schema on a document whose whole problem may be that it
        // does not match the schema.
        if ( const auto tree = rfl::json::read<rfl::Generic>( text ); tree )
        {
            if ( const auto fields = tree.value().to_object(); fields )
            {
                if ( const auto stated = fields.value().get( "FormatVersion" ); stated.has_value() )
                {
                    const auto number = stated.value().to_int();
                    if ( number.has_value() && number.value() != kUIThemeFormatVersion )
                        return Common::MakeFormattedError<UIThemeData>(
                             "format version {} was written by a different build; this one reads version {}",
                             number.value(), kUIThemeFormatVersion );
                }
            }
        }

        const auto parsed = rfl::json::read<UIThemeData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<UIThemeData>( "{}", parsed.error().what() );

        UIThemeData data = parsed.value();

        const int32_t version = data.FormatVersion.value_or( kUIThemeFormatVersion );
        if ( version != kUIThemeFormatVersion )
            return Common::MakeFormattedError<UIThemeData>(
                 "format version {} was written by a different build; this one reads version {}", version,
                 kUIThemeFormatVersion );

        if ( auto valid = ValidateUIThemeData( data ); !valid )
            return Common::MakeFormattedError<UIThemeData>( "{}", valid.GetError() );

        data.FormatVersion = kUIThemeFormatVersion;
        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteUITheme( const UIThemeData& data )
    {
        UIThemeData out   = data;
        out.FormatVersion = kUIThemeFormatVersion;
        return rfl::json::write( out, YYJSON_WRITE_PRETTY );
    }

    Common::ResultStr<UIThemeRuntime>
    BuildUIThemeRuntime( const UIThemeData& data, std::string_view name,
                         const std::unordered_map<std::string, AssetHandle>& fontHandles )
    {
        if ( auto valid = ValidateUIThemeData( data ); !valid )
            return Common::MakeFormattedError<UIThemeRuntime>( "{}", valid.GetError() );

        UIThemeRuntime runtime;
        runtime.Name = data.DisplayName.value_or( std::string( name ) );

        runtime.ColorNames = NamesOf( data.Colors );
        runtime.Colors.reserve( data.Colors.size() );
        for ( const auto& c : data.Colors )
            runtime.Colors.push_back( c.Value );

        runtime.HighContrast.assign( runtime.Colors.size(), std::nullopt );
        for ( const auto& c : data.HighContrastColors )
        {
            // Validate has already established the name is in the palette, so this cannot be npos. It is
            // looked up rather than assumed to be in file order, because the overlay is sparse.
            runtime.HighContrast[IndexOf( runtime.ColorNames, c.Name )] = c.Value;
        }

        runtime.MetricNames = NamesOf( data.Metrics );
        runtime.Metrics.reserve( data.Metrics.size() );
        for ( const auto& m : data.Metrics )
            runtime.Metrics.push_back( m.Value );

        runtime.FontNames = NamesOf( data.Fonts );
        runtime.Fonts.reserve( data.Fonts.size() );
        for ( const auto& f : data.Fonts )
        {
            // A name the caller could not resolve keeps a null handle, which every consumer already reads
            // as "the built-in face". The caller says so with the path in the message; this function has no
            // path to name and must not invent a refusal the theme itself does not warrant.
            const auto  it     = fontHandles.find( f.Name );
            AssetHandle handle = it == fontHandles.end() ? AssetHandle::Null() : it->second;
            runtime.Fonts.push_back( UIThemeResolvedFont{ handle, f.Size } );
        }

        for ( const auto& style : data.Styles )
        {
            UIThemeStyleTable table;
            table.Slots.fill( kUIThemeUnbound );

            for ( const auto& b : style.Slots )
            {
                const UI::StyleSlot slot  = UI::StyleSlotFromName( b.Slot );
                std::size_t         index = std::string::npos;
                switch ( UI::StyleSlotKindOf( slot ) )
                {
                    case UI::StyleSlotKind::Color:
                        index = IndexOf( runtime.ColorNames, b.Token );
                        break;
                    case UI::StyleSlotKind::Metric:
                        index = IndexOf( runtime.MetricNames, b.Token );
                        break;
                    case UI::StyleSlotKind::Font:
                        index = IndexOf( runtime.FontNames, b.Token );
                        break;
                }

                table.Slots[static_cast<std::size_t>( slot )] = static_cast<uint16_t>( index );
            }

            runtime.Styles.emplace( style.Name, table );
        }

        return Common::MakeSuccess( std::move( runtime ) );
    }
} // namespace Desert::Assets
