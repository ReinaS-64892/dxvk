#include "../src/util/util_qvfr_virtual.h"
#include "../src/util/util_qvfr_post.h"
#include <cstdlib>
#include <iostream>
using namespace dxvk::qvfr;
static void check(bool b) { if(!b) std::abort(); }
static void near(float a,float b) { check(std::abs(a-b)<.002f); }
int main() {
  // Post replay runs four passes over both eyes and records only color.
  // Neither eye-local draws nor depth/stencil work may bypass scene replay.
  check(postReplayEligible(true, false, false, false)); // Stereo color post.
  check(!postReplayEligible(false, false, false, false)); // Eye-local color.
  check(!postReplayEligible(false, true, false, false)); // Eye-local world.
  check(!postReplayEligible(true, true, false, false)); // Stereo world.
  check(!postReplayEligible(true, false, true, false)); // Stencil mask.
  check(!postReplayEligible(true, false, false, true)); // Depth bounds.
  VirtualMap identity;
  auto same=virtualUv(identity,-.2f,1.2f);
  check(same[0]==-.2f && same[1]==1.2f);
  constexpr auto vs = "vs.135010b798b162853b35cfe2e49f79ab";
  constexpr auto ps = "fs.3649abe482390ade1473c1588ebba0d1";
  check(measuredIdentityPost(vs, ps, 97));
  check(!measuredIdentityPost("other-vs", ps, 97));
  check(!measuredIdentityPost(vs, "other-ps", 97));
  check(!measuredIdentityPost(vs, ps, 37));
  // An identity postprocess must keep the actual SOURCE crop, even if gaze
  // has moved since that snapshot was made. Check repeated passes, both eyes,
  // edge-clamped gaze, raster positions and the next consumer's atlas UVs.
  for (double v : {0.0, .25, .5, .75, 1.0}) {
    auto source = makeScenePlan({5184,2592,1}, .5, .25,
      {{{.25,v},{.75,1-v}}});
    check(bool(source));
    auto output = postSnapshotEyes({5184,2592,1}, .5, .25, source->eyes,
      {{{.8,1-v},{.2,v}}}, measuredIdentityPost(vs, ps, 97));
    check(bool(output));
    auto repeated = postSnapshotEyes({5184,2592,1}, .5, .25, *output,
      {{{.1,.1},{.9,.9}}}, true);
    check(bool(repeated));
    auto src = virtualLayout({5184,2592,1}, source->eyes);
    auto dst = virtualLayout({5184,2592,1}, *output);
    for (uint32_t eye = 0; eye < 2; eye++) {
      auto s = source->eyes[eye].composite;
      auto d = (*output)[eye].composite;
      check(s.x == d.x && s.y == d.y && s.width == d.width && s.height == d.height);
      check((*repeated)[eye].composite.y == d.y);
      auto a = (*output)[eye].clip, b = source->eyes[eye].clip;
      check(a.scaleX == b.scaleX && a.scaleY == b.scaleY
        && a.offsetX == b.offsetX && a.offsetY == b.offsetY);
      auto raster = replayAtlasViewport((*output)[eye], {0,0,5184,2592,0}, true,
        0, 2592, 5184, -2592);
      auto local = scratchAtlas((*output)[eye]).focus;
      for (uint32_t x : {0u,100u,647u}) for (uint32_t y : {0u,100u,647u}) {
        float u = (d.x+x+.5f)/5184, w = (d.y+y+.5f)/2592;
        auto read = virtualUv(src.mapping, u, w);
        auto next = virtualUv(dst.mapping, u, w);
        near(read[0]*src.extent.width, src.focus[eye].x+x+.5f);
        near(read[1]*src.extent.height, y+.5f);
        near(next[0]*dst.extent.width, dst.focus[eye].x+x+.5f);
        near(next[1]*dst.extent.height, y+.5f);
        near(float(raster.x+u*raster.width+dst.focus[eye].x-local.x), dst.focus[eye].x+x+.5f);
        near(float(raster.y+(1-w)*raster.height+dst.focus[eye].y-local.y), dst.focus[eye].y+y+.5f);
      }
    }
  }
  // Default Y inversion is applied at the scene boundary, once. Unknown
  // outputs use that same absolute default; measured identity outputs above
  // retain it, even when newer gaze would produce a different crop.
  for (double v : {0.0, .25, .5, .75, 1.0}) {
    std::array<Gaze, 2> raw = {Gaze{.25, v}, Gaze{.75, 1-v}};
    auto scene = raw;
    auto source = makeScenePlan({5184,2592,1},.5,.25,scene);
    auto rawPlan = makePlan({5184,2592,1},StereoLayout::DoubleWide,.5,.25,raw);
    auto output = postSnapshotEyes({5184,2592,1},.5,.25,source->eyes,scene,false);
    auto repeated = makeOutputPlan({5184,2592,1},.5,.25,scene);
    auto expected = makePlan({5184,2592,1},StereoLayout::DoubleWide,.5,.25,
      {orientGaze(raw[0], true), orientGaze(raw[1], true)});
    check(source && output && repeated && expected && rawPlan);
    auto src = virtualLayout({5184,2592,1},source->eyes);
    auto dst = virtualLayout({5184,2592,1},*output);
    for (unsigned eye = 0; eye < 2; eye++) {
      auto s = source->eyes[eye].composite;
      auto d = (*output)[eye].composite;
      check(s.x == d.x && s.y == d.y);
      check(rawPlan->eyes[eye].composite.y+s.y+s.height == 2592);
      check(repeated->eyes[eye].composite.y == d.y);
      check(expected->eyes[eye].composite.y == d.y);
      for (uint32_t y : {0u, 100u, 647u}) {
        float u = (d.x+100.5f)/5184, w = (d.y+y+.5f)/2592;
        auto read = virtualUv(src.mapping,u,w);
        near(read[0]*src.extent.width,src.focus[eye].x+100.5f);
        near(read[1]*src.extent.height,y+.5f);
        auto next = virtualUv(dst.mapping,u,w);
        near(next[0]*dst.extent.width,dst.focus[eye].x+100.5f);
        near(next[1]*dst.extent.height,y+.5f);
      }
    }
  }
  for(double gaze:{0.0,.25,.5,.75,1.0}) {
    auto plan=makePlan({5184,2592,1},StereoLayout::DoubleWide,.5,.25,{{{gaze,gaze},{1-gaze,1-gaze}}});
    check(bool(plan));
    auto v=virtualLayout({5184,2592,1},plan->eyes);
    check(v.extent.width==3888 && v.extent.height==1296);
    for(uint32_t eye=0;eye<2;eye++) {
      auto c=plan->eyes[eye].composite;
      for(uint32_t x:{0u,100u,647u}) for(uint32_t y:{0u,100u,647u}) {
        auto uv=virtualUv(v.mapping,(c.x+x+.5f)/5184,(c.y+y+.5f)/2592);
        near(uv[0]*v.extent.width,v.focus[eye].x+x+.5f);
        near(uv[1]*v.extent.height,y+.5f);
      }
    }
    // Distorted/out-of-range lookups remain in a valid tile, with no access
    // to the atlas padding or a neighbouring eye's focus tile.
    for(float u:{-2.f,0.f,.1f,.49f,.5f,.9f,1.f,3.f})
      for(float w:{-2.f,0.f,.2f,.8f,1.f,3.f}) {
        auto uv=virtualUv(v.mapping,u,w);
        check(uv[0]>=0 && uv[0]<1 && uv[1]>=0 && uv[1]<1);
      }
  }
  std::cout<<"QVFR virtual atlas coordinates and eye/crop boundaries passed\n";
}
