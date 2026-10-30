#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dxvk_cmdlist.h"
#include "dxvk_device.h"
#include "dxvk_gaze_dot.h"
#include "dxvk_gaze_vrs.h"
#include "dxvk_gaze_source.h"

#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"

namespace dxvk
{

  // Keep worker implementation and source ownership out of the rendering
  // header. Destruction joins the worker before closing its source mapping.
  struct DxvkGazeVrs::Worker
  {
    DxvkGazeSource source;
    struct Generator
    {
      DxvkGazeSource *source;
      bool operator()(const GazeMapRequest &pass, GazeMap &result) const
      { return source->generate(pass, result); }
    };
    GazeMapWorker<dxvk::thread, dxvk::mutex, dxvk::condition_variable, Generator> maps;
    Worker()
    : maps([] { env::setThreadName("dxvk-gaze-map"); },
        Generator{&source}, std::chrono::milliseconds(4)) { }
  };

  static float getEnvFloat(const char *name, float fallback)
  {
    std::string v = env::getEnvVar(name);
    return v.empty() ? fallback : float(std::atof(v.c_str()));
  }

  static uint32_t getFragmentRateCode(VkExtent2D size)
  {
    uint32_t w = 0u;
    uint32_t h = 0u;

    while (size.width > 1u)
    {
      size.width >>= 1;
      w++;
    }

    while (size.height > 1u)
    {
      size.height >>= 1;
      h++;
    }

    return (w << 2) | h;
  }

  bool dxvkGazeVrsEnabled(const DxvkDevice *device)
  {
    static const bool requested = env::getEnvVar("DXVK_GAZE_VRS") == "1";

    return requested && device->features().khrFragmentShadingRate.attachmentFragmentShadingRate;
  }

  DxvkGazeVrs::DxvkGazeVrs(DxvkDevice *device)
      : m_device(device)
  {
    m_r1 = getEnvFloat("DXVK_GAZE_R1", m_r1);
    m_r2 = getEnvFloat("DXVK_GAZE_R2", m_r2);
    m_fullArea = getEnvFloat("DXVK_GAZE_FULL_AREA", m_fullArea);
    m_midArea = getEnvFloat("DXVK_GAZE_MID_AREA", m_midArea);
    m_minSize = uint32_t(getEnvFloat("DXVK_GAZE_MINSIZE", float(m_minSize)));
    m_debug = env::getEnvVar("DXVK_GAZE_DEBUG") == "1";
    m_dot = env::getEnvVar("DXVK_GAZE_DOT") == "1";
    m_dotRadius = int32_t(getEnvFloat("DXVK_GAZE_DOT_RADIUS", float(m_dotRadius)));

    m_onlySamples = uint32_t(getEnvFloat("DXVK_GAZE_SAMPLES", 0.0f));
    m_onlyFormat = uint32_t(getEnvFloat("DXVK_GAZE_FMT", 0.0f));

    m_grid = env::getEnvVar("DXVK_GAZE_FAKE") == "grid";

    // Raw shading rate code for the outer zone: (log2(w) << 2) | log2(h)
    // 5 = 2x2, 6 = 2x4, 9 = 4x2, 10 = 4x4.
    // "auto" uses the largest rate supported by the current pass.
    std::string coarse = env::getEnvVar("DXVK_GAZE_COARSE");

    if (coarse.empty())
      coarse = env::getEnvVar("DXVK_GAZE_MAXRATE");

    if (coarse == "auto")
    {
      m_autoCoarse = true;
    }
    else if (!coarse.empty())
    {
      m_coarse = uint32_t(std::atof(coarse.c_str()));
    }

    std::string forced = env::getEnvVar("DXVK_GAZE_SIZE");
    if (!forced.empty())
      std::sscanf(forced.c_str(), "%ux%u", &m_forceW, &m_forceH);

    m_ok = createResources();
    if (m_ok)
      m_mapWorker = std::make_unique<Worker>();

    Logger::info(str::format("GazeVRS: init ", m_ok ? "ok" : "FAILED",
                             ", double-buffered, texel ", m_texel.width, "x", m_texel.height,
                             ", max tiles ", m_maxTiles.width, "x", m_maxTiles.height,
                             ", r1=", m_r1, " r2=", m_r2, ", full_area=", m_fullArea,
                             ", mid_area=", m_midArea, ", coarse=", coarse,
                             ", async map worker"));
  }

