#include "../src/dxvk/dxvk_qvfr_composite_ir.h"
#include <ir/passes/ir_pass_scalarize.h>
#include <ir/passes/ir_pass_remove_unused.h>
#include <ir/passes/ir_pass_cse.h>
#include <spirv/spirv_builder.h>
#include <fstream>
#include <iostream>
#include <cstdlib>

using namespace dxbc_spv;
using namespace dxbc_spv::ir;

int main(int argc, char** argv) {
  for (uint32_t samples : {1u, 2u, 4u, 8u, 16u}) {
    Builder b;
    auto fn = b.add(Op::Function(Type()));
    b.add(Op::FunctionEnd());
    auto ep = b.add(Op::EntryPoint(fn, ShaderStage::ePixel));
    b.setCursor(fn);
    b.add(Op::Label());
    BasicType f2(ScalarType::eF32, 2), f4(ScalarType::eF32, 4);
    auto input = b.add(Op::DclInput(f2, ep, 0, 0));
    auto position = b.add(Op::DclInputBuiltIn(f4, ep, BuiltIn::ePosition));
    auto sample = b.add(Op::DclInputBuiltIn(ScalarType::eU32, ep, BuiltIn::eSampleId, InterpolationMode::eFlat));
    auto output = b.add(Op::DclOutput(f4, ep, 0, 0));
    auto srv = b.add(Op::DclSrv(ScalarType::eF32, ep, 0, 0, 1,
      samples == 1 ? ResourceKind::eImage2D : ResourceKind::eImage2DMS));
    auto image = b.add(Op::DescriptorLoad(ScalarType::eSrv, srv, b.makeConstant(0u)));
    auto low = b.add(Op::InputLoad(f2, input, SsaDef()));
    auto pos4 = b.add(Op::InputLoad(f4, position, SsaDef()));
    auto x = b.add(Op::CompositeExtract(ScalarType::eF32, pos4, b.makeConstant(0u)));
    auto y = b.add(Op::CompositeExtract(ScalarType::eF32, pos4, b.makeConstant(1u)));
    auto pos = b.add(Op::CompositeConstruct(f2, x, y));
    auto sampleId = b.add(Op::InputLoad(ScalarType::eU32, sample, SsaDef()));
    auto extentDecl = b.add(Op::DclPushData(BasicType(ScalarType::eI32, 2), ep, 0u, ShaderStage::ePixel));
    auto sourceExtent = b.add(Op::PushDataLoad(BasicType(ScalarType::eI32, 2), extentDecl, SsaDef()));
    // Dynamic fade width keeps both the one-read and two-read branches alive.
    auto value = dxvk::qvfr::emitCompositeColor(b, image, low, pos,
      b.makeConstant(2592 + 800, 700), b.makeConstant(0, 1296),
      b.makeConstant(648, 648), sourceExtent, x, sampleId, samples);
    b.add(Op::OutputStore(output, SsaDef(), value));
    b.add(Op::Return());
    ScalarizePass::runPass(b, ScalarizePass::Options());
    CsePass::Options cse;
    cse.relocateDescriptorLoad = true;
    do {
      RemoveUnusedPass::runPass(b);
    } while (CsePass::runPass(b, cse));
    RemoveUnusedPass::runRemoveUnusedFloatModePass(b);
    spirv::BasicResourceMapping mapping;
    spirv::SpirvBuilder::Options options;
    spirv::SpirvBuilder spv(b, mapping, options);
    spv.buildSpirvBinary();
    auto words = spv.getSpirvBinary();
    if (words.empty()) std::abort();
    if (argc > 1) {
      std::ofstream out(std::string(argv[1]) + std::to_string(samples) + ".spv", std::ios::binary);
      out.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(uint32_t));
      if (!out) std::abort();
    }
  }
  std::cout << "QVFR composite: sample counts 1/2/4/8/16 with dynamic fade compiled to SPIR-V\n";
}
