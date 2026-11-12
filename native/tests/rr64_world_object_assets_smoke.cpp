#include "rr64_world_object_assets.hpp"
#include "rr64_world_objects.hpp"
#include "recomp.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void func_8000CD34(std::uint8_t*,recomp_context*) {
    std::fputs("FAIL: object cache called the original loader\n",stderr);std::abort();
}
extern "C" void func_8005F15C(std::uint8_t*,recomp_context*);
extern "C" void func_8005F21C(std::uint8_t*,recomp_context*);
extern "C" void func_80015A90(std::uint8_t*,recomp_context*);
namespace {
int failures=0;
void check(bool ok,const char* message) { if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",message);} }
std::uint32_t word(std::span<const std::uint8_t> b,std::size_t p) {
    return(std::uint32_t(b[p])<<24u)|(std::uint32_t(b[p+1])<<16u)|(std::uint32_t(b[p+2])<<8u)|b[p+3];
}
std::uint16_t half(std::span<const std::uint8_t> b,std::size_t p) {
    return std::uint16_t((std::uint32_t(b[p])<<8u)|b[p+1]);
}
void put(std::vector<std::uint8_t>& b,std::size_t p,std::uint32_t v) {
    for(std::size_t i=0;i<4;++i){b[p+i]=std::uint8_t(v>>(24u-i*8u));}
}
void put_half(std::vector<std::uint8_t>& b,std::size_t p,std::uint16_t v) {
    b[p]=std::uint8_t(v>>8u);b[p+1]=std::uint8_t(v);
}
void refusal(const std::vector<std::uint8_t>& changed,const char* name) {
    rr64::world::ObjectAssets output;output.bytes={1u,9u,7u};output.vertices=123u;
    std::string error;check(!rr64::world::build_object_assets(changed,output,error),name);
    check(output.bytes==std::vector<std::uint8_t>({1u,9u,7u})&&output.vertices==123u&&!error.empty(),
        "refused build preserves caller output atomically");
}
void root_oracle(const std::vector<std::uint8_t>& rom) {
    std::vector<std::uint8_t> memory(8u*1024u*1024u);
    for(std::size_t i=0;i<0xa0000u;++i){memory[i^3u]=rom[0xc00u+i];}
    const auto write=[&](std::uint32_t p,std::uint32_t v){std::memcpy(memory.data()+(p&0x7fffffu),&v,4);};
    const auto number=[&](std::uint32_t p,float v){write(p,std::bit_cast<std::uint32_t>(v));};
    const auto read=[&](std::uint32_t p){float v;std::memcpy(&v,memory.data()+(p&0x7fffffu),4);return v;};
    constexpr std::uint32_t graph=0x80410000u,pose=0x80410100u,placement=0x80410200u;
    write(graph+0xcu,pose);number(0x8009dbb4u,10.0f);
    const std::array<float,3> origin{1000,-700,150},camera{30,-10,5},position{1020,-620,175};
    for(std::size_t k=0;k<3;++k){number(0x800a4fdcu+std::uint32_t(k)*4u,origin[k]);
        number(0x800d69f8u+std::uint32_t(k)*4u,camera[k]);number(placement+0x10u+std::uint32_t(k)*4u,position[k]);}
    number(placement,0.2f);number(placement+4u,-0.4f);number(placement+8u,0.4f);number(placement+12u,0.8f);
    const auto run=[&](bool billboard){
        recomp_context ctx{};ctx.r4=static_cast<std::int32_t>(graph);ctx.r5=static_cast<std::int32_t>(placement+0x10u);
        ctx.r6=static_cast<std::int32_t>(placement);ctx.r7=2u;ctx.r29=static_cast<std::int32_t>(0x807ff000u);
        ctx.f_odd=&ctx.f0.u32h;
        if(billboard){func_8005F21C(memory.data(),&ctx);}else{func_8005F15C(memory.data(),&ctx);}
        for(std::size_t k=0;k<3;++k){check(read(pose+std::uint32_t(k)*4u)==(position[k]-origin[k]-camera[k])*10.0f,
            "actual object root uses placement minus sector and rebased camera, source2 scale10");}
        ctx.r4=static_cast<std::int32_t>(pose+12u);ctx.r5=static_cast<std::int32_t>(pose);
        ctx.r6=static_cast<std::int32_t>(0x80410300u);func_80015A90(memory.data(),&ctx);
        rr64::world::ObjectPlacementAsset native;native.position=position;native.quaternion={0.2f,-0.4f,0.4f,0.8f};native.billboard=billboard;
        rr64::world::ObjectMatrix matrix{};
        check(rr64::world::object_matrix(native,camera,origin,{read(0x800d6a28u),read(0x800d6a2cu)},matrix),
            "production object root matrix accepts original finite inputs");
        for(std::size_t k=0;k<16;++k){check(std::abs(matrix[k]-read(0x80410300u+std::uint32_t(k)*4u))<0.00001f,
            "complete production object matrix equals original5F15C/5F21C plus15A90");}
    };
    run(false);check(read(pose+12u)==0.2f&&read(pose+16u)==-0.4f&&read(pose+20u)==0.4f&&read(pose+24u)==0.8f,
        "static original root preserves authored tilt on every quaternion axis");
    for(const auto delta:std::array<std::array<float,2>,5>{{{0,0},{30,40},{-30,40},{30,-40},{-30,-40}}}){
        number(0x800d6a28u,position[0]-origin[0]+delta[0]);number(0x800d6a2cu,position[1]-origin[1]+delta[1]);
        run(true);const auto length=std::sqrt(delta[0]*delta[0]+delta[1]*delta[1]);
        const auto w=length==0?1.0f:std::sqrt(0.5f+0.5f*delta[0]/length);
        const auto z=length==0?0.0f:(delta[1]<0?-1.0f:1.0f)*std::sqrt(0.5f-0.5f*delta[0]/length);
        check(read(pose+12u)==0.0f&&read(pose+16u)==0.0f&&std::abs(read(pose+20u)-z)<0.00001f&&
            std::abs(read(pose+24u)-w)<0.00001f,"actual billboard quaternion matches every direction quadrant and coincidence");
    }
}
void packed_bounds(const rr64::world::ObjectAssets& assets) {
    using Matrix=std::array<float,16>;
    const Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::unordered_map<std::uint32_t,std::uint32_t> targets;
    for(const auto& r:assets.relocations){targets.emplace(r.word_offset,r.target_offset);}
    unsigned observed=0u;bool contained=true;
    // Independently consume the emitted GBI and packed uploads, not the source
    // graph or the builder's intermediate matrices. This is the GPU's input.
    for(const auto& model:assets.models){
        std::vector<Matrix> stack{identity};
        for(auto p=model.display_list_offset;p<model.display_list_offset+model.display_list_size;p+=8u){
            const auto op=word(assets.bytes,p);
            if(op==0xda380000u){
                const auto target=targets.at(p+4u);Matrix local{},combined{};
                for(std::size_t k=0;k<16;++k){local[k]=float(std::bit_cast<std::int16_t>(half(assets.bytes,target+k*2u)))+
                    float(half(assets.bytes,target+32u+k*2u))/65536.0f;}
                for(std::size_t r=0;r<4;++r)for(std::size_t c=0;c<4;++c)for(std::size_t k=0;k<4;++k){
                    combined[r*4u+c]+=local[r*4u+k]*stack.back()[k*4u+c];}
                stack.push_back(combined);
            }else if(op==0xd8380002u){check(stack.size()>1u,"packed bounds matrix pop has matching push");if(stack.size()>1u){stack.pop_back();}}
            else if((op>>24u)==1u){
                const auto target=targets.at(p+4u),count=(op>>12u)&0xffu;
                for(unsigned v=0;v<count;++v){
                    std::array<float,3> point{};for(std::size_t k=0;k<3;++k){
                        point[k]=std::bit_cast<std::int16_t>(half(assets.bytes,target+v*16u+k*2u));}
                    for(std::size_t k=0;k<3;++k){const auto& m=stack.back();
                        const auto value=point[0]*m[k]+point[1]*m[4u+k]+point[2]*m[8u+k]+m[12u+k];
                        contained&=value>=model.minimum[k]-0.00001f&&value<=model.maximum[k]+0.00001f;}
                    ++observed;
                }
            }
        }
    }
    check(contained&&observed==50391u,"culling bounds contain every vertex transformed by emitted packed child matrices");
}
}
int main(int argc,char** argv) {
    if(argc!=2){std::fputs("Usage: RR64WorldObjectAssetsSmoke <supported ROM>\n",stderr);return 2;}
    std::ifstream file(argv[1],std::ios::binary);
    const std::vector<std::uint8_t> rom{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    check(rom.size()==0x2000000u,"test loaded complete local ROM");if(failures){return 1;}
    root_oracle(rom);
    rr64::world::ObjectAssets assets;std::string error;
    if(!rr64::world::build_object_assets(rom,assets,error)){
        std::fprintf(stderr,"FAIL: real object asset build: %s\n",error.c_str());return 1;
    }
    packed_bounds(assets);
    check(assets.models.size()==373u&&assets.placements.size()==4257u,"complete original object model/placement inventory");
    check(assets.vertices==50391u&&assets.triangles==39925u&&assets.packets==1876u,
        "all original rendered mesh packets and geometry preserved");
    check(assets.models[163].vertices==0u&&assets.models[250].vertices==0u&&
        std::count_if(assets.models.begin(),assets.models.end(),[](const auto& m){return m.vertices==0u;})==2,
        "original empty roots163 and250 stay empty without shifting model identities");
    unsigned bank1=0u,bank2=0u,billboards=0u,pushes=0u,pops=0u,vertex_commands=0u;
    for(const auto& m:assets.models){
        bank1+=m.source_bank==1u;bank2+=m.source_bank==2u;
        int depth=0;
        check(m.display_list_offset+m.display_list_size<=assets.bytes.size(),"model display list stays in upload");
        for(auto p=m.display_list_offset;p<m.display_list_offset+m.display_list_size;p+=8u){
            const auto op=word(assets.bytes,p);
            if(op==0xda380000u){++pushes;++depth;}
            if(op==0xd8380002u){++pops;--depth;check(word(assets.bytes,p+4u)==64u,"one original child matrix popped");}
            if((op>>24u)==1u){++vertex_commands;}
            check(depth>=0&&depth<=16,"bounded child hierarchy stays balanced");
        }
        check(depth==0&&word(assets.bytes,m.display_list_offset+m.display_list_size-8u)==0xdf000000u,
            "model returns with original root matrix current");
        for(std::size_t k=0;k<3;++k){check(m.minimum[k]<=m.maximum[k],"model bounds contain transformed vertices");}
    }
    check(bank1==37u&&bank2==336u&&pushes==437u&&pops==437u&&vertex_commands==1876u,
        "source banks and all original static child transform scopes preserved");
    for(const auto& p:assets.placements){
        billboards+=p.billboard;check(p.model_index<373u&&p.cell_index<4900u&&p.placement_index<32u,
            "placement identity and model valid");
        for(std::size_t k=0;k<3;++k){check(std::bit_cast<std::uint32_t>(p.position[k])==
            word(rom,p.raw_rom_offset+0x10u+k*4u),"authored placement world coordinates preserved exactly");}
    }
    check(billboards==1958u,"all camera-facing placements remain dynamic metadata");
    for(const auto& r:assets.relocations){
        check(r.word_offset+4u<=assets.bytes.size()&&r.target_offset<assets.bytes.size(),"all GPU references belong to upload");
        check(word(assets.bytes,r.word_offset)==0u,"relocation starts without scratch guest pointer");
        check(r.texture_index==0xffffffffu||r.texture_index<assets.textures.size(),"texture relocation has valid animation owner");
    }
    unsigned animated=0u;
    for(const auto& t:assets.textures){
        animated+=(t.flags&4u)!=0u;
        check(t.model_index<assets.models.size()&&t.model_local_offset+0x40u<
            word(rom,assets.models[t.model_index].raw_rom_offset+4u),"resident texture phase metadata remains inside owning model");
    }
    check(assets.textures.size()==612u&&animated==8u,"original embedded static and animated texture inventory");
    const auto first=std::size_t(0x101d480u)+word(rom,0x101d480u);
    const auto root=first+8u*half(rom,first+0xeu);
    const auto child=root+0x40u+word(rom,root+0x40u);
    check(word(rom,child)==0x12u,"first fixture has original static child");
    auto changed=rom;put(changed,0x101d480u,0xfffffff0u);refusal(changed,"invalid model reference rejected");
    changed=rom;put_half(changed,root+0x12u,0u);refusal(changed,"unsupported root source rejected");
    changed=rom;put_half(changed,child+0x16u,1u);refusal(changed,"dynamic child cannot be frozen");
    changed=rom;put(changed,child+0x38u,0x7fc00000u);refusal(changed,"nonfinite child translation rejected");
    changed=rom;put(changed,child+0x34u,0u);refusal(changed,"invalid child quaternion rejected");
    changed=rom;put_half(changed,assets.placements.front().raw_rom_offset+0x28u,0xffffu);refusal(changed,"invalid placement model descriptor rejected");
    changed=rom;put(changed,assets.placements.front().raw_rom_offset+0x10u,0x7f800000u);refusal(changed,"nonfinite placement rejected");
    auto mesh=child;
    for(unsigned depth=0;word(rom,mesh)!=0x10u&&depth<16u;++depth){
        const auto type=word(rom,mesh);
        const auto entry=type==0x12u?mesh+0x58u+24u*half(rom,mesh+0x18u)+16u*half(rom,mesh+0x20u):mesh+0x38u;
        mesh=entry+word(rom,entry);
    }
    check(word(rom,mesh)==0x10u&&half(rom,mesh+0xcu)!=0u,"first model has original mesh packet oracle");
    changed=rom;put_half(changed,mesh+0x2au,33u);refusal(changed,"oversized object vertex packet rejected");
    changed=rom;put_half(changed,mesh+0x30u+half(rom,mesh+0x2eu),0x7fffu);
    refusal(changed,"object triangle outside vertex packet rejected before writer");
    if(failures){return 1;}
    std::printf("PASS: original object writers; 373 models,4257 placements,1958 billboards,437 balanced child scopes,1876 packets,50391 vertices,39925 triangles; upload=%zu bytes, textures=%zu; malformed data refuses atomically.\n",
        assets.bytes.size(),assets.textures.size());return 0;
}
