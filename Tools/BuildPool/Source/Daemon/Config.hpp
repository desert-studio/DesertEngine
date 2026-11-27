// The daemon's private view of <home>/pool.ini beyond LoadConfig (BuildPool/Daemon.hpp): the lines the daemon
// logs for "every config value in effect".
#pragma once

#include "BuildPool/Daemon.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace buildpool::daemon
{
    inline constexpr uint64_t kMiB = 1024ull * 1024ull;

    // One line per value, in pool.ini syntax ("[pool] reserve_mb = 3257"), for daemon.log.
    std::vector<std::string> DescribeConfig( const PoolConfig& config );

    // The class LoadConfig resolved `name` to (empty = the default class); nullptr when pool.ini has no such
    // class.
    const ClassPolicy* FindClass( const PoolConfig& config, std::string_view name ) noexcept;
} // namespace buildpool::daemon
