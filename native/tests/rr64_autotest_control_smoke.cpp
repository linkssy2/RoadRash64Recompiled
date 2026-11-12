#include "rr64_autotest_control.hpp"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
void check(bool ok,const char* message="autotest control check failed"){if(!ok)throw std::runtime_error(message);}
void environment(const char* name,const char* value){
#ifdef _WIN32
    _putenv_s(name,value);
#else
    setenv(name,value,1);
#endif
}
void parser_and_ports(){
    using namespace rr64::autotest;
    for(const auto* text:{"1 200 8000 -80 80", "1 200 8000 -80 80 0", "1 200 8000 -80 80 0 0"}){
        std::istringstream file(text);Control c;unsigned duration=0;
        check(read_control(file,c,duration) && c.port==0 && c.buttons==0x8000 && duration==200,"legacy port zero");
    }
    for(const auto* text:{"1 100 8000 0 0 0 4", "1 100 8000 0 0 0 -1", "1 100 8000 0 0 1 1",
        "1 100 8000 0 0 0 1 extra", "1 100 8000 0 0 0 broken", "-1 100 8000 0 0", "0 100 8000 0 0",
        "1 0 8000 0 0", "1 30001 8000 0 0", "1 100 10000 0 0", "1 100 8000 -81 0", "1 100 8000 0 81",
        "1 -4294967295 8000 0 0", "1 100 -ffffffff 0 0", "1 100 8000 0 0 -4294967296",
        "1 100 8000 0 0 0 -4294967295"}){
        std::istringstream file(text);Control c{99};unsigned duration=17;
        check(!read_control(file,c,duration) && c.serial==99 && duration==17,"reject malformed command without mutation");
    }
    std::vector<unsigned char> memory(rr64::engine::kRdramSize,0xA5);
    const auto original=memory;
    auto half=[&](unsigned address){std::uint16_t value=0;check(rr64::engine::read_u16(memory.data(),address,value));return value;};
    auto byte=[&](unsigned address){std::int8_t value=0;check(rr64::engine::read_s8(memory.data(),address,value));return value;};
    ControllerInput input;
    Control c{1,0x8000,12,-25,0,0};
    input.apply(memory.data(),c);
    check(half(0x8009CD90)==0x8000 && half(0x8009CD98)==0x8000 && half(0x8009CDA8)==0x8000,"P1 press edge");
    input.apply(memory.data(),c);check(half(0x8009CD98)==0 && half(0x8009CDA8)==0,"held input is not another press");
    c.port=1;input.apply(memory.data(),c);
    check(half(0x8009CD90)==0 && half(0x8009CD98)==0x8000 && half(0x8009CDA8)==0 &&
          byte(0x8009CDA0)==0 && byte(0x8009CDA4)==0,"switching releases P1 buttons and stick");
    check(half(0x8009CD92)==0x8000 && half(0x8009CD9A)==0x8000 && half(0x8009CDAA)==0x8000 &&
          byte(0x8009CDA1)==12 && byte(0x8009CDA5)==-25,"P2 uses separate native arrays and edge history");
    Control neutral;neutral.port=1;input.apply(memory.data(),neutral);
    check(half(0x8009CD92)==0 && half(0x8009CD9A)==0x8000 && half(0x8009CDAA)==0 &&
          byte(0x8009CDA1)==0 && byte(0x8009CDA5)==0,"neutral releases selected port");
    input.apply(memory.data(),neutral);check(half(0x8009CD9A)==0,"release edge occurs once");
    check(half(0x8009CD94)==0xA5A5 && half(0x8009CD96)==0xA5A5,"unowned ports remain untouched");
    const auto before_invalid=memory;c.port=4;input.apply(memory.data(),c);input.apply(nullptr,c);
    check(memory==before_invalid,"out of bounds port cannot write");
    c.port=3;input.apply(memory.data(),c);
    check(half(0x8009CD96)==0x8000 && half(0x8009CD9E)==0x8000 && half(0x8009CDAE)==0x8000 &&
          byte(0x8009CDA3)==12 && byte(0x8009CDA7)==-25,"last valid port remains in array bounds");
    for(std::size_t i=0;i<memory.size();++i){
        const unsigned address=rr64::engine::kRdramBegin+unsigned(i);
        const bool allowed=(address>=0x8009CD90 && address<0x8009CDB0);
        check(allowed || memory[i]==original[i],"controller input cannot change other game state");
    }
}
int main(int argc,char**argv){
    check(argc==2);
    parser_and_ports();
    const std::filesystem::path path=argv[1];
    check(!std::filesystem::exists(path));
    environment("RR64_AUTOTEST_CONTROL",path.string().c_str());
    environment("RR64_AUTOTEST","0");
    {rr64::autotest::ControlFile disabled;check(!disabled.enabled());}
    environment("RR64_AUTOTEST","1");
    const auto write=[&](const char* text){
        for(int tries=0;tries<50;++tries){
            {std::ofstream out(path);if(out){out<<text;return;}}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("cannot write test input");
    };
    {
        rr64::autotest::ControlFile control;check(control.enabled());
        auto await_serial=[&](unsigned serial){
            for(int i=0;i<100;++i){
                auto sample=control.sample();if(sample.serial==serial)return sample;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            throw std::runtime_error("input reader timeout");
        };
        write("1 200 2000 -15 20\n");
        auto first=await_serial(1);check(first.buttons==0x2000 && first.x==-15 && first.y==20 && !first.actions);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(control.sample().serial==0); // stale file cannot re-arm expired input
        write("2 500 0000 0 0 4\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        check(control.sample().serial==0); // unsupported action rejected
        write("2 500 0000 0 0 1\n");
        check(await_serial(2).actions==1); // rejected serial was not consumed
        write("3 500 0000 81 0 1\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        check(control.sample().serial==2); // malformed command cannot replace valid one
        write("3 50 0000 0 0 0\n");check(!await_serial(3).actions);
        write("4 500 0000 0 0 broken\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        check(control.sample().serial==0);
        write("4 500 0000 0 0 2\n");check(await_serial(4).actions==2);
        write("5 500 0000 0 0 3\n");check(await_serial(5).actions==3);
        write("6 500 8000 0 0 0 4\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));check(control.sample().serial==5);
        write("6 100 8000 8 -9 0 1\n");
        auto second=await_serial(6);check(second.port==1 && second.buttons==0x8000 && second.x==8 && second.y==-9);
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        auto neutral=control.sample();
        check(!neutral.serial && neutral.port==1 && !neutral.buttons && !neutral.x && !neutral.y && !neutral.actions,
              "expiry neutralizes P2 rather than reverting to P1");
    } // stop and join the reader before releasing its state
    std::filesystem::remove(path);
    std::cout<<"Autotest controls passed: disabled path, legacy input, per-port edges, bounded writes, expiry, rejection, action, shutdown\n";
}
