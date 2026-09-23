// Asks THIS machine's Vulkan implementation the question bcprobe.c did NOT ask.
//
// bcprobe.c read vkGetPhysicalDeviceFeatures().textureCompressionBC and the FORMAT PROPERTIES of the
// BC formats. Both are properties of the PHYSICAL device: they say what the driver COULD do. Neither
// says the logical device we create has the feature ENABLED, and neither creates an image, puts bytes
// in it, or reads a texel back out.
//
// This one does the whole chain: enable the feature on the logical device -> create an 8x8
// VK_FORMAT_BC7_UNORM_BLOCK image (2x2 blocks, four different colours) -> upload 64 bytes of real BC7
// through a staging buffer with vkCmdCopyBufferToImage -> sample every texel from a compute shader
// with texelFetch -> read the decoded RGBA back to the CPU and compare against what the encoder put in.
//
// The blocks are hand-built BC7 MODE 6 blocks with both endpoints equal, so every one of the 16 texels
// in a block decodes to the same colour and the index bits cannot matter. That makes a wrong readback
// unambiguous: it is the upload or the sampler, not an interpolation subtlety.
//
// Usage:
//   bc7upload            enable textureCompressionBC (the thing under test)
//   bc7upload --disabled NEGATIVE CONTROL: create the device WITHOUT the feature and do the same run
//
// Build: scripts/Probes/bc7/build.sh (macOS/Linux) or scripts/Probes/bc7/build.bat (Windows). Both do
// the same two steps -- glslc the compute shader to a C array, then compile this file against the
// Vulkan loader. It is deliberately a standalone C program with no engine dependency, so it can be
// carried to a Windows machine and run there without building Desert first.
//
// WHY IT IS COMMITTED RATHER THAN THROWN AWAY. On macOS both runs pass identically: MoltenVK 1.1.357 /
// Apple M1 Pro returns all 64 texels bit-exact WITH and WITHOUT the feature enabled, and validation
// layer 1.4.350.1 reports nothing in either -- the layer was proved live in the same run by a
// deliberate anisotropy error it did catch. So this machine cannot answer whether the enable matters;
// only a driver that enforces it can, and that means running this on Windows. Until somebody does, the
// Windows behaviour is UNVERIFIED and this file is how it stops being so.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static const uint32_t kShader[] =
#include "bc7fetch.spv"
     ;

#define VKC( expr )                                                                                               \
    do                                                                                                            \
    {                                                                                                             \
        VkResult r_ = ( expr );                                                                                   \
        if ( r_ != VK_SUCCESS )                                                                                   \
        {                                                                                                         \
            printf( "FAIL %s -> VkResult %d\n", #expr, (int)r_ );                                                 \
            exit( 2 );                                                                                            \
        }                                                                                                         \
    } while ( 0 )

// ---------------------------------------------------------------------------------------------------
// BC7 mode 6, both endpoints equal. Bit order is LSB-first across the 128-bit block:
//   [0..6]   mode (bit 6 set, bits 0..5 clear)
//   [7..62]  R0 R1 G0 G1 B0 B1 A0 A1, seven bits each (channel-major, endpoint-minor)
//   [63,64]  P0 P1
//   [65..127] 63 index bits
// Each channel's final 8-bit value is (e7 << 1) | p, so with p = 0 only EVEN values are expressible;
// every colour below is even on purpose.
static void put_bits( uint8_t* blk, int* pos, uint32_t val, int n )
{
    for ( int i = 0; i < n; ++i )
    {
        const uint32_t bit = ( val >> i ) & 1u;
        blk[( *pos ) >> 3] |= (uint8_t)( bit << ( ( *pos ) & 7 ) );
        ( *pos )++;
    }
}

static void bc7_mode6_solid( uint8_t out[16], uint8_t r, uint8_t g, uint8_t b, uint8_t a )
{
    memset( out, 0, 16 );
    int p = 0;
    put_bits( out, &p, 0x40u, 7 ); // mode 6
    put_bits( out, &p, r >> 1, 7 );
    put_bits( out, &p, r >> 1, 7 );
    put_bits( out, &p, g >> 1, 7 );
    put_bits( out, &p, g >> 1, 7 );
    put_bits( out, &p, b >> 1, 7 );
    put_bits( out, &p, b >> 1, 7 );
    put_bits( out, &p, a >> 1, 7 );
    put_bits( out, &p, a >> 1, 7 );
    put_bits( out, &p, 0u, 1 ); // P0
    put_bits( out, &p, 0u, 1 ); // P1
    // 63 index bits stay zero -> every texel takes endpoint 0, which equals endpoint 1.
}

static uint32_t mem_type( VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want )
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties( pd, &mp );
    for ( uint32_t i = 0; i < mp.memoryTypeCount; ++i )
        if ( ( bits & ( 1u << i ) ) && ( mp.memoryTypes[i].propertyFlags & want ) == want )
            return i;
    printf( "FAIL no memory type for 0x%x / 0x%x\n", bits, (unsigned)want );
    exit( 2 );
}

