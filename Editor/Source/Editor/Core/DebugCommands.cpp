#include "DebugCommands.hpp"

#include <Common/Core/CrashHandler.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>

#include <memory>

namespace Desert::Editor
{
    void AppendCrashCommand( std::vector<PaletteCommand>& commands )
    {
        // DELIBERATE CRASH. It is here and not behind a build flag because the thing it proves — that a
        // crash leaves a report on the machine it happened on — has to be provable on a developer's or a
        // QA machine with the editor they are already running, not only on a build that was compiled for
        // the purpose. It refuses when the handler is not installed, rather than killing the process and
        // leaving nothing: that refusal IS the useful answer.
        commands.push_back( { "Debug", "Crash (test)", []
                              {
                                  if ( !Common::Crash::IsInstalled() )
                                  {
                                      return Common::MakeFormattedError(
                                           "the crash handler is not installed in this process, so a "
                                           "deliberate crash would leave no report" );
                                  }
                                  Common::Crash::TriggerTestCrash( Common::Crash::TestKind::Segv );
                              } } );
    }

    void AppendGpuAllocationCommand( std::vector<PaletteCommand>& commands )
    {
        // THE ALLOCATOR'S OWN CENSUS, for the leak no view ledger can see: device usage that grows while every
        // view's HeldBytes stays flat (RT2k). One line per tag, so a before/after pair diffs to the culprit.
        commands.push_back(
             { "Debug", "Log GPU allocations by tag", []() -> Common::BoolResultStr
               {
                   const auto context = std::dynamic_pointer_cast<Graphic::API::Vulkan::VulkanContext>(
                        EngineContext::GetInstance().GetRendererContext() );
                   if ( !context || !context->GetVulkanAllocator() )
                       return Common::MakeError<bool>(
                            "the renderer is not Vulkan; there is no allocator to read." );
                   const auto& ledger = context->GetVulkanAllocator()->Ledger();
                   LOG_INFO( "[AllocLedger] {} live allocation(s), {:.2f} MiB", ledger.LiveCount(),
                             static_cast<double>( ledger.LiveBytes() ) / ( 1024.0 * 1024.0 ) );
                   for ( const auto& row : ledger.ByTag() )
                       LOG_INFO( "[AllocLedger] tag '{}': {} x, {} B", row.Tag, row.Count, row.Bytes );
                   return PaletteCommandDone();
               } } );
    }
} // namespace Desert::Editor
