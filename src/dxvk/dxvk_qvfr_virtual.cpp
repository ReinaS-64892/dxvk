#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_qvfr.h"
#include "dxvk_shader_ir.h"
#include <cstring>

namespace dxvk {
  namespace {
    Rc<DxvkImageView> virtualView(const Rc<DxvkImage>& image, VkImageUsageFlags usage,
        VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT) {
      DxvkImageViewKey key = {};
      key.viewType = VK_IMAGE_VIEW_TYPE_2D;
      key.format = format;
      key.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
      key.mipCount = key.layerCount = 1;
      key.layout = VK_IMAGE_LAYOUT_GENERAL;
      key.usage = usage;
      return image->createView(key);
    }

    void copyVirtualTile(DxvkContext& ctx, const Rc<DxvkImageView>& dst, qvfr::Rect d,
        const Rc<DxvkImageView>& src, qvfr::Rect s, bool resolve) {
      if (!resolve) {
        ctx.copyImage(dst->image(), {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, {int32_t(d.x),int32_t(d.y),0},
          src->image(), {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, {int32_t(s.x),int32_t(s.y),0}, {s.width,s.height,1});
      } else {
        // Equal-sized compact tiles: the existing blit path averages MSAA
        // samples without ever expanding back into the logical full-size image.
        VkOffset3D so[2]={{int32_t(s.x),int32_t(s.y),0},{int32_t(s.x+s.width),int32_t(s.y+s.height),1}};
        VkOffset3D to[2]={{int32_t(d.x),int32_t(d.y),0},{int32_t(d.x+d.width),int32_t(d.y+d.height),1}};
        ctx.blitImageView(dst,to,virtualView(src->image(),VK_IMAGE_USAGE_SAMPLED_BIT),so,VK_FILTER_NEAREST);
      }
    }
  }

  bool DxvkContext::tryQvfrVirtualCopy(const Rc<DxvkImage>& dst, const Rc<DxvkImage>& src,
      const VkImageSubresourceLayers& dstSub, VkOffset3D dstOffset,
      const VkImageSubresourceLayers& srcSub, VkOffset3D srcOffset,
      VkExtent3D extent, bool resolve, VkFormat format) {
    if (!m_qvfr || !m_qvfr->enabled || m_qvfrBusy || !m_qvfr->virtualEnabled || m_qvfr->debugOutline
     || (src == dst && !m_qvfr->unsafeReplay)) return false;
    auto& q=*m_qvfr;
    if (q.unsafeReplay && q.terminalCycle.normal) return false;
    auto supportedFormat = [&] (const DxvkImage& image) {
      auto f = image.info().format;
      return f == VK_FORMAT_R16G16B16A16_SFLOAT || (f == VK_FORMAT_R16G16B16A16_UNORM
        && (image.info().flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT))
        || (q.unsafeReplay && (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB
          || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB));
    };
    auto full=[&](const Rc<DxvkImage>& image, const VkImageSubresourceLayers& sub, VkOffset3D offset) {
      const auto& i=image->info();
      return sub.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT && !sub.mipLevel && !sub.baseArrayLayer && sub.layerCount==1
        && !offset.x && !offset.y && !offset.z && i.type==VK_IMAGE_TYPE_2D && i.mipLevels==1 && i.numLayers==1
        && i.extent.width==q.size.width && i.extent.height==q.size.height && i.extent.depth==1
        && extent.width==q.size.width && extent.height==q.size.height && extent.depth==1
        && supportedFormat(*image);
    };
    if (!full(src,srcSub,srcOffset) || !full(dst,dstSub,dstOffset) || q.format!=97) return false;
    if ((dst->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || dst->info().tiling!=VK_IMAGE_TILING_OPTIMAL) return false;
    if (resolve && ((!q.unsafeReplay && format!=VK_FORMAT_R16G16B16A16_SFLOAT) || dst->info().sampleCount!=VK_SAMPLE_COUNT_1_BIT
       || src->info().sampleCount==VK_SAMPLE_COUNT_1_BIT)) return false;
    if (!resolve && dst->info().sampleCount!=src->info().sampleCount) return false;

    // Retain the snapshot locally before overwriting a destination or evicting
    // another map entry. Texture handles are logical identities, not snapshots.
    std::optional<DxvkQvfrVirtualImage> source;
    auto vi=q.virtualImages.find(src.ptr());
    if (vi!=q.virtualImages.end() && src != dst) source=vi->second;
    std::array<DxvkQvfrGroup*,2> groups = {};
    if (!source) {
      for (auto& g:q.cache)
        if (g.pending.color && !g.pending.colorStale && g.original.color[0].view
         && g.original.color[0].view->image()==src.ptr()) groups[g.eye]=&g;
      if (!groups[0] || !groups[1]) return false;
    }
    VkFormat atlasFormat = source ? source->atlas->info().format : groups[0]->atlas.color[0].view->info().format;
    if (!dst->isViewCompatible(atlasFormat) || (resolve && format != atlasFormat)) return false;
    std::array<qvfr::EyePlan,2> eyes = source ? source->eyes
      : std::array<qvfr::EyePlan,2>{groups[0]->plan,groups[1]->plan};
    auto layout=qvfr::virtualLayout(q.size,eyes);
    const auto& limits=m_device->properties().core.properties.limits;
    if (layout.extent.width>std::min(limits.maxImageDimension2D,limits.maxFramebufferWidth)
     || layout.extent.height>std::min(limits.maxImageDimension2D,limits.maxFramebufferHeight)) return false;
    // Establish usage before an external GetVulkanImageInfo can return the
    // logical handle. Materialization must not relocate that exported handle.
    m_qvfrBusy=true;
    DxvkImageUsageInfo logicalUsage = {};
    logicalUsage.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool compatible=ensureImageCompatibility(dst,logicalUsage);
    m_qvfrBusy=false;
    if (!compatible) return false;
    // A complete overwrite needs no materialization of the old destination.
    q.virtualImages.erase(dst.ptr());
    flushQvfrImage(dst.ptr(),"virtual-overwrite");
    if (q.unsafeReplay && src != dst)
      for (auto& g : q.cache)
        if (g.original.color[0].view && g.original.color[0].view->image() == dst.ptr()) {
          q.discardedColor += g.pending.color;
          g.pending.complete(true, false);
          g.pending.invalidateColor();
        }
    if (q.virtualImages.size()>=8) {
      if (q.unsafeReplay && !q.terminalCycle.normal) {
        q.virtualImages.erase(q.virtualImages.begin()); q.droppedSnapshots++;
      } else materializeQvfrVirtual(q.virtualImages.begin()->first,"virtual-evict");
    }
    if (source && !resolve) {
      // These backing images are immutable. A full same-sample-count copy is
      // another logical reference to the same snapshot, with no GPU copy.
      auto alias=*source;
      alias.logical=dst;
      q.virtualImages.emplace(dst.ptr(),std::move(alias));
      if (dst->info().sampleCount>VK_SAMPLE_COUNT_1_BIT)
        m_implicitResolves.invalidate(*dst,dst->getAvailableSubresources());
      q.virtualCopies++;
      q.virtualAliases++;
      return true;
    }

    m_qvfrBusy=true;
    endCurrentPass(true);
    auto timing=beginQvfrTiming(resolve ? "virtual-resolve" : "virtual-copy");
    DxvkImageCreateInfo info = {};
    info.type=VK_IMAGE_TYPE_2D; info.format=atlasFormat;
    info.sampleCount=dst->info().sampleCount;
    info.extent={layout.extent.width,layout.extent.height,1}; info.mipLevels=info.numLayers=1;
    info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.stages=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    info.access=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    info.layout=VK_IMAGE_LAYOUT_GENERAL;
    auto image=m_device->createImage(info,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    initImage(image,VK_IMAGE_LAYOUT_UNDEFINED);
    auto atlas=virtualView(image,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,atlasFormat);
    for (uint32_t eye=0;eye<2;eye++) {
      auto old=source ? source->atlas : groups[eye]->atlas.color[0].view;
      auto local=qvfr::scratchAtlas(eyes[eye]);
      // Use the actual atlas format for RGBA8/sRGB as well as HDR.
      auto copy = [&] (qvfr::Rect to, qvfr::Rect from) {
        if (!resolve) copyVirtualTile(*this,atlas,to,old,from,false);
        else {
          VkOffset3D so[2]={{int32_t(from.x),int32_t(from.y),0},{int32_t(from.x+from.width),int32_t(from.y+from.height),1}};
          VkOffset3D dstOff[2]={{int32_t(to.x),int32_t(to.y),0},{int32_t(to.x+to.width),int32_t(to.y+to.height),1}};
          blitImageView(atlas,dstOff,virtualView(old->image(),VK_IMAGE_USAGE_SAMPLED_BIT,atlasFormat),so,VK_FILTER_NEAREST);
        }
      };
      copy(layout.low[eye],source ? source->layout.low[eye] : local.peripheral);
      copy(layout.focus[eye],source ? source->layout.focus[eye] : local.focus);
    }
    q.virtualImages.emplace(dst.ptr(),DxvkQvfrVirtualImage{dst,atlas,layout,eyes});
    if (dst->info().sampleCount>VK_SAMPLE_COUNT_1_BIT)
      m_implicitResolves.invalidate(*dst,dst->getAvailableSubresources());
    if (resolve) q.virtualResolves++; else q.virtualCopies++;
    endQvfrTiming(timing);
    m_qvfrBusy=false;
    return true;
  }

  void DxvkContext::materializeQvfrVirtual(const DxvkImage* image, const char* reason) {
    if (!m_qvfr || m_qvfrBusy) return;
    if (deferQvfrPublish(reason)) return;
    auto& q=*m_qvfr;
    auto entry=q.virtualImages.find(image);
    if (entry==q.virtualImages.end()) return;
    auto v=std::move(entry->second);
    q.virtualImages.erase(entry);
    m_qvfrBusy=true;
    endCurrentPass(true);
    auto timing=beginQvfrTiming("virtual-materialize");
    DxvkImageUsageInfo usage = {};
    usage.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!ensureImageCompatibility(v.logical,usage))
      throw DxvkError("QVFR: cannot materialize virtual color image");
    auto format = v.atlas->info().format;
    auto dst=virtualView(v.logical,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,format);
    for (uint32_t eye=0;eye<2;eye++) {
      auto s=v.layout.low[eye], d=v.eyes[eye].original;
      VkOffset3D so[2]={{int32_t(s.x),int32_t(s.y),0},{int32_t(s.x+s.width),int32_t(s.y+s.height),1}};
      VkOffset3D to[2]={{int32_t(d.x),int32_t(d.y),0},{int32_t(d.x+d.width),int32_t(d.y+d.height),1}};
      blitImageView(dst,to,virtualView(v.atlas->image(),VK_IMAGE_USAGE_SAMPLED_BIT,format),so,VK_FILTER_NEAREST);
      copyVirtualTile(*this,dst,v.eyes[eye].composite,v.atlas,v.layout.focus[eye],false);
    }
    endQvfrTiming(timing);
    if (q.virtualMaterialized++<16)
      Logger::info(str::format("QVFR virtual materialize reason=",reason," resource=",uint64_t(image->getResourceId()),
        " image=",uint64_t(image->handle())));
    m_qvfrBusy=false;
  }

  void DxvkContext::prepareQvfrFullClear(const DxvkImageView* view, VkImageAspectFlags aspects) {
    if (!m_qvfr || m_qvfrBusy || !view) return;
    if (!m_qvfr->virtualEnabled && !m_qvfr->lazyDepthStencil) { flushQvfrView(view,"clear"); return; }
    auto k=view->info();
    auto image=view->image();
    const auto& info=image->info();
    if (k.mipIndex || k.layerIndex || k.mipCount!=1 || k.layerCount!=1
     || info.mipLevels!=1 || info.numLayers!=1) { flushQvfrView(view,"clear"); return; }
    auto& q=*m_qvfr;
    if (aspects & VK_IMAGE_ASPECT_COLOR_BIT)
      q.virtualDiscarded += q.virtualImages.erase(image);
    for (size_t i=0;i<q.cache.size();i++) {
      auto& g=q.cache[i];
      if (!g.pending.any()) continue;
      if ((aspects & VK_IMAGE_ASPECT_COLOR_BIT) && g.original.color[0].view
       && g.original.color[0].view->image()==image) {
        q.discardedColor += g.pending.color;
        g.pending.complete(true,false);
        g.pending.invalidateColor();
      }
      if ((aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && g.original.depth.view
       && g.original.depth.view->image()==image) {
        // Preserve only surviving planes before ending this continuation.
        // A stencil-only clear must not lose pending color or depth, and a
        // depth-only clear must preserve pending stencil.
        VkImageAspectFlags surviving = VK_IMAGE_ASPECT_COLOR_BIT
          | ((VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) & ~aspects);
        flushQvfrGroup(i,"clear-preserve",surviving);
        bool depth = aspects & VK_IMAGE_ASPECT_DEPTH_BIT;
        bool stencil = aspects & VK_IMAGE_ASPECT_STENCIL_BIT;
        q.discardedDepth += depth && g.pending.depth;
        q.discardedStencil += stencil && g.pending.stencil;
        g.pending.complete(false,depth,stencil);
        // Strict UNSAFE retains the surviving color instead of publishing
        // it. A subsequent scene draw may therefore reuse this same atlas:
        // reload cleared depth/stencil rather than testing against old data.
        if (depth) g.pending.invalidateDepth();
        if (stencil) g.stencilValid = false;
      }
    }
  }

  void DxvkContext::prepareQvfrVirtualReads() {
    if (!m_qvfr || !m_qvfr->enabled || !m_qvfr->virtualEnabled || m_qvfrBusy || m_qvfr->virtualImages.empty()) return;
    auto& q=*m_qvfr;
    if (m_flags.test(DxvkContextFlag::GpDirtyPipeline) && !updateGraphicsPipeline()) return;
    if (m_state.gp.flags.any(DxvkGraphicsPipelineFlag::HasStorageDescriptors,
        DxvkGraphicsPipelineFlag::HasTransformFeedback)) return;
    auto shader=m_state.gp.shaders.fs;
    if (!shader) return;
    const auto& meta=shader->metadata();
    std::array<bool,qvfr::VirtualSlots> used = {};
    if (q.unsafeReplay) {
      auto pipeline = lookupGraphicsPipeline(m_state.gp.shaders);
      if (!pipeline) return;
      auto range = pipeline->getLayout()->getReadOnlyResourcesForStage(VK_SHADER_STAGE_FRAGMENT_BIT);
      for (uint32_t i = 0; i < range.bindingCount; i++) {
        auto binding = range.bindings[i];
        uint32_t index = binding.getResourceIndex();
        if (binding.getDescriptorType() == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && index < used.size())
          used[index] = true;
      }
    }
    std::array<qvfr::VirtualMap,qvfr::VirtualSlots> table = {};
    for(uint32_t slot=0;slot<qvfr::VirtualSlots;slot++) {
      if (q.unsafeReplay && !used[slot]) continue;
      auto original=m_resources[slot].imageView;
      if (!original) continue;
      auto it=q.virtualImages.find(original->image());
      if (it==q.virtualImages.end()) continue;
      auto mask=meta.qvfrSamplers[slot];
      auto key=original->info();
      bool safe=mask && key.format==it->second.atlas->info().format && key.aspects==VK_IMAGE_ASPECT_COLOR_BIT
        && key.viewType==VK_IMAGE_VIEW_TYPE_2D && !key.mipIndex && key.mipCount==1 && !key.layerIndex && key.layerCount==1
        && original->image()->info().sampleCount==VK_SAMPLE_COUNT_1_BIT;
      for(uint32_t s=0;s<16;s++) if(mask&(1u<<s)) {
        auto sampler=m_samplers[s];
        if (!sampler) { safe=false; continue; }
        const auto& k=sampler->key().u.p;
        safe &= !k.pixelCoord && !k.compareEnable && !k.reduction;
        if (!q.unsafeReplay)
          safe &= k.minFilter==k.magFilter && k.anisotropy<=1
            && k.addressU==VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE && k.addressV==VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      }
      bool raw = !safe && q.unsafeReplay && key.format==it->second.atlas->info().format
        && key.aspects==VK_IMAGE_ASPECT_COLOR_BIT && key.viewType==VK_IMAGE_VIEW_TYPE_2D
        && !key.mipIndex && key.mipCount==1 && !key.layerIndex && key.layerCount==1
        && original->image()->info().sampleCount==VK_SAMPLE_COUNT_1_BIT;
      if (!safe && !raw) continue;
      key.layout=VK_IMAGE_LAYOUT_GENERAL; key.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
      auto view=it->second.atlas->image()->createView(key);
      q.virtualBindings.emplace(slot,original);
      if (safe) table[slot]=it->second.layout.mapping;
      else q.rawVirtualReads++;
      bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT,slot,std::move(view));
      q.virtualReads++;
    }
    if (q.virtualBindings.empty()) return;
    auto& variant=q.virtualShaders[DxvkShader::getCookie(shader)];
    if (!variant) {
      auto ir=dynamic_cast<DxvkIrShader*>(shader.ptr());
      if (ir) variant=ir->createQvfrVirtualShader();
    }
    if (!variant) {
      if (!q.unsafeReplay) restoreQvfrVirtualReads();
      return;
    }
    q.virtualSavedShader=shader;
    bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(Rc<DxvkShader>(variant));
    if (!q.virtualUniforms || std::memcmp(table.data(),q.virtualTable.data(),sizeof(table))) {
      DxvkBufferCreateInfo info = {};
      info.size=sizeof(table); info.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.stages=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; info.access=VK_ACCESS_UNIFORM_READ_BIT;
      info.debugName="QVFR virtual image mappings";
      q.virtualUniforms=m_device->createBuffer(info,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      std::memcpy(q.virtualUniforms->mapPtr(0),table.data(),sizeof(table));
      q.virtualTable=table;
    }
    bindUniformBuffer(VK_SHADER_STAGE_FRAGMENT_BIT,qvfr::VirtualUniformSlot,DxvkBufferSlice(q.virtualUniforms));
  }

  void DxvkContext::finalizeQvfrForExternalUse() {
    if (!m_qvfr) return;
    if (qvfr::fullInteropPublication(m_qvfr->virtualEnabled,
        m_qvfr->lazyDepthStencil, m_qvfr->interopTransition))
      flushQvfr("interop-flush");
    // Proton's Submit explicitly transitions every submitted image first.
    // Generic interop flushes without that image boundary require all outputs.
    // Retained private depth/stencil is unrelated to a known submitted color
    // image, just as intermediate virtual snapshots are in the virtual mode.
    if (!m_qvfr->interopTransition)
      while (!m_qvfr->virtualImages.empty())
        materializeQvfrVirtual(m_qvfr->virtualImages.begin()->first,"interop-flush");
    m_qvfr->interopTransition=false;
  }

  void DxvkContext::restoreQvfrVirtualReads() {
    if (!m_qvfr || m_qvfr->virtualBindings.empty()) return;
    for(auto& [slot,view]:m_qvfr->virtualBindings)
      bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT,slot,std::move(view));
    m_qvfr->virtualBindings.clear();
    if (m_qvfr->virtualSavedShader)
      bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(m_qvfr->virtualSavedShader));
    bindUniformBuffer(VK_SHADER_STAGE_FRAGMENT_BIT,qvfr::VirtualUniformSlot,DxvkBufferSlice());
  }

  bool DxvkContext::deferQvfrPublish(const char* reason) {
    if (!m_qvfr || !m_qvfr->enabled || !m_qvfr->unsafeReplay
     || m_qvfr->terminalCycle.normal || qvfr::unsafePublishBoundary(reason)) return false;
    m_qvfr->deferredFlushes++;
    return true;
  }

  bool DxvkContext::beginQvfrTerminalDraw() {
    if (!m_qvfr || !m_qvfr->enabled || !m_qvfr->unsafeReplay || m_qvfrBusy) return false;
    auto& q = *m_qvfr;
    uint64_t target = 0;
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      const auto& color = m_state.om.renderTargets.color[i];
      // A depth-only pass can leave the submitted color RTV bound. It must
      // not pull the entire new frame back to full resolution prematurely.
      if (color.view && m_state.gp.state.omBlend[i].colorWriteMask()
       && q.terminalCycle.images.count(uint64_t(color.view->image()->getResourceId()))) {
        target = uint64_t(color.view->image()->getResourceId()); break;
      }
    }
    if (!q.terminalCycle.startDraw(target)) return false;
    restoreQvfrVirtualReads();
    flushQvfrShaderImages(false);
    flushQvfrDrawTargets("terminal-draw");
    if (q.terminalBatches++ < 8 || !(q.terminalBatches % 120))
      Logger::info(str::format("QVFR terminal-normal frame=", m_frameCount,
        " target=", target, " reason=learned-external-image batches=", q.terminalBatches,
        " vs=", m_state.gp.shaders.vs ? m_state.gp.shaders.vs->debugName() : "none",
        " ps=", m_state.gp.shaders.fs ? m_state.gp.shaders.fs->debugName() : "none"));
    return true;
  }

