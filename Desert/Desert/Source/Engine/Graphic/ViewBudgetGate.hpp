#pragma once

#include <Common/Core/ResultWithCodes.hpp>
#include <Engine/Core/ViewBudget.hpp>
#include <Engine/Graphic/ViewMemory.hpp>

#include <cstdint>
#include <string_view>

namespace Desert::Graphic
{
    /// `--view-budget-mib`: the ceiling every view is checked against, in MiB, replacing the driver's.
    /// 0 clears it. Read by ReadViewBudget and nothing else; it exists so a small-device refusal can be
    /// reproduced on a machine with a large one.
    void SetViewBudgetOverrideMiB( uint64_t mib );

    /// The device's current reading (driver budget, or heap size when the driver will not say, or the
    /// override), with the open views' holdings standing in for usage when the driver gives none. The
    /// first call logs the ceiling and its source once, so an unknown budget is never silent.
    [[nodiscard]] Engine::ViewBudget::Reading ReadViewBudget();

    /**
     * @brief Asked BEFORE a SceneRenderer is built: may @p who create a view of @p profile at @p extent?
     *
     * The request is the view's target forecast (ViewTargetCensus); a Background demand also keeps the
     * main view's forecast at the current window size free. A refusal is an error whose text names the
     * requested bytes, the ceiling and its source, the usage and every open view's holding.
     */
    [[nodiscard]] Common::BoolResultStr MayCreateView( Engine::ViewBudget::Demand who, std::string_view viewName,
                                                       const ViewProfile& profile, const ViewExtent& extent );

    /**
     * @brief Asked BEFORE an open view rebuilds its targets at @p to: does the growth over @p from fit?
     *
     * A refusal names the view, both sizes, the growth, the ceiling and its source, the usage and every open
     * view's holding; the caller keeps its old targets (Engine::ViewBudget::MayResize).
     */
    [[nodiscard]] Common::BoolResultStr MayResizeView( std::string_view viewName, const ViewProfile& profile,
                                                       const ViewExtent& from, const ViewExtent& to );
} // namespace Desert::Graphic
