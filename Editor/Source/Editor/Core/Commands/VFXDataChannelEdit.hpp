#pragma once

/**
 * THE `.dfxch` FIELD-LIST EDITOR'S COMMAND LAYER (VFX-10c): every edit of a channel's field list is one undo step.
 *
 * The editor window (REMAINDER VFX-10c) holds a WORKING copy of the channel's VFXDataChannelData; each gesture -
 * add, remove, rename, retype, move - goes through ApplyChannelFieldEdit, which runs the edit on a copy, refuses
 * it by name when the result is not a valid channel (Serialization::ValidateVFXDataChannelData: one source of what
 * a channel may hold), and only then writes the working copy and pushes ONE CommandHistory entry holding the field
 * list before and after (a channel is a handful of fields: a by-value snapshot, no inverse to get wrong).
 * An edit that changes nothing pushes nothing. EditedObject() is the working copy, so the window's destructor
 * forgets its entries with CommandHistory::DropFor(&working).
 */

#include <Editor/Core/CommandHistory.hpp>

#include <Engine/Assets/Serialization/VFXDataChannel.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    using VFXChannelFields = std::vector<Assets::Serialization::VFXDataChannelField>;

    /// One undo step of a channel's field list: the whole list before and after.
    class VFXDataChannelEditCommand final : public ICommand
    {
    public:
        VFXDataChannelEditCommand( Assets::Serialization::VFXDataChannelData* target, VFXChannelFields before,
                                   VFXChannelFields after, std::string label )
             : m_Target( target ), m_Before( std::move( before ) ), m_After( std::move( after ) ),
               m_Label( std::move( label ) )
        {
        }

        bool Undo() override
        {
            m_Target->Fields = m_Before;
            return true;
        }

        bool Redo() override
        {
            m_Target->Fields = m_After;
            return true;
        }

        [[nodiscard]] const void* EditedObject() const override
        {
            return m_Target;
        }

        std::string GetLabel() const override
        {
            return m_Label;
        }

    private:
        Assets::Serialization::VFXDataChannelData* m_Target;
        VFXChannelFields                           m_Before;
        VFXChannelFields                           m_After;
        std::string                                m_Label;
    };

    /// Runs @p edit on a copy of @p working's fields; a refused edit or an invalid result leaves @p working as it
    /// was and pushes nothing, a valid change is written and pushed to @p history as one entry labelled @p label.
    inline Common::BoolResultStr
    ApplyChannelFieldEdit( Assets::Serialization::VFXDataChannelData& working, CommandHistory& history,
                           const std::string&                                               label,
                           const std::function<Common::BoolResultStr( VFXChannelFields& )>& edit )
    {
        Assets::Serialization::VFXDataChannelData next = working;
        if ( auto done = edit( next.Fields ); !done )
            return Common::MakeFormattedError<bool>( "{}: {}", label, done.GetError() );
        if ( auto valid = Assets::Serialization::ValidateVFXDataChannelData( next ); !valid )
            return Common::MakeFormattedError<bool>( "{}: {}", label, valid.GetError() );
        if ( next.Fields == working.Fields )
            return BOOLSUCCESS;
        VFXChannelFields before = std::move( working.Fields );
        working.Fields          = next.Fields;
        history.PushCommand(
             std::make_unique<VFXDataChannelEditCommand>( &working, std::move( before ), next.Fields, label ) );
        return BOOLSUCCESS;
    }

    /// The first of Field, Field1, Field2... no field of @p fields is called.
    inline std::string FreeChannelFieldName( const VFXChannelFields& fields )
    {
        for ( std::size_t n = 0;; ++n )
        {
            const std::string name = n == 0 ? std::string( "Field" ) : "Field" + std::to_string( n );
            bool              used = false;
            for ( const auto& f : fields )
                used = used || f.Name == name;
            if ( !used )
                return name;
        }
    }

    inline Common::BoolResultStr AddChannelField( Assets::Serialization::VFXDataChannelData&     working,
                                                  CommandHistory&                                history,
                                                  Assets::Serialization::VFXDataChannelFieldType type )
    {
        return ApplyChannelFieldEdit( working, history, "Add channel field",
                                      [type]( VFXChannelFields& f ) -> Common::BoolResultStr
                                      {
                                          f.push_back( { FreeChannelFieldName( f ), type } );
                                          return BOOLSUCCESS;
                                      } );
    }

    inline Common::BoolResultStr RemoveChannelField( Assets::Serialization::VFXDataChannelData& working,
                                                     CommandHistory& history, std::size_t index )
    {
        return ApplyChannelFieldEdit( working, history, "Remove channel field",
                                      [index]( VFXChannelFields& f ) -> Common::BoolResultStr
                                      {
                                          if ( index >= f.size() )
                                              return Common::MakeFormattedError<bool>( "no field {}", index );
                                          f.erase( f.begin() + static_cast<std::ptrdiff_t>( index ) );
                                          return BOOLSUCCESS;
                                      } );
    }

    inline Common::BoolResultStr RenameChannelField( Assets::Serialization::VFXDataChannelData& working,
                                                     CommandHistory& history, std::size_t index,
                                                     const std::string& name )
    {
        return ApplyChannelFieldEdit( working, history, "Rename channel field",
                                      [index, &name]( VFXChannelFields& f ) -> Common::BoolResultStr
                                      {
                                          if ( index >= f.size() )
                                              return Common::MakeFormattedError<bool>( "no field {}", index );
                                          f[index].Name = name;
                                          return BOOLSUCCESS;
                                      } );
    }

    inline Common::BoolResultStr RetypeChannelField( Assets::Serialization::VFXDataChannelData& working,
                                                     CommandHistory& history, std::size_t index,
                                                     Assets::Serialization::VFXDataChannelFieldType type )
    {
        return ApplyChannelFieldEdit( working, history, "Change channel field type",
                                      [index, type]( VFXChannelFields& f ) -> Common::BoolResultStr
                                      {
                                          if ( index >= f.size() )
                                              return Common::MakeFormattedError<bool>( "no field {}", index );
                                          f[index].Type = type;
                                          return BOOLSUCCESS;
                                      } );
    }

    /// Moves field @p from to position @p to (the order is the entry layout's order).
    inline Common::BoolResultStr MoveChannelField( Assets::Serialization::VFXDataChannelData& working,
                                                   CommandHistory& history, std::size_t from, std::size_t to )
    {
        return ApplyChannelFieldEdit( working, history, "Reorder channel fields",
                                      [from, to]( VFXChannelFields& f ) -> Common::BoolResultStr
                                      {
                                          if ( from >= f.size() || to >= f.size() )
                                              return Common::MakeFormattedError<bool>(
                                                   "cannot move field {} to {} of {}", from, to, f.size() );
                                          auto field = std::move( f[from] );
                                          f.erase( f.begin() + static_cast<std::ptrdiff_t>( from ) );
                                          f.insert( f.begin() + static_cast<std::ptrdiff_t>( to ),
                                                    std::move( field ) );
                                          return BOOLSUCCESS;
                                      } );
    }
} // namespace Desert::Editor
