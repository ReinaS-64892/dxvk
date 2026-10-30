// Native test: c++ -std=c++17 -O2 -pthread -Isrc/dxvk tests/gaze_map_unit.cpp -o /tmp/gaze-map-unit
#include "dxvk_gaze_map.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace dxvk;
using namespace std::chrono_literals;

static GazeMapRequest request(int id = 0)
{
  GazeMapRequest r;
  r.shape = {160u, 80u, 1u, 97u, 1u};
  r.texel = {8u, 8u};
  r.fx = {0.5f, 0.5f}; r.fy = {0.5f, 0.5f};
  r.center = {id, 5, id, 5};
  r.dotCenter = {id * 8, 40, id * 8, 40};
  return r;
}

template<typename Pred>
static void until(Pred pred)
{
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!pred())
  {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(1ms);
  }
}

static void checkPixels(const GazeMapRequest &r)
{
  GazeMap result;
  generateGazeMap(r, result);
  const uint32_t tx = (r.shape[0] + r.texel[0] - 1u) / r.texel[0];
  const uint32_t ty = (r.shape[1] + r.texel[1] - 1u) / r.texel[1];
  const uint32_t stride = (tx * ty + 3u) & ~3u;
  assert(result.data.size() == size_t(stride) * r.shape[2]);
  // Independent scalar reference for the original radius/area semantics,
  // both eye layouts, asymmetric gaze and layer-offset padding.
  const bool sbs = r.shape[2] == 1u;
  const float eyeW = sbs ? r.shape[0] * 0.5f : float(r.shape[0]);
  const float minD = std::min(eyeW, float(r.shape[1]));
  auto radius = [&](float area, float fallback) {
    if (area < 0.0f) return fallback;
    const float a = std::clamp(area, 0.0f, 100.0f) * 0.01f;
    return a >= 1.0f ? 10.0f : std::sqrt(a * eyeW * float(r.shape[1]) / 3.14159265f) / minD;
  };
  const float r1 = radius(r.fullArea, r.r1), r2 = radius(r.midArea, r.r2);
  for (uint32_t l=0; l<r.shape[2]; ++l)
  {
    for (uint32_t y=0; y<ty; ++y)
      for (uint32_t x=0; x<tx; ++x)
      {
        const float px=(x+0.5f)*r.texel[0], py=(y+0.5f)*r.texel[1];
        const uint32_t eye=sbs ? (px<eyeW ? 0u:1u) : l;
        const float lx=sbs ? px-float(eye)*eyeW : px;
        const float d=std::hypot(lx-r.fx[eye]*eyeW,py-r.fy[eye]*r.shape[1])/minD;
        uint8_t expected=0u;
        if (d>=r1) expected=uint8_t(d>=r2 ? r.coarse:std::min(5u,r.coarse));
        assert(result.data[size_t(l)*stride+y*tx+x]==expected);
      }
    for (uint32_t i=tx*ty; i<stride; ++i) assert(result.data[size_t(l)*stride+i]==0u);
  }
  assert(result.request.dotCenter == r.dotCenter);
  // A recycled allocation must also clear layer-alignment padding.
  std::fill(result.data.begin(), result.data.end(), 255u);
  generateGazeMap(r, result);
  for (uint32_t l = 0; l < r.shape[2]; ++l)
    for (uint32_t i = tx * ty; i < stride; ++i)
      assert(result.data[size_t(l) * stride + i] == 0u);
}

struct Gate
{
  std::mutex mutex;
  std::condition_variable cv;
  bool release = false;
  int entered = 0;
  std::vector<int> jobs;
  std::thread::id thread;
};

struct SlowGenerator
{
  Gate *gate;
  bool operator()(const GazeMapRequest &r, GazeMap &result) const
  {
    {
      std::unique_lock<std::mutex> lock(gate->mutex);
      gate->thread=std::this_thread::get_id();
      gate->jobs.push_back(r.center[0]);
      ++gate->entered;
      gate->cv.notify_all();
      gate->cv.wait(lock,[&] {return gate->release;});
    }
    generateGazeMap(r,result);
    return true;
  }
};

// Hold the real mailbox mutex to verify the render-side call uses try_lock.
struct TestMutex
{
  static TestMutex *instance;
  std::mutex value;
  TestMutex() {instance=this;}
  void lock() {value.lock();}
  bool try_lock() {return value.try_lock();}
  void unlock() {value.unlock();}
};
TestMutex *TestMutex::instance=nullptr;

