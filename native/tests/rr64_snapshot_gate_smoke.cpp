#include "ultramodern/rr64_snapshot_gate.hpp"
#include <array>
#include <thread>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <cstdio>
int main(){
    ultramodern::rr64::SnapshotGate gate;
    std::array<unsigned char,65536> memory{},copy{};
    std::atomic<bool> stop=false;
    std::barrier rendezvous(3);
    auto writer=[&](unsigned half){
        {auto access=gate.worker();rendezvous.arrive_and_wait();} // workers can overlap
        unsigned char value=0;
        while(!stop.load()){
            auto access=gate.worker();++value;
            for(unsigned i=half*32768;i<(half+1)*32768;++i)memory[i]=value;
        }
    };
    std::thread a(writer,0),b(writer,1);
    // Do not request exclusive access until both overlap-test readers arrived:
    // a writer-preferring shared_mutex could otherwise block the second reader
    // while the first is waiting at the barrier. The gate has no such barrier.
    rendezvous.arrive_and_wait();
    for(unsigned n=0;n<1000;++n){
        if(!gate.copy(memory.data(),copy.data(),memory.size()))std::abort();
        for(unsigned half=0;half<2;++half)
            for(unsigned i=half*32768;i<(half+1)*32768;++i)
                if(copy[i]!=copy[half*32768])std::abort();
    }
    stop=true;a.join();b.join();
    if(gate.copy(memory.data(),memory.data()+1,1024) || gate.copy(nullptr,copy.data(),1))std::abort();
    std::puts("1000 snapshots coherent with concurrent disjoint workers; overlapping destination rejected");
}
