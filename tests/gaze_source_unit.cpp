// Native test links only the source adapter, with environment/logging stubs.
// c++ -std=c++17 -O2 -pthread -Iinclude/native/windows -Isrc/dxvk tests/gaze_source_unit.cpp src/dxvk/dxvk_gaze_source.cpp -o /tmp/gaze-source-unit
#include "dxvk_gaze_source.h"
#include "../src/util/log/log.h"
#include <cassert>
#include <iostream>
#include <map>

static std::map<std::string,std::string> variables;
namespace dxvk::env {
  std::string getEnvVar(const char *name) {return variables[name];}
}
namespace dxvk {
  void Logger::info(const std::string &) { }
}

int main()
{
  dxvk::GazeMapRequest pass;
  pass.shape={160u,80u,1u,97u,1u};pass.texel={8u,8u};
  dxvk::GazeMap result;
  variables["DXVK_GAZE_FAKE"]="fixed";
  {
    dxvk::DxvkGazeSource source;
    assert(source.generate(pass,result));
    assert(result.valid);
    assert((result.request.dotCenter==std::array<int32_t,4>{40,20,40,20}));
    assert(!source.generate(pass,result));
    auto changed=pass;changed.shape[2]=2u;
    assert(source.generate(changed,result));
    assert((result.request.dotCenter==std::array<int32_t,4>{80,20,80,20}));
    assert(!source.generate(changed,result));
    assert(source.generate(pass,result));
    assert(result.request.shape==pass.shape);
  }
  variables["DXVK_GAZE_GAIN_Y"]="2";
  variables["DXVK_GAZE_FLIP_Y"]="1";
  {
    dxvk::DxvkGazeSource source;
    assert(source.generate(pass,result));
    assert(result.request.fy[0]==1.0f && result.request.fy[1]==1.0f);
    assert((result.request.dotCenter==std::array<int32_t,4>{40,80,40,80}));
  }
  variables["DXVK_GAZE_FLIPY_R"]="1";
  {
    dxvk::DxvkGazeSource source;
    assert(source.generate(pass,result));
    assert(result.request.fy[0]==1.0f && result.request.fy[1]==0.0f);
    assert((result.request.dotCenter==std::array<int32_t,4>{40,80,40,0}));
  }
  std::cout<<"Gaze source tests passed\n";
}
