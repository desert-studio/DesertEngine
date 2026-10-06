#include <Engine/Core/GpuBenchmark.hpp>

#include <Engine/Core/Device.hpp>

namespace Desert::Engine
{
    // Every field comes from the device the process created, so the key names exactly the GPU and driver the
    // cached levels were measured on; a field read from anywhere else (a config, a previous run) would let a GPU
    // swap keep the old answer.
    Common::Scalability::BenchmarkCacheKey MakeBenchmarkCacheKey( const Device& device, uint32_t tableVersion )
    {
        const DeviceCapabilities&              caps = device.GetCapabilities();
        Common::Scalability::BenchmarkCacheKey key;
        key.VendorId      = caps.VendorId;
        key.DeviceId      = caps.DeviceId;
        key.DriverVersion = caps.DriverVersion;
        key.DeviceName    = caps.Name;
        key.TableVersion  = tableVersion;
        return key;
    }
} // namespace Desert::Engine