  DxvkGazeVrs::~DxvkGazeVrs()
  {
    m_mapWorker.reset();
  }

  DxvkGazeVrs::Slot::~Slot()
  {
    auto vk = device->vkd();
    if (view) vk->vkDestroyImageView(vk->device(), view, nullptr);
    if (image) vk->vkDestroyImage(vk->device(), image, nullptr);
    if (buffer) vk->vkDestroyBuffer(vk->device(), buffer, nullptr);
    if (imageMem) vk->vkFreeMemory(vk->device(), imageMem, nullptr);
    if (bufferMem) vk->vkFreeMemory(vk->device(), bufferMem, nullptr);
  }

  bool DxvkGazeVrs::createResources()
  {
    auto vk = m_device->vkd();
    const auto &memProps = m_device->adapter()->memoryProperties();

    const auto &fsrProps = m_device->properties().khrFragmentShadingRate;

    if (fsrProps.minFragmentShadingRateAttachmentTexelSize.width && fsrProps.minFragmentShadingRateAttachmentTexelSize.height)
      m_texel = fsrProps.minFragmentShadingRateAttachmentTexelSize;

    // Preserve the 8192-pixel coverage of 512 tiles at 16x16 texels on
    // devices with smaller attachment texels, without reducing the old capacity.
    const uint32_t maxImageSize = m_device->properties().core.properties.limits.maxImageDimension2D;
    m_maxTiles.width = std::min(maxImageSize, std::max(512u, (8192u + m_texel.width - 1u) / m_texel.width));
    m_maxTiles.height = std::min(maxImageSize, std::max(512u, (8192u + m_texel.height - 1u) / m_texel.height));

    auto findType = [&](uint32_t bits, VkMemoryPropertyFlags flags) -> int32_t
    {
      for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
      {
        if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & flags) == flags)
          return int32_t(i);
      }
      return -1;
    };

    for (auto &slot : m_slots)
    {
      slot = new Slot(m_device);
      VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imageInfo.imageType = VK_IMAGE_TYPE_2D;
      imageInfo.format = VK_FORMAT_R8_UINT;
      imageInfo.extent = {m_maxTiles.width, m_maxTiles.height, 1u};
      imageInfo.mipLevels = 1u;
      imageInfo.arrayLayers = MaxLayers;
      imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imageInfo.usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

      if (vk->vkCreateImage(vk->device(), &imageInfo, nullptr, &slot->image))
        return false;

      VkMemoryRequirements req = {};
      vk->vkGetImageMemoryRequirements(vk->device(), slot->image, &req);

      int32_t type = findType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (type < 0)
        return false;

      VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocInfo.allocationSize = req.size;
      allocInfo.memoryTypeIndex = uint32_t(type);

      if (vk->vkAllocateMemory(vk->device(), &allocInfo, nullptr, &slot->imageMem) || vk->vkBindImageMemory(vk->device(), slot->image, slot->imageMem, 0))
        return false;

      VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = slot->image;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      viewInfo.format = VK_FORMAT_R8_UINT;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, MaxLayers};

      if (vk->vkCreateImageView(vk->device(), &viewInfo, nullptr, &slot->view))
        return false;

      VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      // Each layer starts at a four-byte aligned offset in the upload buffer.
      const VkDeviceSize layerStride = (VkDeviceSize(m_maxTiles.width) * m_maxTiles.height + 3u) & ~VkDeviceSize(3u);
      bufferInfo.size = layerStride * MaxLayers;
      bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

      if (vk->vkCreateBuffer(vk->device(), &bufferInfo, nullptr, &slot->buffer))
        return false;

      vk->vkGetBufferMemoryRequirements(vk->device(), slot->buffer, &req);
      type = findType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (type < 0)
        return false;

      allocInfo.allocationSize = req.size;
      allocInfo.memoryTypeIndex = uint32_t(type);

      if (vk->vkAllocateMemory(vk->device(), &allocInfo, nullptr, &slot->bufferMem) || vk->vkBindBufferMemory(vk->device(), slot->buffer, slot->bufferMem, 0))
        return false;
    }

    return true;
  }

  bool DxvkGazeVrs::prepare(
      DxvkCommandList *cmd,
      VkExtent2D extent,
      uint32_t layers,
      VkFormat colorFormat,
      VkSampleCountFlagBits samples,
      VkRenderingFragmentShadingRateAttachmentInfoKHR &info)
  {
    // Supported eye image layouts:
    //  - array: 2 layers, one eye per layer
    //  - sbs:   1 layer, both eyes side by side (left half / right half)
    bool candidate = false;
    bool sbs = false;

    if (m_ok)
    {
      if (m_forceW)
      {
        candidate = extent.width == m_forceW && extent.height == m_forceH && layers <= MaxLayers;
        sbs = candidate && layers == 1u && extent.width * 10u >= extent.height * 18u;
      }
      else if (layers == MaxLayers)
      {
        candidate = extent.width >= m_minSize && extent.height >= m_minSize;
      }
      else if (layers == 1u)
      {
        sbs = extent.width * 10u >= extent.height * 18u && extent.height >= m_minSize;
        candidate = sbs;
      }
    }

    // Depth-only passes have no color target: no dots, and VRS there would desync depth from the color pass
    if (colorFormat == VK_FORMAT_UNDEFINED)
      candidate = false;

    // Optional pass filters, to pick the scene pass out of several same-sized passes
    if (candidate && m_onlySamples && uint32_t(samples) != m_onlySamples)
      candidate = false;

    if (candidate && m_onlyFormat && uint32_t(colorFormat) != m_onlyFormat)
      candidate = false;

    auto now = std::chrono::steady_clock::now();

    if (m_debug)
    {
      m_shapes[{extent.width, extent.height, layers, uint32_t(colorFormat), uint32_t(samples)}]++;

      if (now >= m_nextSummary)
      {
        if (m_nextSummary.time_since_epoch().count() != 0)
        {
          Logger::info("GazeVRS: --- passes in last 5s: WxH layers fmt samples count ---");
          for (const auto &e : m_shapes)
          {
            if (e.second < 10u)
              continue;
            Logger::info(str::format("GazeVRS:   ", e.first[0], "x", e.first[1],
                                     " layers=", e.first[2], " fmt=", e.first[3],
                                     " samples=", e.first[4], " count=", e.second));
          }
        }
        m_shapes.clear();
        m_nextSummary = now + std::chrono::seconds(5);
      }
    }

    if (!candidate)
      return false;

    uint32_t coarse = m_coarse;

    if (m_autoCoarse && m_autoCoarseSamples != samples)
    {
      m_autoCoarseSamples = samples;
      m_autoCoarseRate = 0u;

      auto vki = m_device->vki();
      VkPhysicalDevice physicalDevice = m_device->adapter()->handle();

      uint32_t count = 0u;
      VkResult result = vki->vkGetPhysicalDeviceFragmentShadingRatesKHR(
          physicalDevice,
          &count, nullptr);

      if (result == VK_SUCCESS && count > 0u)
      {
        std::vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(count);

        for (auto &rate : rates)
          rate.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;

        result = vki->vkGetPhysicalDeviceFragmentShadingRatesKHR(
            physicalDevice, &count, rates.data());

        Logger::info(str::format(
            "GazeVRS: shading rate query result=", int32_t(result),
            " count=", count));

        if (result == VK_SUCCESS || result == VK_INCOMPLETE)
        {
          VkSampleCountFlags sampleMask = VkSampleCountFlags(samples);

          for (const auto &rate : rates)
          {
            if (!(rate.sampleCounts & sampleMask))
              continue;

            m_autoCoarseRate = getFragmentRateCode(rate.fragmentSize);

            Logger::info(str::format(
                "GazeVRS: auto coarse=", rate.fragmentSize.width, "x", rate.fragmentSize.height,
                " (code ", m_autoCoarseRate, "), samples=", uint32_t(samples)));

            break;
          }
        }
      }

      if (result != VK_SUCCESS && result != VK_INCOMPLETE)
      {
        Logger::warn(str::format(
            "GazeVRS: failed to query fragment shading rates, result=",
            int32_t(result)));
      }
      else if (count == 0u)
      {
        Logger::warn(
            "GazeVRS: no fragment shading rates reported");
      }
    }

    if (m_autoCoarse)
      coarse = m_autoCoarseRate;

    const uint32_t tilesX = (extent.width + m_texel.width - 1u) / m_texel.width;
    const uint32_t tilesY = (extent.height + m_texel.height - 1u) / m_texel.height;

    if (tilesX > m_maxTiles.width || tilesY > m_maxTiles.height)
      return false;

    const uint32_t usedLayers = sbs ? 1u : MaxLayers;
    const float eyeW = sbs ? float(extent.width) * 0.5f : float(extent.width);
    const std::array<uint32_t, 5> shape = {
        extent.width, extent.height, usedLayers, uint32_t(colorFormat), uint32_t(samples)};

    // Publish only a completed upload. This barrier precedes any new upload,
    // so rendering with the front image never depends on the back upload.
    const std::array<uint64_t, 2> completed = {
        m_slots[0]->completion->value(), m_slots[1]->completion->value()};
    if (m_buffers.promote(completed))
    {
      VkImageMemoryBarrier2 ready = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      ready.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
      ready.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      ready.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
      ready.dstAccessMask = VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
      ready.oldLayout = ready.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      ready.srcQueueFamilyIndex = ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ready.image = m_slots[m_buffers.active]->image;
      ready.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, MaxLayers};
      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1u;
      dep.pImageMemoryBarriers = &ready;
      cmd->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dep);
    }

    GazeMapRequest request;
    request.shape = shape;
    request.texel = {m_texel.width, m_texel.height};
    request.coarse = coarse;
    request.r1 = m_r1;
    request.r2 = m_r2;
    request.fullArea = m_fullArea;
    request.midArea = m_midArea;

    if (m_mapWorker->maps.failed())
    {
      if (!m_loggedWorkerFailure)
      {
        Logger::err("GazeVRS: map worker failed; disabling VRS");
        m_loggedWorkerFailure = true;
      }
      return false;
    }
    m_mapWorker->maps.exchange(request, true, m_cpuMap);
    const int32_t back = m_buffers.writable(completed);
    // A completed map can lag behind gaze, but must match this render target.
    // Do not upload a map already displayed or already pending on the GPU.
    auto matches = [&](const Rc<Slot> &slot) {
      return slot->shape == m_cpuMap->request.shape && slot->flags == m_cpuMap->request.flags
          && slot->coarse == m_cpuMap->request.coarse && slot->center == m_cpuMap->request.center;
    };
    const bool upload = back >= 0 && m_cpuMap && m_cpuMap->valid
        && m_cpuMap->request.shape == shape && m_cpuMap->request.coarse == coarse
        && (m_buffers.active < 0 || !matches(m_slots[m_buffers.active]));

    VkImageSubresourceRange allLayers = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, MaxLayers};

    if (upload)
    {
      auto &slot = m_slots[back];
      const uint32_t layerStride = (tilesX * tilesY + 3u) & ~3u;
      const auto &data = m_cpuMap->data;
      const auto &mapRequest = m_cpuMap->request;

      // Previous uses of this slot have completed on the GPU. No broad
      // graphics-to-transfer dependency is needed for a free back slot.
      VkImageMemoryBarrier2 imgBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      imgBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      imgBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
      imgBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      imgBarrier.oldLayout = slot->layoutInit ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
      imgBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      imgBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      imgBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      imgBarrier.image = slot->image;
      imgBarrier.subresourceRange = allLayers;

      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1u;
      dep.pImageMemoryBarriers = &imgBarrier;
      cmd->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dep);

      for (size_t off = 0u; off < data.size(); off += 65536u)
      {
        size_t chunk = std::min<size_t>(65536u, data.size() - off);
        cmd->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, slot->buffer, off, chunk, data.data() + off);
      }

      VkBufferMemoryBarrier2 bufBarrier2 = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
      bufBarrier2.srcQueueFamilyIndex = bufBarrier2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      bufBarrier2.buffer = slot->buffer;
      bufBarrier2.size = VK_WHOLE_SIZE;
      bufBarrier2.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
      bufBarrier2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      bufBarrier2.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
      bufBarrier2.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;

      VkDependencyInfo dep2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep2.bufferMemoryBarrierCount = 1u;
      dep2.pBufferMemoryBarriers = &bufBarrier2;
      cmd->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dep2);

      std::array<VkBufferImageCopy2, MaxLayers> regions = {};

      for (uint32_t l = 0; l < usedLayers; l++)
      {
        regions[l].sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
        regions[l].bufferOffset = VkDeviceSize(l) * layerStride;
        regions[l].bufferRowLength = 0u;
        regions[l].bufferImageHeight = 0u;
        regions[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, l, 1u};
        regions[l].imageOffset = {0, 0, 0};
        regions[l].imageExtent = {tilesX, tilesY, 1u};
      }

      VkCopyBufferToImageInfo2 copyInfo = {VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
      copyInfo.srcBuffer = slot->buffer;
      copyInfo.dstImage = slot->image;
      copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
      copyInfo.regionCount = usedLayers;
      copyInfo.pRegions = regions.data();
      cmd->cmdCopyBufferToImage(DxvkCmdBuffer::ExecBuffer, &copyInfo);

      slot->layoutInit = true;
      slot->shape = shape;
      slot->flags = mapRequest.flags;
      slot->coarse = mapRequest.coarse;
      slot->center = mapRequest.center;
      slot->dotCenter = mapRequest.dotCenter;
      cmd->track(slot);
      cmd->queueSignal(slot->completion, m_buffers.upload(back));
    }

    // During startup or a render-target change, use full-rate rendering until
    // a compatible attachment is ready rather than waiting for an upload.
    if (!m_cpuMap || !m_cpuMap->valid || m_buffers.active < 0
        || m_slots[m_buffers.active]->shape != shape || m_slots[m_buffers.active]->coarse != coarse)
      return false;

    auto &front = m_slots[m_buffers.active];
    cmd->track(front);
    cmd->queueSignal(front->completion, m_buffers.read());
    // Debug overlay (sbs only): colored squares drawn at the end of the pass.
    //  - normal: magenta dots at the displayed attachment's saved gaze,
    //    including its upload latency and tile-granularity update caching.
    //  - FAKE=grid: 8 differently colored squares at known framebuffer positions
    if ((m_dot || m_grid) && sbs)
    {
      g_gazeDot.passActive = true;
      g_gazeDot.width = int32_t(extent.width);
      g_gazeDot.height = int32_t(extent.height);
      g_gazeDot.radius = m_dotRadius;
      g_gazeDot.x[0] = front->dotCenter[0];
      g_gazeDot.y[0] = front->dotCenter[1];
      g_gazeDot.x[1] = int32_t(eyeW) + front->dotCenter[2];
      g_gazeDot.y[1] = front->dotCenter[3];
      g_gazeDot.count = 0;

      auto add = [&](float px, float py, int32_t r, float cr, float cg, float cb)
      {
        if (g_gazeDot.count >= 16u)
          return;
        auto &d = g_gazeDot.dots[g_gazeDot.count++];
        d.x = int32_t(px * float(extent.width));
        d.y = int32_t(py * float(extent.height));
        d.r = r;
        d.rgb[0] = cr;
        d.rgb[1] = cg;
        d.rgb[2] = cb;
      };

      if (m_grid)
      {
        const int32_t sz = 70;
        add(0.03f, 0.06f, sz, 1.0f, 0.0f, 0.0f); // top-left corner      red
        add(0.97f, 0.06f, sz, 0.0f, 1.0f, 0.0f); // top-right corner     green
        add(0.03f, 0.94f, sz, 0.0f, 0.0f, 1.0f); // bottom-left corner   blue
        add(0.97f, 0.94f, sz, 1.0f, 1.0f, 0.0f); // bottom-right corner  yellow
        add(0.25f, 0.50f, sz, 1.0f, 1.0f, 1.0f); // center of left half  white
        add(0.75f, 0.50f, sz, 1.0f, 0.0f, 1.0f); // center of right half magenta
        add(0.25f, 0.12f, sz, 0.0f, 1.0f, 1.0f); // top of left half     cyan
        add(0.75f, 0.12f, sz, 1.0f, 0.5f, 0.0f); // top of right half    orange
      }
      else
      {
        add(float(g_gazeDot.x[0]) / float(extent.width), float(g_gazeDot.y[0]) / float(extent.height), m_dotRadius, 1.0f, 0.0f, 1.0f);
        add(float(g_gazeDot.x[1]) / float(extent.width), float(g_gazeDot.y[1]) / float(extent.height), m_dotRadius, 1.0f, 0.0f, 1.0f);
      }
    }

    info.imageView = front->view;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    info.shadingRateAttachmentTexelSize = m_texel;
    return true;
  }

}
