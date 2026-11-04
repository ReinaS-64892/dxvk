#include "../src/dxvk/dxvk_qvfr_ir.h"
#include <ir/ir_legalize.h>
#include <spirv/spirv_builder.h>
#include <fstream>
#include <iostream>
#include <cstdlib>

using namespace dxbc_spv::ir;
static void check(bool c) { if(!c) std::abort(); }

static Builder shader(int variant) {
  Builder b;
  auto fn=b.add(Op::Function(Type())); b.add(Op::FunctionEnd());
  auto ep=b.add(Op::EntryPoint(fn,variant==5?ShaderStage::eVertex:ShaderStage::ePixel));
  b.setCursor(fn);
  auto input=b.add(Op::DclInput(Type(ScalarType::eF32,2),ep,0,0));
  auto output=b.add(Op::DclOutput(Type(ScalarType::eF32,4),ep,0,0));
  auto srv=b.add(Op::DclSrv(ScalarType::eF32,ep,0,3,variant==6?2:1,ResourceKind::eImage2D));
  auto smp=b.add(Op::DclSampler(ep,0,2,1));
  auto image=b.add(Op::DescriptorLoad(ScalarType::eSrv,srv,b.makeConstant(0u)));
  auto sampler=b.add(Op::DescriptorLoad(ScalarType::eSampler,smp,b.makeConstant(0u)));
  auto uv=b.add(Op::InputLoad(Type(ScalarType::eF32,2),input,SsaDef()));
  auto offset=variant==2?b.makeConstant(1,0):SsaDef();
  auto grad=variant==3?b.makeConstant(.1f,.1f):SsaDef();
  auto color=b.add(Op::ImageSample(Type(ScalarType::eF32,4),image,sampler,SsaDef(),uv,offset,SsaDef(),SsaDef(),SsaDef(),grad,grad,SsaDef()));
  if(variant==1) {
    auto load=b.add(Op::ImageLoad(Type(ScalarType::eF32,4),image,b.makeConstant(0u),SsaDef(),b.makeConstant(0,0),SsaDef(),SsaDef()));
    color=b.add(Op::FAdd(Type(ScalarType::eF32,4),color,load));
  }
  if(variant==4) {
    auto sizeType=Type().addStructMember(BasicType(ScalarType::eU32,2)).addStructMember(ScalarType::eU32);
    auto size=b.add(Op::ImageQuerySize(sizeType,image,b.makeConstant(0u)));
    auto xy=b.add(Op::CompositeExtract(BasicType(ScalarType::eU32,2),size,b.makeConstant(0u)));
    auto x=b.add(Op::CompositeExtract(ScalarType::eU32,xy,b.makeConstant(0u)));
    auto f=b.add(Op::ConvertItoF(ScalarType::eF32,x));
    auto scale=b.add(Op::CompositeConstruct(BasicType(ScalarType::eF32,4),f,f,f,f));
    color=b.add(Op::FMul(Type(ScalarType::eF32,4),color,scale));
  }
  if(variant==7) b.add(Op::DclSrv(ScalarType::eU32,ep,0,3,1,ResourceKind::eImage2D));
  b.add(Op::OutputStore(output,SsaDef(),color)); b.add(Op::Return());
  return b;
}

int main(int argc,char** argv) {
  for(bool approximate : {false,true}) for(int variant=0;variant<8;variant++) {
    auto b=shader(variant);
    uint32_t before=0, after=0;
    for(auto i=b.begin();i!=b.end();++i) before++;
    auto probe=dxvk::qvfr::lowerVirtualSamples(b,false,approximate);
    for(auto i=b.begin();i!=b.end();++i) after++;
    check(before==after);
    auto mask=dxvk::qvfr::lowerVirtualSamples(b,true,approximate);
    check(probe==mask);
    bool supported=variant==0 || (approximate && (variant==2 || variant==3 || variant==4));
    check(mask[3]==(supported?4:0));
    for(uint32_t i=0;i<128;i++) if(i!=3) check(!mask[i]);
    if(!supported) continue;
    CompileOptions opts;
    legalizeIr(b,opts);
    dxbc_spv::spirv::BasicResourceMapping mapping;
    dxbc_spv::spirv::SpirvBuilder::Options options;
    dxbc_spv::spirv::SpirvBuilder spirv(b,mapping,options);
    spirv.buildSpirvBinary();
    auto words=spirv.getSpirvBinary();
    check(!words.empty());
    if(argc>1) {
      auto path=std::string(argv[1])+(approximate?".unsafe-"+std::to_string(variant)+".spv":"");
      std::ofstream out(path,std::ios::binary);
      out.write(reinterpret_cast<const char*>(words.data()),words.size()*sizeof(uint32_t));
      check(bool(out));
    }
  }
  std::cout<<"QVFR virtual sampler: 16 conservative/approximate cases and SPIR-V generation passed\n";
}
