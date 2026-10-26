#include "rr64_ground_contact.hpp"
#include <cstdio>
#include <initializer_list>
#include <limits>
int main() {
    using rr64::ground_contact::support_force;
    unsigned failures=0;
    auto check=[&](bool ok){if(!ok)++failures;};
    for(float dt:{0.0f,-1.0f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),1.0f/30,1.0f/20})
        check(support_force(-100,dt)==-100);
    for(float dt:{1.0f/240,1.0f/120,1.0f/60,1.0f/30}) {
        // The native force has inverse-dt scaling; normalized force should be constant.
        check(std::abs(support_force(-2.0f/dt,dt)+60.0f)<0.0001f);
        check(support_force(100,dt)==100);
        check(support_force(0,dt)==0);
    }
    check(std::isnan(support_force(std::numeric_limits<float>::quiet_NaN(),1.0f/60)));
    using rr64::ground_contact::downward_scale;
    using rr64::ground_contact::wheel_gap;
    for(float dt:{0.0f,-1.0f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),1.0f/30,1.0f/20})
        check(downward_scale(dt)==1.0f);
    for(int steps:{1,2,4,8}) {
        const float scale=downward_scale(1.0f/(30*steps));
        float gap=-0.2f;
        for(int i=0;i<steps;++i)gap-=0.5f*wheel_gap(gap,scale);
        check(std::abs(gap+0.1f)<0.000001f);
        check(wheel_gap(0.2f,scale)==0.2f);
        check(wheel_gap(0.0f,scale)==0.0f);
    }
    std::printf("Ground contact force: %u failures\n",failures);
    return failures?1:0;
}