  bool DxvkContext::dropUnsafeQvfrDraw() {
    if (!m_qvfr || !m_qvfr->enabled || !m_qvfr->unsafeReplay || m_qvfrBusy || m_qvfr->terminalCycle.normal) return false;
    auto& q = *m_qvfr;
    auto matches = [&] (const Rc<DxvkImageView>& view) {
      if (!view) return false;
      auto e = view->mipLevelExtent(0);
      return e.width == q.size.width && e.height == q.size.height;
    };
    bool vr = matches(m_state.om.renderTargets.depth.view);
    for (const auto& color : m_state.om.renderTargets.color) vr |= matches(color.view);
    if (!vr) return false;
    if (q.droppedDraws++ < 8)
      Logger::info(str::format("QVFR unsafe dropped-draw frame=", m_frameCount,
        " reason=", q.lastSkip, " keep=split"));
    restoreQvfrVirtualReads();
    return true;
  }

  void DxvkContext::noteQvfrTerminalWrite(const DxvkImage& image, const char* operation) {
    if (!m_qvfr || !m_qvfr->enabled || !m_qvfr->unsafeReplay || m_qvfrBusy) return;
    auto& q = *m_qvfr;
    if (!q.terminalCycle.images.count(uint64_t(image.getResourceId()))) return;
    q.terminalCycle.startDraw(uint64_t(image.getResourceId()));
    if (q.terminalBatches++ < 8 || !(q.terminalBatches % 120))
      Logger::info(str::format("QVFR terminal-normal frame=", m_frameCount,
        " target=", uint64_t(image.getResourceId()), " operation=", operation,
        " reason=learned-external-image batches=", q.terminalBatches));
  }

