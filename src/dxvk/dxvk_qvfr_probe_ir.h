#pragma once

#include <ir/ir_builder.h>

namespace dxvk::qvfr {
  // Diagnostic replay: replace application outputs with the actual, unmodified
  // normalized coordinates of one texture instruction. No UV/gaze heuristic.
  // Multiple sample sites and output feedback are deliberately ambiguous.
  inline bool lowerOrientationProbe(dxbc_spv::ir::Builder& b, uint32_t slot, uint16_t* samplerMask = nullptr) {
    using namespace dxbc_spv::ir;
    SsaDef entry, sample;
    uint32_t declarations = 0;
    std::vector<SsaDef> stores;
    std::vector<SsaDef> outputs;
    auto zero = [&] (SsaDef d) {
      if (!d) return true;
      const auto& op = b.getOp(d);
      if (!op.isConstant()) return false;
      for (uint32_t i = 0; i < op.getOperandCount(); i++)
        if (uint64_t(op.getOperand(i))) return false;
      return true;
    };
    for (auto i = b.begin(); i != b.end(); ++i) {
      const auto& op = *i;
      if (op.getOpCode() == OpCode::eEntryPoint) {
        if (ShaderStage(op.getOperand(op.getOperandCount()-1)) != ShaderStage::ePixel || entry)
          return false;
        entry = op.getDef();
      }
      if (op.getOpCode() == OpCode::eDclSrv && !op.getOperand(1)) {
        uint32_t base = uint32_t(op.getOperand(2)), count = uint32_t(op.getOperand(3));
        if (slot >= base && (!count || slot-base < count)) declarations++;
      }
      switch (op.getOpCode()) {
        case OpCode::eDclOutput:
        case OpCode::eDclOutputBuiltIn: outputs.push_back(op.getDef()); break;
        case OpCode::eOutputLoad:
        case OpCode::eDclUav:
        case OpCode::eDclInputTarget:
          return false;
        case OpCode::eOutputStore: stores.push_back(op.getDef()); break;
        case OpCode::eImageSample: {
          auto load = b.getOp(SsaDef(op.getOperand(0)));
          if (load.getOpCode() != OpCode::eDescriptorLoad) break;
          auto srv = b.getOp(SsaDef(load.getOperand(0)));
          if (srv.getOpCode() != OpCode::eDclSrv || srv.getOperand(1)
           || uint32_t(srv.getOperand(2)) != slot) break;
          if (sample || uint32_t(srv.getOperand(3)) != 1
           || ResourceKind(srv.getOperand(4)) != ResourceKind::eImage2D
           || !zero(SsaDef(load.getOperand(1))) || op.getOperand(2)
           || !zero(SsaDef(op.getOperand(4))) || op.getOperand(10)
           || b.getOp(SsaDef(op.getOperand(3))).getType() != Type(ScalarType::eF32, 2))
            return false;
          sample = op.getDef();
        } break;
        default: break;
      }
    }
    if (!entry || !sample || declarations != 1) return false;
    auto samplerLoad = b.getOp(SsaDef(b.getOp(sample).getOperand(1)));
    if (samplerLoad.getOpCode() != OpCode::eDescriptorLoad
     || !zero(SsaDef(samplerLoad.getOperand(1)))) return false;
    auto sampler = b.getOp(SsaDef(samplerLoad.getOperand(0)));
    if (sampler.getOpCode() != OpCode::eDclSampler || sampler.getOperand(1)
     || uint32_t(sampler.getOperand(2)) >= 16 || uint32_t(sampler.getOperand(3)) != 1) return false;
    if (samplerMask) *samplerMask = uint16_t(1u << uint32_t(sampler.getOperand(2)));
    // Leave control flow and discard intact. The clear value and validity tag
    // distinguish uncovered/discarded pixels from successful measurements.
    for (auto store : stores) b.remove(store);
    for (auto old : outputs) {
      std::vector<SsaDef> names;
      auto uses = b.getUses(old);
      for (auto i = uses.first; i != uses.second; ++i)
        if (i->getOpCode() == OpCode::eDebugName) names.push_back(i->getDef());
      for (auto name : names) b.remove(name);
      b.remove(old);
    }
    auto output = b.add(Op::DclOutput(Type(ScalarType::eF32, 4), entry, 0, 0));
    auto fn = SsaDef(b.getOp(entry).getOperand(0));
    b.addAfter(fn, Op::OutputStore(output, SsaDef(), b.makeConstant(0.f, 0.f, 0.f, 0.f)));
    auto uv = SsaDef(b.getOp(sample).getOperand(3));
    auto x = b.addBefore(sample, Op::CompositeExtract(ScalarType::eF32, uv, b.makeConstant(0u)));
    auto y = b.addBefore(sample, Op::CompositeExtract(ScalarType::eF32, uv, b.makeConstant(1u)));
    auto value = b.addBefore(sample, Op::CompositeConstruct(BasicType(ScalarType::eF32, 4),
      x, y, b.makeConstant(1.f), b.makeConstant(1.f)));
    b.addBefore(sample, Op::OutputStore(output, SsaDef(), value));
    return true;
  }
}
