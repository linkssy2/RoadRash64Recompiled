#include "rr64_video_mode.hpp"
#include "ultramodern/config.hpp"
#include <cstdio>
#include <cstdlib>

extern "C" void rr64_refresh_viewport_dimensions(unsigned char *memory, unsigned layout) {
    if (!rr64::video::refresh_viewport_dimensions(memory,layout)) return;
    static const bool trace=[] {
        const char *value=std::getenv("RR64_COURSE_DIAGNOSTICS");
        return value && value[0]=='1' && value[1]=='\0';
    }();
    static thread_local unsigned reports=0;
    if (!trace || reports>=48) return;
    unsigned width=0,height=0,cached_width=0,cached_height=0,mode=0;
    using rr64::engine::read_u32;
    read_u32(memory,0x800b0808u,width); read_u32(memory,0x800b080cu,height);
    read_u32(memory,0x800b74a8u,cached_width); read_u32(memory,0x800b74acu,cached_height);
    read_u32(memory,rr64::engine::globals::main_mode,mode);
    std::fprintf(stderr,"[RR64-VIEWPORT-REFRESH] mode=%u layout=%u framebuffer=%ux%u old-region=%ux%u sample=%u\n",
        mode,layout,width,height,cached_width,cached_height,++reports);
}

extern "C" unsigned int rr64_combined_video_callback(unsigned char *memory, unsigned int original) {
    return rr64::video::combined_callback(memory, original, true);
}
