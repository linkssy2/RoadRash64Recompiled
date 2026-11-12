#pragma once
#include "rr64_prediction_frame.hpp"
#include <fstream>
#include <filesystem>
#include <type_traits>

namespace rr64::prediction {
// Private, same-ABI diagnostic artifacts. These include loaded game data and
// must never enter test-log ZIPs or distributable packages. Own ROM required.
struct ReplayCase {
    struct Header {
        std::uint64_t magic=0x3153414334365252ull,rom_hash=0;
        std::uint32_t version=6,input_size=sizeof(FrameInput),image_size=engine::kRdramSize;
        std::uint32_t local=0,humans=0,mapped=0,attempt=0,category=0;
        std::uint32_t native_size=sizeof(FrameOutput),native_valid=1;
    } header;
    FrameInput input{};
    FrameOutput expected_native{};
    std::vector<unsigned char> before_private,before_live,after_live;
};
static_assert(std::is_trivially_copyable_v<FrameInput>);
inline std::uint64_t case_rom_hash(std::span<const unsigned char> rom){
    std::uint64_t hash=14695981039346656037ull;
    for(auto byte:rom){hash^=byte;hash*=1099511628211ull;}return hash;
}
inline bool write_case(const std::filesystem::path& path,const ReplayCase& c){
    if(c.before_private.size()!=engine::kRdramSize || c.before_live.size()!=engine::kRdramSize || c.after_live.size()!=engine::kRdramSize)return false;
    const auto temporary=std::filesystem::path(path.string()+".partial");
    if(std::filesystem::exists(path) || std::filesystem::exists(temporary))return false;
    std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&c.header),sizeof(c.header));
    out.write(reinterpret_cast<const char*>(&c.input),sizeof(c.input));
    out.write(reinterpret_cast<const char*>(&c.expected_native),sizeof(c.expected_native));
    for(const auto* image:{&c.before_private,&c.before_live,&c.after_live})out.write(reinterpret_cast<const char*>(image->data()),image->size());
    out.flush();if(!out)return false;out.close();
    std::filesystem::rename(temporary,path);return true;
}
inline bool read_case(const std::filesystem::path& path,ReplayCase& out){
    std::error_code error;
    if(std::filesystem::file_size(path,error)!=sizeof(ReplayCase::Header)+sizeof(FrameInput)+sizeof(FrameOutput)+3ull*engine::kRdramSize || error)return false;
    std::ifstream in(path,std::ios::binary);ReplayCase c;
    in.read(reinterpret_cast<char*>(&c.header),sizeof(c.header));
    if(!in || c.header.magic!=ReplayCase::Header{}.magic || c.header.version!=6 ||
       c.header.input_size!=sizeof(FrameInput) || c.header.image_size!=engine::kRdramSize ||
       c.header.native_size!=sizeof(FrameOutput) || c.header.native_valid>1 ||
       c.header.local>=14 || c.header.mapped>1 || c.header.humans>=1u<<14)return false;
    in.read(reinterpret_cast<char*>(&c.input),sizeof(c.input));
    in.read(reinterpret_cast<char*>(&c.expected_native),sizeof(c.expected_native));
    for(auto* image:{&c.before_private,&c.before_live,&c.after_live}){
        image->resize(engine::kRdramSize);in.read(reinterpret_cast<char*>(image->data()),image->size());
        if(!in)return false;
    }
    if(in.peek()!=std::char_traits<char>::eof())return false;
    out=std::move(c);return true;
}
}