  bool DxvkContext::dropUnsafeQvfrCompute() {
    if (!m_qvfr || !m_qvfr->enabled || !m_qvfr->unsafeReplay || m_qvfrBusy || m_qvfr->terminalCycle.normal) return false;
    auto pipeline = lookupComputePipeline(m_state.cp.shaders);
    if (!pipeline) return false;
    auto layout = pipeline->getLayout();
    auto writable = layout->getReadWriteResources();
    for (uint32_t i = 0; i < writable.bindingCount; i++) {
      auto binding = writable.bindings[i];
      if (binding.getDescriptorType() != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
      auto view = m_resources[binding.getResourceIndex()].imageView;
      if (view && m_qvfr->terminalCycle.startDraw(uint64_t(view->image()->getResourceId()))) {
        m_qvfr->terminalBatches++;
        return false; // The caller now flushes compute inputs/outputs normally.
      }
    }
    // Preserve buffer-producing work (including GPU indirect arguments).
    // Its image writes may be stale, but inventing undefined command-buffer
    // contents is not required to keep the retained image representation.
    for (uint32_t i = 0; i < writable.bindingCount; i++) {
      auto type = writable.bindings[i].getDescriptorType();
      if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
        return false;
    }
    auto touches = [&] (DxvkPipelineBindingRange range) {
      for (uint32_t i = 0; i < range.bindingCount; i++) {
        auto type = range.bindings[i].getDescriptorType();
        if (type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
        auto view = m_resources[range.bindings[i].getResourceIndex()].imageView;
        if (!view) continue;
        auto e = view->mipLevelExtent(0);
        if (e.width == m_qvfr->size.width && e.height == m_qvfr->size.height) return true;
      }
      return false;
    };
    if (!touches(layout->getReadOnlyResourcesForStage(VK_SHADER_STAGE_COMPUTE_BIT))
     && !touches(layout->getReadWriteResources())) return false;
    m_qvfr->droppedCompute++;
    return true;
  }

  uint32_t DxvkContext::qvfrReplayEyes() const {
    if (!m_qvfr || !m_qvfr->unsafeReplay || m_state.vp.viewportCount != 1) return 1;
    const auto& v = m_state.vp.viewports[0];
    return v.x == 0 && v.y == float(m_qvfr->size.height) && v.width == float(m_qvfr->size.width)
      && v.height == -float(m_qvfr->size.height) ? 2 : 1;
  }
}
