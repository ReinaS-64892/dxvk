#pragma once
#include <ir/ir_builder.h>
#include "../util/util_qvfr_virtual.h"

namespace dxvk::qvfr {
  // A nonzero entry means every use of this PS SRV is supported; bits name
  // the sampler registers used by it. Unsupported operations retain native IR.
  inline std::array<uint16_t, VirtualSlots> lowerVirtualSamples(dxbc_spv::ir::Builder& b,
      bool rewrite = true, bool approximate = false) {
    using namespace dxbc_spv::ir;
    std::array<uint16_t, VirtualSlots> result = {};
    struct Sample { SsaDef def; uint32_t slot; };
    std::vector<Sample> samples;
    SsaDef entry;
    auto zero = [&] (SsaDef d) {
      return !d || (b.getOp(d).isConstant() && uint64_t(b.getOp(d).getOperand(0)) == 0);
    };
    std::vector<SsaDef> declarations;
    for (auto i = b.getDeclarations().first; i != b.getDeclarations().second; ++i)
      if (i->getOpCode() == OpCode::eDclSrv) declarations.push_back(i->getDef());
    std::array<uint32_t, VirtualSlots> covering = {};
    for (auto def : declarations) {
      auto decl=b.getOp(def);
      auto ep=b.getOp(SsaDef(decl.getOperand(0)));
      if (ShaderStage(ep.getOperand(ep.getOperandCount()-1)) != ShaderStage::ePixel || decl.getOperand(1)) continue;
      uint32_t base=uint32_t(decl.getOperand(2)), count=uint32_t(decl.getOperand(3));
      if (!count) count=VirtualSlots;
      for (uint32_t i=base;i<VirtualSlots && i-base<count;i++) covering[i]++;
    }
    for (auto def : declarations) {
      auto decl = b.getOp(def);
      auto ep = b.getOp(SsaDef(decl.getOperand(0)));
      if (ep.getOpCode() != OpCode::eEntryPoint || ShaderStage(ep.getOperand(ep.getOperandCount()-1)) != ShaderStage::ePixel
       || decl.getOperand(1) || uint32_t(decl.getOperand(2)) >= VirtualSlots || uint32_t(decl.getOperand(3)) != 1
       || ResourceKind(decl.getOperand(4)) != ResourceKind::eImage2D) continue;
      uint32_t slot = uint32_t(decl.getOperand(2));
      if (covering[slot]!=1) continue; // Aliased or indexed register ranges.
      bool safe = true;
      uint16_t samplerMask = 0;
      std::vector<Sample> found;
      auto uses = b.getUses(def);
      for (auto i = uses.first; i != uses.second; ++i) {
        auto load = *i;
        if (load.getOpCode() == OpCode::eDebugName) continue;
        if (load.getOpCode() != OpCode::eDescriptorLoad || !zero(SsaDef(load.getOperand(1)))) { safe = false; break; }
        auto reads = b.getUses(load.getDef());
        for (auto j = reads.first; j != reads.second; ++j) {
          auto op = *j;
          if (op.getOpCode() == OpCode::eDebugName) continue;
          // In the explicitly approximate mode, dimensions describe the atlas.
          // Keep the query itself intact, accepting mismatched texel math.
          if (approximate && op.getOpCode() == OpCode::eImageQuerySize) continue;
          if (op.getOpCode() != OpCode::eImageSample || op.getType() != Type(ScalarType::eF32, 4)
           || op.getOperand(2) || op.getOperand(10)
           || (!approximate && !zero(SsaDef(op.getOperand(5))))) { safe = false; break; }
          for (uint32_t k : {4u, 6u, 7u, 8u, 9u, 10u})
            if (!approximate && op.getOperand(k)) safe = false;
          auto sampler = b.getOp(SsaDef(op.getOperand(1)));
          if (sampler.getOpCode() != OpCode::eDescriptorLoad || !zero(SsaDef(sampler.getOperand(1)))) { safe = false; break; }
          auto sd = b.getOp(SsaDef(sampler.getOperand(0)));
          if (sd.getOpCode() != OpCode::eDclSampler || sd.getOperand(1) || uint32_t(sd.getOperand(2)) >= 16 || uint32_t(sd.getOperand(3)) != 1) { safe = false; break; }
          samplerMask |= uint16_t(1u << uint32_t(sd.getOperand(2)));
          found.push_back({op.getDef(), slot});
        }
        if (!safe) break;
      }
      if (!safe || !samplerMask) continue;
      entry = SsaDef(decl.getOperand(0));
      result[slot] = samplerMask;
      samples.insert(samples.end(), found.begin(), found.end());
    }
    if (samples.empty() || !rewrite) return result;
    auto cbv = b.add(Op::DclCbv(Type(ScalarType::eF32, 4).addArrayDimension(VirtualSlots*6),
      entry, 0, VirtualUniformSlot, 1));
    auto f2 = BasicType(ScalarType::eF32, 2), f4 = BasicType(ScalarType::eF32, 4);
    for (auto sample : samples) {
      auto op = b.getOp(sample.def);
      auto add = [&] (Op a) { return b.addBefore(sample.def, std::move(a)); };
      auto extract = [&] (SsaDef d, uint32_t n) { return add(Op::CompositeExtract(ScalarType::eF32, d, b.makeConstant(n))); };
      auto vec = [&] (SsaDef x, SsaDef y) { return add(Op::CompositeConstruct(f2, x, y)); };
      auto select = [&] (SsaDef c, SsaDef a, SsaDef z) {
        auto cond = add(Op::CompositeConstruct(BasicType(ScalarType::eBool,2), c,c));
        return add(Op::Select(f2,cond,a,z));
      };
      auto descriptor = add(Op::DescriptorLoad(ScalarType::eCbv, cbv, b.makeConstant(0u)));
      std::array<SsaDef,6> rows;
      for (uint32_t i=0;i<6;i++) rows[i]=add(Op::BufferLoad(f4,descriptor,b.makeConstant(sample.slot*6+i),16));
      auto original = SsaDef(op.getOperand(3));
      auto uv = add(Op::FClamp(f2,original,b.makeConstant(0.0f,0.0f),b.makeConstant(1.0f,1.0f)));
      auto halfPixel = add(Op::FMul(f2,vec(extract(rows[5],0),extract(rows[5],1)),b.makeConstant(.5f,.5f)));
      auto map = [&] (uint32_t i, SsaDef lo, SsaDef hi) {
        auto scale=vec(extract(rows[i],0),extract(rows[i],1));
        auto offset=vec(extract(rows[i],2),extract(rows[i],3));
        auto calc=[&](SsaDef p){return add(Op::FAdd(f2,add(Op::FMul(f2,p,scale)),offset));};
        auto min=add(Op::FAdd(f2,calc(lo),halfPixel));
        auto max=add(Op::FSub(f2,calc(hi),halfPixel));
        max=add(Op::FMax(f2,min,max));
        return add(Op::FClamp(f2,calc(uv),min,max));
      };
      auto mapped=map(0,b.makeConstant(0.0f,0.0f),b.makeConstant(1.0f,1.0f));
      for(uint32_t eye=0;eye<2;eye++) {
        auto bounds=rows[3+eye];
        SsaDef inside;
        for(uint32_t axis=0;axis<2;axis++) {
          auto a=add(Op::FGe(ScalarType::eBool,extract(uv,axis),extract(bounds,axis)));
          auto z=add(Op::FLt(ScalarType::eBool,extract(uv,axis),extract(bounds,axis+2)));
          auto c=add(Op::BAnd(ScalarType::eBool,a,z));
          inside=inside?add(Op::BAnd(ScalarType::eBool,inside,c)):c;
        }
        auto high=map(1+eye,vec(extract(bounds,0),extract(bounds,1)),vec(extract(bounds,2),extract(bounds,3)));
        mapped=select(inside,high,mapped);
      }
      auto enabled=add(Op::FGt(ScalarType::eBool,extract(rows[5],0),b.makeConstant(0.0f)));
      op.setOperand(3,select(enabled,mapped,original));
      b.rewriteOp(sample.def,std::move(op));
    }
    return result;
  }
}
