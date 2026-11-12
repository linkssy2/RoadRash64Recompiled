#include "rr64_netplay.hpp"
#include <chrono>
#include <thread>
#include <string>
#include <cstdio>
int main(int argc,char**argv){
    if(argc!=3)return 2;
    bool host=std::string(argv[1])=="host", admitted_expected=std::string(argv[2])=="yes";
    rr64::netplay::Config c{};c.mode=host?rr64::netplay::Mode::Host:rr64::netplay::Mode::Join;c.port=26466;c.maximum_players=2;
    rr64::netplay::configure(c);bool admitted=false;unsigned peak=0;
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(host?6:(admitted_expected?5:3));
    while(std::chrono::steady_clock::now()<end){rr64::netplay::update();auto s=rr64::netplay::get_status();admitted|=s.connected;peak=std::max<unsigned>(peak,s.connected_players);std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    rr64::netplay::shutdown();bool ok=host?peak==2:admitted==admitted_expected;
    std::fprintf(stderr,"Online limit: host=%u admitted=%u peak=%u expected=%u pass=%u\n",host,admitted,peak,admitted_expected,ok);return ok?0:1;
}

