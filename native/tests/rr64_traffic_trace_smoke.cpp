#include "../src/rr64_engine_layout.hpp"
#include <vector>
extern "C" void rr64_traffic_trace(unsigned char*,unsigned,unsigned,unsigned,unsigned,unsigned);
int main() {
    using namespace rr64::engine;
    std::vector<unsigned char> memory(kRdramSize);
    constexpr unsigned node=0x80200000,entity=0x80201000;
    auto* m=memory.data();
    write_u32(m,node,4);write_u32(m,node+4,entity);
    write_float(m,entity+0xA8,12);write_float(m,entity+0xAC,34);write_float(m,entity+0xB0,56);
    const auto before=memory;
    rr64_traffic_trace(m,0,entity,1,2,3);
    for(unsigned i=0;i<1000;++i)rr64_traffic_trace(m,2,node,0,0,0);
    rr64_traffic_trace(m,3,node,0,0,0);
    rr64_traffic_trace(m,4,node,1,0,0);
    rr64_traffic_trace(m,2,node,1,0,0);
    rr64_traffic_trace(m,3,node,1,0,0);
    rr64_traffic_trace(m,4,node,0,0,0);
    rr64_traffic_trace(m,1,entity,0,0,0);
    return memory==before ? 0 : 1;
}
