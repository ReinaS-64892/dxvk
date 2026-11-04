#pragma once

#include "dxvk_context_state.h"
#include "dxvk_gpu_query.h"
#include "../util/util_qvfr.h"
#include "../util/util_qvfr_gaze_shm.h"
#include "../util/util_qvfr_cache.h"
#include "../util/util_qvfr_virtual.h"
#include "../util/util_qvfr_orientation.h"
#include "../util/util_qvfr_unsafe.h"
#include "../util/util_qvfr_scene.h"
#include "../util/util_qvfr_control.h"
#include <map>
#include <set>

namespace dxvk {

  struct DxvkQvfrGroup {
    qvfr::PendingPlanes pending;
    bool stencilValid = false;
    uint32_t eye = 0;
    uint64_t frame = 0;
    uint64_t lastUse = 0;
    qvfr::EyePlan plan = {};
    DxvkRenderTargets original;
    DxvkRenderTargets atlas;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
    uint32_t lowW = 0, lowH = 0, focusW = 0, focusH = 0;

    qvfr::CacheSlot slot() const {
      return {original.color[0].view ? uintptr_t(original.color[0].view->image()) : 0,
        original.depth.view ? uintptr_t(original.depth.view->image()) : 0,
        eye, lastUse, pending.any()};
    }
  };

  struct DxvkQvfrTiming {
    std::string_view phase;
    Rc<DxvkQuery> begin, end;
    std::chrono::steady_clock::time_point cpuBegin;
    double cpuMs = 0;
    uint64_t endTrackingId = 0;
  };

  struct DxvkQvfrVirtualImage {
    Rc<DxvkImage> logical;
    Rc<DxvkImageView> atlas;
    qvfr::VirtualLayout layout;
    std::array<qvfr::EyePlan, 2> eyes;
  };

  struct DxvkQvfrOrientationCapture {
    Rc<DxvkBuffer> readback;
    uint64_t frame = 0, tracking = 0, pass = 0, sequence = 0;
    uint64_t source = 0, target = 0;
    uint32_t slot = 0;
    uint32_t height = 0;
    bool terminalCandidate = false;
    DxvkViewportState viewport;
    std::string vs, ps;
  };

  struct DxvkQvfrOrientationShader {
    Rc<DxvkShader> shader;
    uint16_t samplers = 0;
  };

