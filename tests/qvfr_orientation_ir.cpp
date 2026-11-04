#include "../src/dxvk/dxvk_qvfr_probe_ir.h"
#include "../src/util/util_qvfr_orientation.h"
#include <ir/ir_legalize.h>
#include <spirv/spirv_builder.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>

using namespace dxbc_spv::ir;
#define check(c) do { if (!(c)) { std::cerr << "check failed at line " << __LINE__ << '\n'; std::abort(); } } while (false)

static Builder shader(uint32_t variant) {
  Builder b;
  auto fn = b.add(Op::Function(Type())); b.add(Op::FunctionEnd());
  auto ep = b.add(Op::EntryPoint(fn, variant == 3 ? ShaderStage::eVertex : ShaderStage::ePixel));
  b.setCursor(fn);
  auto input = b.add(Op::DclInput(Type(ScalarType::eF32, 2), ep, 0, 0));
  auto output = b.add(Op::DclOutput(Type(ScalarType::eF32, 4), ep, 0, 0));
  auto srv = b.add(Op::DclSrv(ScalarType::eF32, ep, 0, 3, 1, ResourceKind::eImage2D));
  auto sampler = b.add(Op::DclSampler(ep, 0, 2, 1));
  auto image = b.add(Op::DescriptorLoad(ScalarType::eSrv, srv, b.makeConstant(0u)));
  auto smp = b.add(Op::DescriptorLoad(ScalarType::eSampler, sampler, b.makeConstant(0u)));
  auto uv = b.add(Op::InputLoad(Type(ScalarType::eF32, 2), input, SsaDef()));
  if (variant == 1)
    uv = b.add(Op::FAdd(Type(ScalarType::eF32, 2),
      b.add(Op::FMul(Type(ScalarType::eF32, 2), uv, b.makeConstant(1.f, -1.f))), b.makeConstant(0.f, 1.f)));
  if (variant == 6) {
    auto cbv = b.add(Op::DclCbv(Type(ScalarType::eF32, 4).addArrayDimension(1), ep, 0, 0, 1));
    auto cb = b.add(Op::DescriptorLoad(ScalarType::eCbv, cbv, b.makeConstant(0u)));
    auto row = b.add(Op::BufferLoad(Type(ScalarType::eF32, 4), cb, b.makeConstant(0u), 16));
    auto x = b.add(Op::CompositeExtract(ScalarType::eF32, row, b.makeConstant(0u)));
    auto y = b.add(Op::CompositeExtract(ScalarType::eF32, row, b.makeConstant(1u)));
    uv = b.add(Op::FMul(Type(ScalarType::eF32, 2), uv,
      b.add(Op::CompositeConstruct(BasicType(ScalarType::eF32, 2), x, y))));
  }
  SsaDef branch;
  if (variant == 7) {
    auto x = b.add(Op::CompositeExtract(ScalarType::eF32, uv, b.makeConstant(0u)));
    auto cond = b.add(Op::FLt(ScalarType::eBool, x, b.makeConstant(.5f)));
    branch = b.add(Op::ScopedIf(SsaDef(), cond));
  }
  auto color = b.add(Op::ImageSample(Type(ScalarType::eF32, 4), image, smp, SsaDef(), uv,
    variant == 4 ? b.makeConstant(0, 1) : SsaDef(), SsaDef(), SsaDef(), SsaDef(), SsaDef(), SsaDef(), SsaDef()));
  if (variant == 2)
    b.add(Op::ImageSample(Type(ScalarType::eF32, 4), image, smp, SsaDef(),
      b.add(Op::FAdd(Type(ScalarType::eF32, 2), uv, b.makeConstant(.1f, .1f))),
      SsaDef(), SsaDef(), SsaDef(), SsaDef(), SsaDef(), SsaDef(), SsaDef()));
  if (variant == 5)
    b.add(Op::OutputLoad(Type(ScalarType::eF32, 4), output, SsaDef()));
  b.add(Op::OutputStore(output, SsaDef(), color));
  if (branch) {
    auto end = b.add(Op::ScopedEndIf(branch));
    b.rewriteOp(branch, Op(b.getOp(branch)).setOperand(0, end));
  }
  b.add(Op::Return());
  return b;
}

int main(int argc, char** argv) {
  for (uint32_t variant = 0; variant < 8; variant++) {
    auto b = shader(variant);
    bool supported = variant == 0 || variant == 1 || variant == 6 || variant == 7;
    uint16_t samplers = 0;
    bool measured = dxvk::qvfr::lowerOrientationProbe(b, 3, &samplers);
    if (measured != supported) std::cerr << "variant=" << variant << " measured=" << measured << '\n';
    check(measured == supported);
    if (!supported) continue;
    check(samplers == (1u << 2));
    CompileOptions opts;
    legalizeIr(b, opts);
    dxbc_spv::spirv::BasicResourceMapping mapping;
    dxbc_spv::spirv::SpirvBuilder::Options options;
    dxbc_spv::spirv::SpirvBuilder spv(b, mapping, options);
    spv.buildSpirvBinary();
    auto words = spv.getSpirvBinary();
    check(!words.empty());
    if (argc > 1) {
      std::ofstream out(std::string(argv[1]) + std::to_string(variant) + ".spv", std::ios::binary);
      out.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(uint32_t));
      check(bool(out));
    }
  }
  using namespace dxvk::qvfr;
  std::array<OrientationPixel, 16> pixels;
  constexpr uint32_t height = 2592;
  for (bool flip : {false, true}) {
    for (uint32_t point = 0; point < 4; point++) for (uint32_t y = 0; y < 2; y++)
      for (uint32_t x = 0; x < 2; x++) {
        float v = (height * ((point & 2) ? 3u : 1u) / 4u + y + .5f) / height;
        pixels[point*4+y*2+x] = {.25f, flip ? 1-v : v, 1.f, 1.f};
      }
    for (uint32_t eye = 0; eye < 2; eye++)
      check(!std::strcmp(classifyOrientation(pixels, eye, height), flip ? "flip-y" : "identity-y"));
    // Half-width final blits only cover one eye. The other eye's clear
    // pixels must not hide a measured flip, or be reported as identity.
    auto half = pixels;
    for (uint32_t point : {1u, 3u}) for (uint32_t p = 0; p < 4; p++)
      half[point*4+p] = {};
    check(!std::strcmp(classifyOrientation(half, 0, height), flip ? "flip-y" : "identity-y"));
    check(!std::strcmp(classifyOrientation(half, 1, height), "unknown"));
  }
  pixels[0].valid = 0;
  check(!std::strcmp(classifyOrientation(pixels, 0, height), "unknown"));
  pixels[0].valid = 1;
  pixels[0].v = std::numeric_limits<float>::quiet_NaN();
  check(!std::strcmp(classifyOrientation(pixels, 0, height), "unknown"));
  pixels[0].v = .5f;
  check(!std::strcmp(classifyOrientation(pixels, 0, height), "other"));
  std::cout << "QVFR orientation: probe IR, SPIR-V and measured Y classification passed\n";
}
