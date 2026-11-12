#include "../src/rr64_racer_view.hpp"
#include "../src/rr64_log_batch.hpp"
#include <cstdio>
#include <limits>
static std::string received;
static unsigned writes=0;
static void sink(std::string_view value){received.append(value);++writes;}
int main() {
    using namespace rr64::lod;
    const ViewPoint a{0,0},b{-10,10},c{10,10};
    for(float x:{-6.5f,6.5f}) {
        if(racer_view_contains({x,5},a,b,c,1))return 1;
        if(!racer_view_contains({x,5},a,b,c,1.5f))return 2;
    }
    for(auto p:{ViewPoint{0,-1},ViewPoint{0,11},ViewPoint{9,5}})
        if(racer_view_contains(p,a,b,c,1.5f))return 3;
    if(racer_view_contains({0,5},a,a,a,1.5f) ||
        racer_view_contains({std::numeric_limits<float>::quiet_NaN(),5},a,b,c,1.5f))return 4;
    for(float x=-4;x<=4;++x)
        if(!racer_view_contains({x,5},a,b,c,1) || !racer_view_contains({x,5},a,b,c,1.5f))return 5;
    {rr64::LogBatch batch(sink);for(int i=0;i<100;++i)batch.append("line\n");}
    if(writes!=1 || received.size()!=500)return 6;
    received.clear();writes=0;
    {rr64::LogBatch batch(sink);batch.append(std::string(60000,'a'));batch.append(std::string(60000,'b'));}
    if(writes!=2 || received!=std::string(60000,'a')+std::string(60000,'b'))return 7;
    std::puts("Racer view: both side margins, retained near/far rejection and invalid inputs passed. Diagnostic batching: preserved content and bounded flush passed.");
}