  struct DxvkQvfrState {
    bool sceneOnly = true;
    bool lazyDepthStencil = false;
    bool unsafeSkipImageSeed = false, unsafeSkipStencilSeed = false, unsafeSkipStencilCompose = false;
    uint64_t unsafeImageSeedsSkipped = 0, unsafeStencilSeedsSkipped = 0, unsafeStencilCompositesSkipped = 0;
    bool unsafeSkipDepthCompose = false, colorSinglePass = true;
    uint32_t colorFadePixels = 0;
    uint64_t unsafeDepthCompositesSkipped = 0, colorSinglePassComposites = 0, colorSinglePassFallbacks = 0;
    qvfr::SceneCycle sceneCycle;
    uint64_t sceneEnds = 0;
    uint64_t atlasImagesCreated = 0, atlasRebuilds = 0, atlasSeedReuses = 0;
    uint64_t seedMappingChanged = 0, seedMappingSame = 0;
    uint64_t compatibilityChecks = 0, compatibilityBarriers = 0, compatibilityRelocations = 0;
    uint64_t stencilWriteBatches = 0, stencilReadOnlyBatches = 0;
    uint64_t nativeDirectBatches = 0, nativeDirectDraws = 0;
    qvfr::UnsafeTerminalCycle terminalCycle;
    uint32_t replayEye = 0;
    uint64_t terminalBatches = 0, deferredFlushes = 0, droppedDraws = 0, droppedCompute = 0;
    uint64_t retainedEvictions = 0, droppedSnapshots = 0, droppedPlanes = 0, rawVirtualReads = 0;
    bool postDepthDisabled = false;
    DxvkDepthStencilState postSavedDepth;
    DxvkDepthBounds postSavedBounds;
    bool orientationTrace = false;
    uint64_t orientationFrame = ~uint64_t(0), orientationCount = 0;
    uint32_t orientationFrameCount = 0;
    uint64_t orientationSequence = 0;
    Rc<DxvkImageView> orientationTarget;
    Rc<DxvkShader> orientationSavedShader;
    DxvkRenderTargets orientationSavedTargets;
    DxvkViewportState orientationSavedVp;
    DxvkOmAttachmentBlend orientationSavedBlend;
    std::map<std::pair<uint32_t, uint32_t>, DxvkQvfrOrientationShader> orientationShaders;
    std::map<std::array<uint64_t, 6>, uint64_t> orientationLastCapture;
    std::vector<DxvkQvfrOrientationCapture> orientationPending;
    std::map<std::array<uint64_t, 3>, uint64_t> orientationTransfers;
    uint32_t orientationTransferCount = 0;
    qvfr::Extent size = {};
    uint32_t format = 97;
    uint32_t samples = 4;
    double scale = 0.5, focusScale = 1.0;
    double fraction = 0.25;
    bool sharedGaze = false;
    bool debugOutline = false;
    bool sourceOutline = false;
    qvfr::OutlineCycle outlineCycle;
    bool stencilEnabled = true;
    bool unsafePost = false;
    bool unsafeInstancing = false;
    bool safeInstancing = false;
    bool unsafeReplay = false;
    bool enabled = true;
    bool controlError = false;
    qvfr::ControlSettings controlDefaults, controlRequested;
    std::string controlPath;
    std::chrono::steady_clock::time_point nextControlPoll = {};
    uint64_t indirectBatches = 0, advancedBatches = 0;
    uint64_t instancedBatches = 0, depthRefreshes = 0;
    const char* lastSkip = "draw";
    uint32_t fallbackInstances = 0, fallbackFirstInstance = 0;
    std::array<size_t, 2> postGroups = {};
    std::optional<DxvkQvfrVirtualImage> postSnapshot;
    size_t postTiming = size_t(-1);
    uint64_t postCacheBatches = 0, postSnapshotBatches = 0, postPasses = 0;
    bool virtualEnabled = true, interopTransition = false;
    std::map<const DxvkImage*, DxvkQvfrVirtualImage> virtualImages;
    std::map<uint32_t, Rc<DxvkImageView>> virtualBindings;
    std::array<qvfr::VirtualMap, qvfr::VirtualSlots> virtualTable = {};
    Rc<DxvkBuffer> virtualUniforms;
    Rc<DxvkShader> virtualSavedShader;
    std::map<uint32_t, Rc<DxvkShader>> virtualShaders;
    uint64_t virtualCopies = 0, virtualAliases = 0, virtualResolves = 0, virtualReads = 0, virtualMaterialized = 0;
    uint64_t replayedBatches = 0;
    uint64_t replayPasses = 0, pairedInOnePass = 0;
    uint64_t pairPass = ~uint64_t(0);
    // Reason names are string literals; lookup does not allocate per draw.
    std::map<std::string_view, uint64_t> skipped;
    std::set<std::string_view> targetSkips;
    uint64_t groups = 0;
    uint64_t colorComposites = 0, depthComposites = 0, stencilComposites = 0, stencilBatches = 0;
    bool profile = false, profileClipped = false;
    bool profileFull = false, profileCaptureFull = false;
    uint64_t cpuProfileFrame = ~uint64_t(0), cpuQvfrCalls = 0, cpuNativeCalls = 0;
    double cpuQvfrMs = 0, cpuNativeMs = 0;
    qvfr::ReplayTimingBudget replayTimingBudget, nativeTimingBudget;
    uint64_t profileFrame = ~uint64_t(0);
    size_t profileReplay = size_t(-1);
    std::vector<DxvkQvfrTiming> timings;
    uint64_t cacheClock = 0;
    uint64_t cacheHits = 0, seededGroups = 0, colorRefreshes = 0;
    uint64_t discardedColor = 0, discardedDepth = 0, discardedStencil = 0, virtualDiscarded = 0;
    uint64_t externalReads = 0;
    uint64_t fallbackDiagnosticFrame = ~uint64_t(0);
    uint32_t fallbackDiagnosticCount = 0;
    size_t active = 0;
    std::array<DxvkQvfrGroup, 4> cache;
    std::map<std::string_view, uint64_t> compositionReasons;
    DxvkRenderTargets savedTargets;
    DxvkViewportState savedVp;
    void* gazeFile = nullptr;
    void* gazeMap = nullptr;
    const gaze_bridge::SharedGaze* gaze = nullptr;
    gaze_bridge::Freshness freshness;
    std::chrono::steady_clock::time_point nextOpen = {};
    std::string gazePath;
    uint64_t gazeFrame = ~uint64_t(0);
    bool gazeValid = false;
    // Raw bridge UVs; scene/output default orientation is applied to copies.
    std::array<qvfr::Gaze, 2> frameGaze = {};

    ~DxvkQvfrState();
    bool readGaze(std::array<qvfr::Gaze, 2>& result);
    void closeGaze();
  };

  // Measure only outer application draws; recursive peripheral/focus draws
  // are already inside that scope. This is command-recording CPU time, not
  // the application's simulation time or the D3D11 producer thread's time.
  struct DxvkQvfrCpuScope {
    DxvkQvfrState* state = nullptr;
    uint64_t batches = 0;
    std::chrono::steady_clock::time_point start;
    DxvkQvfrCpuScope(DxvkQvfrState* q, bool busy, uint64_t frame) {
      if (!q || !q->profile || busy || frame % 120) return;
      state = q;
      if (q->cpuProfileFrame != frame) {
        q->cpuProfileFrame = frame;
        q->cpuQvfrMs = q->cpuNativeMs = 0;
        q->cpuQvfrCalls = q->cpuNativeCalls = 0;
      }
      batches = q->replayedBatches;
      start = std::chrono::steady_clock::now();
    }
    ~DxvkQvfrCpuScope() {
      if (!state) return;
      double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
      if (state->replayedBatches != batches) {
        state->cpuQvfrMs += ms;
        state->cpuQvfrCalls++;
      } else {
        state->cpuNativeMs += ms;
        state->cpuNativeCalls++;
      }
    }
  };

}