int main()
{
  for (uint32_t layers : {1u,2u})
    for (uint32_t coarse : {0u,5u,10u})
      for (float area : {-1.0f,0.0f,45.0f,100.0f})
      {
        auto r=request(); r.shape={137u,73u,layers,97u,1u};
        r.fx={0.13f,0.91f};r.fy={0.77f,0.22f};r.coarse=coarse;
        r.fullArea=area;r.midArea=area<0.0f ? -1.0f:std::max(65.0f,area);
        checkPixels(r);
      }
  {
    // Thread startup and destruction with no submitted job.
    GazeMapWorker<> idle;
  }
  Gate gate;
  std::unique_ptr<GazeMap> result;
  {
    using Worker=GazeMapWorker<std::thread,TestMutex,std::condition_variable_any,SlowGenerator>;
    Worker worker({},SlowGenerator{&gate});
    until([&] {worker.exchange(request(1),true,result);std::lock_guard<std::mutex> lock(gate.mutex);return gate.entered==1;});
    assert(!result);
    assert(gate.thread!=std::this_thread::get_id());

    // The worker is deliberately blocked inside generation. Render calls
    // must still return and queued gaze 2 must be replaced by the latest 3.
    worker.exchange(request(2),true,result);
    worker.exchange(request(3),true,result);
    std::mutex holdMutex;
    std::condition_variable holdCv;
    bool holding = false, release = false;
    std::thread holder([&] {
      std::lock_guard<TestMutex> mailbox(*TestMutex::instance);
      std::unique_lock<std::mutex> lock(holdMutex);
      holding = true;
      holdCv.notify_one();
      holdCv.wait(lock, [&] { return release; });
    });
    {
      std::unique_lock<std::mutex> lock(holdMutex);
      holdCv.wait(lock, [&] { return holding; });
    }
    assert(!worker.exchange(request(4),true,result));
    {
      std::lock_guard<std::mutex> lock(holdMutex);
      release = true;
    }
    holdCv.notify_one();
    holder.join();
    {
      std::lock_guard<std::mutex> lock(gate.mutex);gate.release=true;
    }
    gate.cv.notify_all();
    until([&] {worker.exchange(request(3),true,result);return result && result->request.center[0]==3;});
    assert(result->request.dotCenter==request(3).dotCenter);
    {
      std::lock_guard<std::mutex> lock(gate.mutex);
      assert((gate.jobs==std::vector<int>{1,3}));
    }
    // Fixed gaze does not repeatedly compute the same map.
    for(int i=0;i<100;i++) worker.exchange(request(3),true,result);
    assert(!worker.failed());
  }
  {
    GazeMapWorker<> worker;
    for (int i = 0; i < 500; ++i)
      worker.exchange(request(i), true, result);
    until([&] {
      worker.exchange(request(499), true, result);
      return result && result->request.center[0] == 499;
    });
    assert(result->request.dotCenter == request(499).dotCenter);
  }
  {
    // Gaze can change on the worker without a new render-side request.
    // Invalid data must be published, and recovery at the same coordinates
    // must regenerate a usable map.
    struct PollGenerator
    {
      std::atomic<int> *sample;
      int last = -2;
      bool operator()(const GazeMapRequest &pass, GazeMap &result)
      {
        const int current = sample->load();
        if (current == last) return false;
        last = current;
        result.request = pass;
        result.valid = current >= 0;
        if (result.valid)
        {
          auto r = pass;
          r.center[0] = current;
          r.dotCenter[0] = current * 8;
          generateGazeMap(r, result);
        }
        return true;
      }
    };
    std::atomic<int> sample = {1};
    GazeMapWorker<std::thread,std::mutex,std::condition_variable,PollGenerator>
        worker({}, PollGenerator{&sample}, 2ms);
    result.reset();
    until([&] {
      worker.exchange(request(), true, result);
      return result && result->valid && result->request.center[0] == 1;
    });
    sample.store(-1);
    until([&] {
      worker.exchange(request(), false, result);
      return result && !result->valid;
    });
    sample.store(1);
    until([&] {
      worker.exchange(request(), false, result);
      return result && result->valid && result->request.center[0] == 1;
    });
    assert(result->request.dotCenter[0] == 8);
  }
  {
    struct ThrowGenerator {bool operator()(const GazeMapRequest &,GazeMap &) const {throw std::runtime_error("test");}};
    GazeMapWorker<std::thread,std::mutex,std::condition_variable,ThrowGenerator> worker;
    until([&] {worker.exchange(request(),true,result);return worker.failed();});
  }
  std::cout<<"Gaze map worker tests passed\n";
}
