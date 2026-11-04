#pragma once
#include <ir/ir_builder.h>
#include <ir/ir_utils.h>

namespace dxvk::qvfr {
  // Floating-point color only. Source and destination have the same sample
  // count. Preserve sample IDs in focus (exact copies at density 1); match the existing nearest MSAA
  // blitter's sample-grid mapping in the peripheral region.
  inline dxbc_spv::ir::SsaDef emitCompositeColor(dxbc_spv::ir::Builder& b,
      dxbc_spv::ir::SsaDef image, dxbc_spv::ir::SsaDef lowCoord,
      dxbc_spv::ir::SsaDef position, dxbc_spv::ir::SsaDef focusDst,
      dxbc_spv::ir::SsaDef focusSrc, dxbc_spv::ir::SsaDef focusExtent,
      dxbc_spv::ir::SsaDef sourceExtent, dxbc_spv::ir::SsaDef fade,
      dxbc_spv::ir::SsaDef sampleId, uint32_t samples) {
    using namespace dxbc_spv::ir;
    BasicType f2(ScalarType::eF32, 2), i2(ScalarType::eI32, 2), f4(ScalarType::eF32, 4);
    auto extract = [&] (SsaDef v, uint32_t i) {
      return b.add(Op::CompositeExtract(ScalarType::eF32, v, b.makeConstant(i)));
    };
    auto relative = b.add(Op::FSub(f2, position, b.add(Op::ConvertItoF(f2, focusDst))));
    auto remaining = b.add(Op::FSub(f2, b.add(Op::ConvertItoF(f2, focusExtent)), relative));
    auto edge = b.add(Op::FMin(f2, relative, remaining));
    auto distance = b.add(Op::FMin(ScalarType::eF32, extract(edge, 0), extract(edge, 1)));
    auto inside = b.add(Op::FGt(ScalarType::eBool, distance, b.makeConstant(0.0f)));
    auto denominator = b.add(Op::FMax(ScalarType::eF32, fade, b.makeConstant(1.0f)));
    auto weight = b.add(Op::FDiv(ScalarType::eF32, distance, denominator));
    weight = b.add(Op::FMin(ScalarType::eF32, b.makeConstant(1.0f),
      b.add(Op::FMax(ScalarType::eF32, b.makeConstant(0.0f), weight))));
    weight = b.add(Op::Select(ScalarType::eF32,
      b.add(Op::FGt(ScalarType::eBool, fade, b.makeConstant(0.0f))), weight,
      b.add(Op::Select(ScalarType::eF32, inside, b.makeConstant(1.0f), b.makeConstant(0.0f)))));
    auto focusRatio = b.add(Op::FDiv(f2, b.add(Op::ConvertItoF(f2, sourceExtent)),
      b.add(Op::ConvertItoF(f2, focusExtent))));
    auto focusCoord = b.add(Op::IAdd(i2, focusSrc,
      b.add(Op::ConvertFtoI(i2, b.add(Op::FMul(f2, relative, focusRatio))))));
    uint32_t sx = samples >= 8 ? 4 : samples >= 2 ? 2 : 1;
    uint32_t sy = samples / sx;
    auto grid = b.add(Op::ConvertFtoI(i2,
      b.add(Op::FMul(f2, lowCoord, b.makeConstant(float(sx), float(sy))))));
    auto lowPixel = b.add(Op::SShr(i2, grid,
      b.makeConstant(int32_t(sx == 4 ? 2 : sx == 2 ? 1 : 0),
        int32_t(sy == 4 ? 2 : sy == 2 ? 1 : 0))));
    SsaDef lowSample, focusSample, mip;
    if (samples > 1) {
      auto ux = b.add(Op::CompositeExtract(ScalarType::eI32, grid, b.makeConstant(0u)));
      auto uy = b.add(Op::CompositeExtract(ScalarType::eI32, grid, b.makeConstant(1u)));
      auto index = b.add(Op::IAdd(ScalarType::eI32,
        b.add(Op::IAnd(ScalarType::eI32, ux, b.makeConstant(int32_t(sx-1)))),
        b.add(Op::IMul(ScalarType::eI32, b.makeConstant(int32_t(sx)),
          b.add(Op::IAnd(ScalarType::eI32, uy, b.makeConstant(int32_t(sy-1))))))));
      uint64_t map = samples == 2 ? 0x1ull : samples == 4 ? 0x3210ull
        : samples == 8 ? 0x26147035ull : 0xe58b602c3714d9afull;
      auto shift = b.add(Op::IMul(ScalarType::eI32, index, b.makeConstant(4)));
      lowSample = b.add(Op::ConvertItoI(ScalarType::eU32,
        b.add(Op::UShr(ScalarType::eU64, b.makeConstant(map), shift))));
      lowSample = b.add(Op::IAnd(ScalarType::eU32, lowSample, b.makeConstant(15u)));
      focusSample = sampleId;
    } else mip = b.makeConstant(0u);
    auto pickFocus = b.add(Op::FGt(ScalarType::eBool, weight, b.makeConstant(0.0f)));
    auto pickFocus2 = b.add(Op::CompositeConstruct(BasicType(ScalarType::eBool, 2), pickFocus, pickFocus));
    auto coord = b.add(Op::Select(i2, pickFocus2, focusCoord, lowPixel));
    auto sample = samples > 1
      ? b.add(Op::Select(ScalarType::eU32, pickFocus, focusSample, lowSample)) : SsaDef();
    auto value = b.add(Op::ImageLoad(f4, image, mip, SsaDef(), coord, sample, SsaDef()));

    // Only the fade band needs two reads. Interior and exterior use one read.
    auto blend = b.add(Op::BAnd(ScalarType::eBool, pickFocus,
      b.add(Op::FLt(ScalarType::eBool, weight, b.makeConstant(1.0f)))));
    auto merge = b.add(Op::Label());
    auto branchEnd = b.addBefore(merge, Op::Branch(merge));
    auto body = b.addBefore(branchEnd, Op::Label());
    auto branch = b.addBefore(body, Op::BranchConditional(blend, body, merge));
    auto parent = findContainingBlock(b, branch);
    b.rewriteOp(parent, Op::LabelSelection(merge));
    b.setCursor(body);
    auto low = b.add(Op::ImageLoad(f4, image, mip, SsaDef(), lowPixel, lowSample, SsaDef()));
    auto w4 = b.add(Op::CompositeConstruct(f4, weight, weight, weight, weight));
    auto mixed = b.add(Op::FMad(f4, b.add(Op::FSub(f4, value, low)), w4, low));
    b.setCursor(merge);
    return b.add(Op::Phi(f4).addPhi(parent, value).addPhi(body, mixed));
  }
}
