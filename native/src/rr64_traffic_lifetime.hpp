#pragma once
#include "rr64_traffic_distance.hpp"
#include "rr64_traffic_roster.hpp"
#include <cmath>
#include <limits>

namespace rr64::traffic {
// Only native traffic owns these two query objects. A compacted roster index
// is authenticated before reading the car, so riders and stale allocations
// always retain their ordinary camera-independent collision support.
inline unsigned floor_owner(unsigned char* m, unsigned query) {
    using namespace engine;
    unsigned count=0;
    if (!read_u32(m,0x800A6528u,count) || count>capacity) return 0;
    for (unsigned offset : {0x238u,0x2A4u}) {
        if (query<offset) continue;
        const unsigned car=query-offset;
        unsigned index=0;
        if ((car&3) || !valid_guest_range(car,0x338) ||
            !read_u32(m,car+0x28,index) || index>=count || entity(m,index)!=car) continue;
        return car;
    }
    return 0;
}

// The original car updater retires traffic when its floor disappears. Keep
// immutable floors in every local view and for upcoming native traffic births;
// outside both sets, let that original cleanup run.
// This is stateless: a never-seen birth cannot acquire permanent exemption.
inline bool allow_floor_fallback(unsigned char* m, unsigned query) {
    using namespace engine;
    const unsigned car=floor_owner(m,query);
    if (!car) return true;

    unsigned views=0,width=0,grid=0,cell_size=0;
    float x=0,y=0,scale=0;
    if (!read_u32(m,0x8009DB88u,views) || views<1 || views>4 ||
        !read_u32(m,globals::terrain_map_width,width) || !width || width>terrain::maximum_map_width ||
        !read_u32(m,globals::terrain_cell_grid,grid) ||
        !valid_guest_range(grid,width*width*terrain::cell_stride) ||
        !read_u32(m,0x800DACD0u,cell_size) || !cell_size ||
        !read_float(m,car+0xD0,x) || !read_float(m,car+0xD4,y) ||
        !read_float(m,0x80007518u,scale) ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(scale) || scale<=0) return true;

    // 75EA0 uses the candidate center D0/D4 for BOTH axles, at scale four.
    // Testing that center gives both axles the same lifetime decision. Either
    // axle still on genuinely resident terrain also protects the whole car.
    const double half=double(width)*cell_size*0.5;
    const double row=std::floor((double(x)*scale+half)/cell_size);
    const double column=std::floor((double(y)*scale+half)/cell_size);
    std::array<std::array<double,2>,3> occupied{{{row,column},{row,column},{row,column}}};
    unsigned point=1;
    for (unsigned offset : {0x238u,0x2A4u}) {
        float qx=0,qy=0;
        if (!read_float(m,car+offset,qx) || !read_float(m,car+offset+4,qy) ||
            !std::isfinite(qx) || !std::isfinite(qy)) return true;
        const double qr=std::floor((double(qx)+half)/cell_size);
        const double qc=std::floor((double(qy)+half)/cell_size);
        occupied[point++]={qr,qc};
        if (qr>=0 && qr<width && qc>=0 && qc<width) {
            std::uint8_t state=0;
            if (read_u8(m,grid+(unsigned(qr)*width+unsigned(qc))*terrain::cell_stride+terrain::cell_state,state) &&
                state==terrain::ready_state) return true;
        }
    }

    bool incomplete=false;
    for (unsigned view=0;view<views;++view) {
        unsigned counter=0,count=0;
        if (!read_u32(m,0x800A7704u+view*4,counter) || !counter) { incomplete=true;continue; }
        // 7B8D4 increments the counter before filling its 31-cell record.
        // Each of three buffers has four FC-byte viewport records.
        const unsigned record=0x800DDEB0u+((counter-1)%3)*0x3F0u+view*0xFCu;
        if (!read_u32(m,record+terrain::candidate_count,count) || !count ||
            count>terrain::candidate_pointer_capacity) { incomplete=true;continue; }
        unsigned min_row=width,min_column=width,max_row=0,max_column=0;
        bool valid=true;
        for (unsigned i=0;i<count;++i) {
            unsigned cell=0;
            if (!read_u32(m,record+i*4,cell) || cell<grid || (cell-grid)%terrain::cell_stride ||
                (cell-grid)/terrain::cell_stride>=width*width) { valid=false;break; }
            const unsigned index=(cell-grid)/terrain::cell_stride;
            min_row=std::min(min_row,index/width);max_row=std::max(max_row,index/width);
            min_column=std::min(min_column,index%width);max_column=std::max(max_column,index%width);
        }
        if (!valid) { incomplete=true;continue; }
        for (const auto& p:occupied)
            if (p[0]>=min_row && p[0]<=max_row && p[1]>=min_column && p[1]<=max_column) return true;
    }
    // Race setup/viewport changes may not have produced every native record
    // yet. They must never remove a newly placed car on incomplete evidence.
    if (incomplete) return true;

    // 79334 schedules births at leading human progress +350; the maintained
    // spawn extension adds at most650. This forward interval protects fresh
    // unloaded cars without retaining departed queues behind the players.
    unsigned total=0;
    float car_route=0,end=0,birth_reach=0;
    if (!read_u32(m,0x800A656Cu,total) || !total || total>kMaximumRacers ||
        !read_float(m,car+0x24,car_route) || !read_float(m,0x800D762Cu,end) ||
        !read_float(m,0x80006B80u,birth_reach) || !std::isfinite(car_route) ||
        !std::isfinite(end) || !std::isfinite(birth_reach) || birth_reach<0) return true;
    for (unsigned slot=0;slot<total;++slot) {
        const unsigned actor=0x800D8570u+slot*0x118u;
        unsigned controller=0,progress=0,excluded=0;
        std::uint16_t active=0,ai=0,eligible=0,finished=0;
        if (!read_u16(m,actor+0x24,active) || !read_u16(m,actor+0x26,ai) ||
            !read_u32(m,actor+8,controller)) return true;
        if (!active || ai || controller>=4) continue;
        if (!read_u32(m,actor+0xE8,progress) || !valid_guest_range(progress,0x54) ||
            !read_u16(m,progress+0x48,eligible) || !read_u32(m,progress+0x4C,excluded) ||
            !read_u16(m,progress+0x50,finished)) return true;
        if (!eligible || excluded || finished) continue;
        float base=0,t=0,length=0;
        if (!read_float(m,progress+0x20,base) || !read_float(m,progress+8,t) ||
            !read_float(m,progress+0xC,length) || !std::isfinite(base) ||
            !std::isfinite(t) || !std::isfinite(length) || length<0) return true;
        // 68D0C adds lap*lap_length only for standings. Traffic stores the
        // local coordinate, so compare before that addition. Reproduce its
        // one end-crossing subtraction, never wrap the upcoming interval.
        float local=base+std::clamp(t,0.0f,1.0f)*length;
        if (!std::isfinite(local)) return true;
        if (local>end) {
            float lap_length=0;
            if (!read_float(m,0x800D7630u,lap_length) || !std::isfinite(lap_length) || lap_length<=0) return true;
            local=(base-lap_length)+std::clamp(t,0.0f,1.0f)*length;
            if (!std::isfinite(local)) return true;
        }
        const double reach=double(birth_reach)+maximum_spawn_extension;
        // Birth scheduling and native segment reconstruction use several
        // float operations. Admit their rounding error, not an extra road gap.
        const double rounding=8.0*std::numeric_limits<float>::epsilon()*
            std::max({1.0,std::abs(double(local)),std::abs(double(car_route)),std::abs(double(end)),reach});
        if (car_route>=double(local)-rounding && car_route<=double(end)+rounding &&
            double(car_route)-local<=reach+rounding) return true;
    }
    return false;
}
}
