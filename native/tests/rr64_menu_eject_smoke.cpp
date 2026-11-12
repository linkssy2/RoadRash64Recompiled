#include "../src/rr64_local_eject.hpp"
#include "../src/rr64_traffic_distance.hpp"
#include "../lib/rt64/src/common/rt64_rr64_menu_borders.h"
#include <vector>
#include <cstdio>

int main() {
    using namespace rr64::engine;
    std::vector<unsigned char> memory(kRdramSize);
    auto* m = memory.data();
    constexpr unsigned pool = 0x80100000;
    write_u32(m, globals::bike_pool_pointer, pool);
    write_u32(m, globals::active_racer_count, 8);
    write_u32(m, local_race::humans, 4);
    // Deliberately different from roster order: resolve the actual binding.
    for (unsigned slot = 1; slot < 4; ++slot)
        write_u32(m, 0x800D8570 + slot * 0x118 + 0xE0, pool + (5-slot)*bike::stride);
    for (unsigned slot = 0; slot < 4; ++slot) {
        unsigned found = 0;
        if (!rr64::local_eject::resolve_bike(m, slot, found) ||
            found != pool + (slot ? 5-slot : 0)*bike::stride) return 1;
    }
    unsigned found = 123;
    if (rr64::local_eject::resolve_bike(m, 4, found) || found != 123) return 2;
    write_u32(m, local_race::humans, 2);
    if (rr64::local_eject::resolve_bike(m, 2, found)) return 3;
    for (unsigned bad : {pool-1, pool+1, pool+8*bike::stride, 0u}) {
        write_u32(m, 0x800D8570 + 0x118 + 0xE0, bad);
        if (rr64::local_eject::resolve_bike(m, 1, found)) return 4;
    }
    write_u32(m, globals::active_racer_count, 15);
    if (rr64::local_eject::resolve_bike(m, 0, found)) return 5;
    using RR64MenuBorders::bounds;
    if (bounds(true,0,960,4,3) != std::pair<int,int>{120,840}) return 6;
    if (bounds(false,0,960,4,3) != std::pair<int,int>{0,960}) return 7;
    if (bounds(true,0,720,3,3) != std::pair<int,int>{0,720}) return 8;
    if (bounds(true,0,3440,43,24) != std::pair<int,int>{760,2680}) return 9;
    if (bounds(true,0,2560,16,9) != std::pair<int,int>{560,2000}) return 10;
    if (bounds(true,120,720,3,3) != std::pair<int,int>{120,840}) return 11;
    constexpr unsigned node=0x80200000, car=0x80201000;
    write_u32(m,globals::main_mode,0x09);
    write_u32(m,globals::pending_mode,0x09);
    write_u32(m,node+actor_scene::type,4);
    write_u32(m,node+actor_scene::entity,car);
    write_u16(m,car+traffic_scene::entity_active,1);
    for (unsigned count=1; count<=4; ++count) {
        write_u32(m,0x8009DB88,count);
        for (unsigned view=0; view<count; ++view) {
            write_u32(m,globals::active_viewport,view);
            if (!rr64::traffic::within_distance(m,node,400000000.0f,100000000.0f,false,100)) return 12;
            if (rr64::traffic::within_distance(m,node,400000000.0f,100000000.0f,false,0)) return 13;
            if (rr64::traffic::within_distance(m,node,400000000.0f,100000000.0f,false,25)) return 14;
            if (rr64::traffic::within_distance(m,node,961000000.0f,100000000.0f,false,100)) return 15;
        }
    }
    write_u16(m,car+traffic_scene::entity_active,0);
    if (rr64::traffic::within_distance(m,node,400000000.0f,100000000.0f,false,100)) return 16;
    if (!rr64::traffic::within_distance(nullptr,0,0,0,true,0)) return 17;
    write_u16(m,car+traffic_scene::entity_active,1);
    write_u32(m,globals::main_mode,1);
    write_u32(m,globals::pending_mode,1);
    if (rr64::traffic::within_distance(m,node,400000000.0f,100000000.0f,false,100)) return 18;
    using rr64::traffic::spawn_distance;
    if(spawn_distance(350,10000,100)!=1000 || spawn_distance(350,10000,50)!=675 ||
       spawn_distance(350,10000,0)!=350 || spawn_distance(350,10000,200)!=1000) return 19;
    if(spawn_distance(950,1000,100)!=975 || spawn_distance(1000,1000,100)!=1000 ||
       spawn_distance(1100,1000,100)!=1100 || spawn_distance(-1,1000,100)!=-1) return 20;
    // Across a range of course lengths and slider values, never overshoot
    // the original end guard or move a valid proposal backwards.
    for(float limit:{100.0f,1000.0f,10000.0f})
        for(int percent=0;percent<=100;percent+=5)
            for(int i=0;i<=100;++i){float before=limit*i/100;
                float after=spawn_distance(before,limit,percent);
                if(after<before || after>limit || after-before>650.01f)return 21;}
    std::puts("Passed: four local bindings, inactive/invalid ownership, native and ultrawide menu borders, race bypass.");
}