static int g_validationHits = 0;

static VKAPI_ATTR VkBool32 VKAPI_CALL dbg_cb( VkDebugUtilsMessageSeverityFlagBitsEXT      sev,
                                              VkDebugUtilsMessageTypeFlagsEXT             types,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void* user )
{
    (void)types;
    (void)user;
    ++g_validationHits;
    const char* tag = ( sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT )     ? "VALIDATION-ERROR"
                      : ( sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ) ? "VALIDATION-WARN"
                                                                                  : "validation-info";
    printf( "  [%s] %s\n", tag, data->pMessage );
    return VK_FALSE;
}

#define W 8
#define H 8
#define BLOCKS_X ( W / 4 )
#define BLOCKS_Y ( H / 4 )
#define TEXELS ( W * H )

int main( int argc, char** argv )
{
    const int enableFeature = !( argc > 1 && strcmp( argv[1], "--disabled" ) == 0 );
    printf( "=== bc7upload: textureCompressionBC will be %s on the logical device ===\n",
            enableFeature ? "ENABLED" : "NOT enabled (negative control)" );

    // --- instance, with validation so a missing feature is not silent ------------------------------
    VkApplicationInfo app = { .sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "bc7upload",
                              .apiVersion       = VK_API_VERSION_1_1 };

    // EVERY INSTANCE EXTENSION IS ASKED FOR ONLY IF THE LOADER HAS IT. VK_KHR_portability_enumeration
    // exists on macOS because MoltenVK is a portability driver and does NOT exist on a Windows loader;
    // naming it unconditionally would fail vkCreateInstance with VK_ERROR_EXTENSION_NOT_PRESENT before
    // this probe ever reached the question it is here to ask.
    const char*           iexts[4];
    uint32_t              iextn  = 0;
    VkInstanceCreateFlags iflags = 0;
    {
        uint32_t en = 0;
        vkEnumerateInstanceExtensionProperties( NULL, &en, NULL );
        VkExtensionProperties* eps = (VkExtensionProperties*)calloc( en ? en : 1, sizeof( *eps ) );
        vkEnumerateInstanceExtensionProperties( NULL, &en, eps );
        for ( uint32_t i = 0; i < en; ++i )
        {
            if ( strcmp( eps[i].extensionName, "VK_KHR_portability_enumeration" ) == 0 )
            {
                iexts[iextn++] = "VK_KHR_portability_enumeration";
                iflags |= 0x00000001; // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
            }
            else if ( strcmp( eps[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME ) == 0 )
            {
                iexts[iextn++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
            }
        }
        free( eps );
    }

    const char*          layers[] = { "VK_LAYER_KHRONOS_validation" };
    VkInstanceCreateInfo ici      = { .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .flags                   = iflags,
                                      .pApplicationInfo        = &app,
                                      .enabledLayerCount       = 1,
                                      .ppEnabledLayerNames     = layers,
                                      .enabledExtensionCount   = iextn,
                                      .ppEnabledExtensionNames = iexts };
    VkInstance           inst;
    if ( vkCreateInstance( &ici, NULL, &inst ) != VK_SUCCESS )
    {
        printf( "  (validation layer unavailable; retrying without it)\n" );
        ici.enabledLayerCount = 0;
        VKC( vkCreateInstance( &ici, NULL, &inst ) );
    }

    PFN_vkCreateDebugUtilsMessengerEXT mk =
         (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr( inst, "vkCreateDebugUtilsMessengerEXT" );
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if ( mk )
    {
        VkDebugUtilsMessengerCreateInfoEXT dci = {
             .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
             .messageSeverity =
                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
             .messageType =
                  VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
             .pfnUserCallback = dbg_cb };
        mk( inst, &dci, NULL, &messenger );
    }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices( inst, &n, NULL );
    if ( !n )
    {
        printf( "FAIL no physical devices\n" );
        return 1;
    }
    VkPhysicalDevice pds[8];
    if ( n > 8 )
        n = 8;
    vkEnumeratePhysicalDevices( inst, &n, pds );
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties( pd, &props );
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures( pd, &supported );
    printf( "device: %s (driver %u, api %u.%u.%u)\n", props.deviceName, props.driverVersion,
            VK_VERSION_MAJOR( props.apiVersion ), VK_VERSION_MINOR( props.apiVersion ),
            VK_VERSION_PATCH( props.apiVersion ) );
    printf( "physical-device SUPPORT  textureCompressionBC = %d   (what bcprobe.c measured)\n",
            supported.textureCompressionBC ? 1 : 0 );

    // --- logical device ----------------------------------------------------------------------------
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties( pd, &qn, NULL );
    VkQueueFamilyProperties qprops[16];
    if ( qn > 16 )
        qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties( pd, &qn, qprops );
    uint32_t family = UINT32_MAX;
    for ( uint32_t i = 0; i < qn; ++i )
        if ( ( qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT ) && ( qprops[i].queueFlags & VK_QUEUE_TRANSFER_BIT ) )
        {
            family = i;
            break;
        }
    if ( family == UINT32_MAX )
    {
        printf( "FAIL no compute+transfer queue family\n" );
        return 1;
    }

    const float              prio  = 1.0f;
    VkDeviceQueueCreateInfo  qci   = { .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueFamilyIndex = family,
                                       .queueCount       = 1,
                                       .pQueuePriorities = &prio };
    VkPhysicalDeviceFeatures feats = { 0 };
    if ( enableFeature )
        feats.textureCompressionBC = VK_TRUE;
    const char* dexts[] = { "VK_KHR_portability_subset" };
    uint32_t    dextn   = 0;
    {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties( pd, NULL, &en, NULL );
        VkExtensionProperties* eps = (VkExtensionProperties*)calloc( en ? en : 1, sizeof( *eps ) );
        vkEnumerateDeviceExtensionProperties( pd, NULL, &en, eps );
        for ( uint32_t i = 0; i < en; ++i )
            if ( strcmp( eps[i].extensionName, "VK_KHR_portability_subset" ) == 0 )
                dextn = 1;
        free( eps );
    }
    VkDeviceCreateInfo dci = { .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount    = 1,
                               .pQueueCreateInfos       = &qci,
                               .enabledExtensionCount   = dextn,
                               .ppEnabledExtensionNames = dexts,
                               .pEnabledFeatures        = &feats };
    VkDevice           dev;
    VKC( vkCreateDevice( pd, &dci, NULL, &dev ) );
    VkQueue queue;
    vkGetDeviceQueue( dev, family, 0, &queue );
    printf( "logical-device ENABLED   textureCompressionBC = %d   (the question under test)\n",
            enableFeature ? 1 : 0 );

    // PROVE THE INSTRUMENT BEFORE TRUSTING ITS SILENCE. samplerAnisotropy is never enabled here, so a
    // sampler that asks for anisotropy is a validation error the layer is known to catch. If this does
    // not fire, the layer is not wired and "no validation error" downstream would mean nothing.
    {
        const int           before = g_validationHits;
        VkSamplerCreateInfo bad    = {
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .anisotropyEnable = VK_TRUE, .maxAnisotropy = 16.0f };
        VkSampler s = VK_NULL_HANDLE;
        printf( "instrument self-test (deliberate validation error):\n" );
        vkCreateSampler( dev, &bad, NULL, &s );
        if ( s != VK_NULL_HANDLE )
            vkDestroySampler( dev, s, NULL );
        printf( "  -> messenger fired %d time(s): %s\n", g_validationHits - before,
                g_validationHits > before ? "layer IS live" : "LAYER IS DEAD, silence below proves nothing" );
        g_validationHits = 0;
    }

    // --- the pixels --------------------------------------------------------------------------------
    // Four blocks, laid out 2x2. Every channel even, so P = 0 expresses it exactly.
    const uint8_t want[4][4] = {
         { 252, 8, 8, 254 }, { 8, 252, 8, 254 }, { 8, 8, 252, 254 }, { 128, 64, 32, 254 } };
    uint8_t blocks[BLOCKS_Y * BLOCKS_X * 16];
    for ( int by = 0; by < BLOCKS_Y; ++by )
        for ( int bx = 0; bx < BLOCKS_X; ++bx )
        {
            const int k = by * BLOCKS_X + bx;
            bc7_mode6_solid( &blocks[k * 16], want[k][0], want[k][1], want[k][2], want[k][3] );
        }
    printf( "upload: %d bytes of BC7 (%dx%d texels = %dx%d blocks x 16 B)\n", (int)sizeof( blocks ), W, H,
            BLOCKS_X, BLOCKS_Y );

    // --- image -------------------------------------------------------------------------------------
    VkImageCreateInfo imgci = { .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType     = VK_IMAGE_TYPE_2D,
                                .format        = VK_FORMAT_BC7_UNORM_BLOCK,
                                .extent        = { W, H, 1 },
                                .mipLevels     = 1,
                                .arrayLayers   = 1,
                                .samples       = VK_SAMPLE_COUNT_1_BIT,
                                .tiling        = VK_IMAGE_TILING_OPTIMAL,
                                .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage           image;
    VkResult          ir = vkCreateImage( dev, &imgci, NULL, &image );
    printf( "vkCreateImage(BC7_UNORM_BLOCK) -> VkResult %d%s\n", (int)ir,
            ir == VK_SUCCESS ? " (created)" : " (REFUSED)" );
    if ( ir != VK_SUCCESS )
        return 3;

    VkMemoryRequirements mreq;
    vkGetImageMemoryRequirements( dev, image, &mreq );
    VkMemoryAllocateInfo mai = { .sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mreq.size,
                                 .memoryTypeIndex =
                                      mem_type( pd, mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ) };
    VkDeviceMemory       imem;
    VKC( vkAllocateMemory( dev, &mai, NULL, &imem ) );
    VKC( vkBindImageMemory( dev, image, imem, 0 ) );

    VkImageViewCreateInfo ivci = { .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                   .image            = image,
                                   .viewType         = VK_IMAGE_VIEW_TYPE_2D,
                                   .format           = VK_FORMAT_BC7_UNORM_BLOCK,
                                   .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView           view;
    VKC( vkCreateImageView( dev, &ivci, NULL, &view ) );

    VkSamplerCreateInfo sci = { .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter    = VK_FILTER_NEAREST,
                                .minFilter    = VK_FILTER_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkSampler           sampler;
    VKC( vkCreateSampler( dev, &sci, NULL, &sampler ) );

    // --- staging + output buffers ------------------------------------------------------------------
    VkBuffer       staging, out;
    VkDeviceMemory smem, omem;
    {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                   .size  = sizeof( blocks ),
                                   .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
        VKC( vkCreateBuffer( dev, &bci, NULL, &staging ) );
        VkMemoryRequirements br;
        vkGetBufferMemoryRequirements( dev, staging, &br );
        VkMemoryAllocateInfo ai = { .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                    .allocationSize  = br.size,
                                    .memoryTypeIndex = mem_type( pd, br.memoryTypeBits,
                                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ) };
        VKC( vkAllocateMemory( dev, &ai, NULL, &smem ) );
        VKC( vkBindBufferMemory( dev, staging, smem, 0 ) );
        void* p = NULL;
        VKC( vkMapMemory( dev, smem, 0, VK_WHOLE_SIZE, 0, &p ) );
        memcpy( p, blocks, sizeof( blocks ) );
        vkUnmapMemory( dev, smem );
    }
    {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                   .size  = TEXELS * 4 * sizeof( float ),
                                   .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        VKC( vkCreateBuffer( dev, &bci, NULL, &out ) );
        VkMemoryRequirements br;
        vkGetBufferMemoryRequirements( dev, out, &br );
        VkMemoryAllocateInfo ai = { .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                    .allocationSize  = br.size,
                                    .memoryTypeIndex = mem_type( pd, br.memoryTypeBits,
                                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ) };
        VKC( vkAllocateMemory( dev, &ai, NULL, &omem ) );
        VKC( vkBindBufferMemory( dev, out, omem, 0 ) );
    }

    // --- descriptors + pipeline --------------------------------------------------------------------
    VkDescriptorSetLayoutBinding binds[2] = {
         { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
         { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dslci = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = binds };
    VkDescriptorSetLayout dsl;
    VKC( vkCreateDescriptorSetLayout( dev, &dslci, NULL, &dsl ) );

    VkDescriptorPoolSize       psz[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
                                          { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpci   = { .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .maxSets       = 1,
                                          .poolSizeCount = 2,
                                          .pPoolSizes    = psz };
    VkDescriptorPool           dpool;
    VKC( vkCreateDescriptorPool( dev, &dpci, NULL, &dpool ) );
    VkDescriptorSetAllocateInfo dsai = { .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool     = dpool,
                                         .descriptorSetCount = 1,
                                         .pSetLayouts        = &dsl };
    VkDescriptorSet             dset;
    VKC( vkAllocateDescriptorSets( dev, &dsai, &dset ) );

    VkDescriptorImageInfo  dii   = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorBufferInfo dbi   = { out, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet   wr[2] = { { .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet          = dset,
                                       .dstBinding      = 0,
                                       .descriptorCount = 1,
                                       .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .pImageInfo      = &dii },
                                     { .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet          = dset,
                                       .dstBinding      = 1,
                                       .descriptorCount = 1,
                                       .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo     = &dbi } };
    vkUpdateDescriptorSets( dev, 2, wr, 0, NULL );

    VkShaderModuleCreateInfo smci = {
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof( kShader ), .pCode = kShader };
    VkShaderModule sm;
    VKC( vkCreateShaderModule( dev, &smci, NULL, &sm ) );
    VkPipelineLayoutCreateInfo plci = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout playout;
    VKC( vkCreatePipelineLayout( dev, &plci, NULL, &playout ) );
    VkComputePipelineCreateInfo cpci = { .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                         .stage  = { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
                                                     .module = sm,
                                                     .pName  = "main" },
                                         .layout = playout };
    VkPipeline                  pipe;
    VKC( vkCreateComputePipelines( dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe ) );

    // --- record ------------------------------------------------------------------------------------
    VkCommandPoolCreateInfo cpci2 = { .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .queueFamilyIndex = family };
    VkCommandPool           pool;
    VKC( vkCreateCommandPool( dev, &cpci2, NULL, &pool ) );
    VkCommandBufferAllocateInfo cbai = { .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool        = pool,
                                         .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1 };
    VkCommandBuffer             cb;
    VKC( vkAllocateCommandBuffers( dev, &cbai, &cb ) );
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKC( vkBeginCommandBuffer( cb, &bi ) );

    VkImageMemoryBarrier toDst = { .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .srcAccessMask       = 0,
                                   .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image               = image,
                                   .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                          NULL, 1, &toDst );

    // bufferRowLength / bufferImageHeight are in TEXELS even for a block format; 0 means "tightly
    // packed", which for BC7 means 16 bytes per 4x4 block, BLOCKS_X blocks per row.
    VkBufferImageCopy copy = { .bufferOffset      = 0,
                               .bufferRowLength   = 0,
                               .bufferImageHeight = 0,
                               .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                               .imageOffset       = { 0, 0, 0 },
                               .imageExtent       = { W, H, 1 } };
    vkCmdCopyBufferToImage( cb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy );

    VkImageMemoryBarrier toRead = toDst;
    toRead.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                          NULL, 1, &toRead );

    vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe );
    vkCmdBindDescriptorSets( cb, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &dset, 0, NULL );
    vkCmdDispatch( cb, 1, 1, 1 );
    VkMemoryBarrier host = { .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                             .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0,
                          NULL, 0, NULL );
    VKC( vkEndCommandBuffer( cb ) );

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VKC( vkQueueSubmit( queue, 1, &si, VK_NULL_HANDLE ) );
    VKC( vkQueueWaitIdle( queue ) );

    // --- read back ---------------------------------------------------------------------------------
    float* px = NULL;
    VKC( vkMapMemory( dev, omem, 0, VK_WHOLE_SIZE, 0, (void**)&px ) );

    printf( "\nvalidation messages during the BC7 create/upload/sample chain: %d\n", g_validationHits );
    printf( "\nsampled back (texelFetch, one line per texel row, 8-bit rounded):\n" );
    int bad = 0;
    for ( int y = 0; y < H; ++y )
    {
        printf( "  y=%d ", y );
        for ( int x = 0; x < W; ++x )
        {
            const float* t = &px[( y * W + x ) * 4];
            const int    r = (int)( t[0] * 255.0f + 0.5f ), g = (int)( t[1] * 255.0f + 0.5f );
            const int    b = (int)( t[2] * 255.0f + 0.5f ), a = (int)( t[3] * 255.0f + 0.5f );
            const int    k = ( y / 4 ) * BLOCKS_X + ( x / 4 );
            if ( r != want[k][0] || g != want[k][1] || b != want[k][2] || a != want[k][3] )
                ++bad;
            printf( "(%3d,%3d,%3d,%3d)", r, g, b, a );
        }
        printf( "\n" );
    }
    printf( "\nexpected per block: " );
    for ( int k = 0; k < 4; ++k )
        printf( "[%d]=(%d,%d,%d,%d) ", k, want[k][0], want[k][1], want[k][2], want[k][3] );
    printf( "\nmismatched texels: %d of %d  ->  %s\n", bad, TEXELS,
            bad == 0 ? "BC7 UPLOAD AND SAMPLE VERIFIED" : "MISMATCH" );

    vkUnmapMemory( dev, omem );
    if ( messenger )
    {
        PFN_vkDestroyDebugUtilsMessengerEXT rm =
             (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr( inst, "vkDestroyDebugUtilsMessengerEXT" );
        if ( rm )
            rm( inst, messenger, NULL );
    }
    return bad == 0 ? 0 : 4;
}
