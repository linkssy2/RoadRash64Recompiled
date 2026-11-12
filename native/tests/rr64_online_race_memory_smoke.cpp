#include "rr64_netplay.hpp"
#include "rr64_prediction_cop_state.hpp"
#include <vector>
#include <cstdlib>
namespace { bool npc_fixture=false,gate_released=false,gate_loaded=false; unsigned finish_mode=0; rr64::netplay::Status status; void check(bool b){if(!b)std::abort();} }
namespace { bool authority_fixture=false;rr64::netplay::AuthorityFrame supplied_frame; }
namespace rr64::netplay {
Status get_status(){return status;}
// This fixture exercises the legacy apply path. New authority capture must
// remain unavailable here rather than fabricating a host frame.
bool authority_get_frame(AuthorityFrame &out){if(!authority_fixture)return false;out=supplied_frame;return true;}
bool authority_publish_frame(const AuthorityFrame &){return false;}
bool authority_race_gate(bool loaded){gate_loaded=loaded;return gate_released && loaded;}
bool authority_start(){status.authoritative=true;status.authority_humans=3;return true;}
bool authority_set_finish_mode(unsigned){return false;}
unsigned authority_finish_mode(){return finish_mode;}
void shutdown(){status={};}
bool get_rider_state(std::uint8_t slot,RiderState &r){
    r.root.bike_origin={10.f+slot,20.f,30.f};r.root.bike_velocity={40.f,50.f+slot,60.f};r.root.bike_motion={70.f,80.f,90.f+slot};r.root.rider_velocity={100.f,110.f+slot,120.f};
    r.host_ai=npc_fixture; r.character=3; r.bike=2;
    r.root.rider_anchor={130.f+slot,140.f,150.f};
    r.root.bike_display_angles={0.125f+slot,-0.25f,0.75f};
    r.root.attack={1,7,1,0,{0.5f,0.25f,-1.f,0,0,0}};
    r.weapon=5;r.root.equipment_valid=1;r.root.inventory[5]=3;r.root.inventory[1]=1;
    r.root.rider_impact_reserve=27.5f+slot;r.root.durability=87.25f+slot; r.root.durability_capacity=150.5f;
    r.root.valid=1;r.root.bike_height=300+slot;r.root.rider_height=400+slot;r.root.bike_rotation={0,0,0,1};r.root.rider_rotation={0,0,1,0};r.root.bike_attached=r.root.rider_attached=slot%2;r.root.ejected=1-slot%2;
    r.rider_position_valid=1;r.rider_x=200+slot;r.active=true;r.position_x=100+slot;r.front_wheel_x=101+slot;r.rear_wheel_x=99+slot;return true;
}

bool get_player_input(std::uint8_t,std::uint16_t &,float &,float &){return false;}
void set_local_rider_state(const RiderState &){}
bool set_host_world_state(const world_sync::Snapshot &){return true;}
bool set_host_ai_rider_state(std::uint8_t,const RiderState &){return true;}
bool set_selection(const online_flow::Selection &){return true;}
bool host_release_selection(){return false;}
}
extern "C" int rr64_is_live_race_mode(unsigned){return 1;}
extern "C" int rr64_online_postrace_update(unsigned char *,unsigned){return 0;}
extern "C" int rr64_highlights_presenting(){return 0;}
namespace rr64::highlight_camera { bool active() noexcept {return false;} }
namespace rr64::prediction {
bool capture_cop_rules(CopRulesState &){return false;}
bool capture_cop_posts(CopPostsState &){return false;}
}
#include "../src/rr64_online_race_sync.cpp"
int main(){
    using namespace rr64;
    std::vector<unsigned char> memory(16*1024*1024);
    auto *m=memory.data();const unsigned pool=0x80100000;
    engine::write_u32(m,engine::globals::bike_pool_pointer,pool);
    engine::write_u32(m,0x800A6574,14);
    for(unsigned g=0;g<14;++g) {
        const unsigned b=pool+g*engine::bike::stride, r=0x80300000+g*engine::rider::stride;
        engine::write_u32(m,0x800D8570+(13-g)*0x118+0xe0,b);
        engine::write_u32(m,0x800D8570+(13-g)*0x118+0xe4,r);
        engine::write_u32(m,b+engine::bike::rider_pointer,r);
        engine::write_u32(m,r+engine::rider::bike_pointer,b);
    }
    status.active=status.connected=true;status.phase=netplay::Phase::Race;
    for(auto &p:status.players)p.connected=true;
    for(unsigned count:{2u,4u,14u})for(unsigned local=0;local<count;++local){
        status.connected_players=count;status.local_slot=local;status.replicated_riders=count>4;
        for(unsigned s=0;s<14;++s)status.players[s].connected=s<count;
        for(unsigned g=0;g<14;++g)engine::write_float(m,pool+g*engine::bike::stride+engine::bike::body_position,-999);
        for(unsigned g=0;g<14;++g)engine::write_float(m,0x80300000+g*engine::rider::stride+0x8c,-999);
        const unsigned local_g=13-online_flow::mapped_slot(local,local,count>4);
        const unsigned local_b=(pool+local_g*engine::bike::stride)&0xffffff;
        const unsigned local_r=(0x80300000+local_g*engine::rider::stride)&0xffffff;
        const std::vector<unsigned char> saved_b(memory.begin()+local_b,memory.begin()+local_b+engine::bike::stride);
        const std::vector<unsigned char> saved_r(memory.begin()+local_r,memory.begin()+local_r+engine::rider::stride);
        online_race_sync::apply_remote_riders(m);
        check(std::equal(saved_b.begin(),saved_b.end(),memory.begin()+local_b));
        check(std::equal(saved_r.begin(),saved_r.end(),memory.begin()+local_r));

        for(unsigned slot=0;slot<count;++slot){
            const auto g=13-online_flow::mapped_slot(slot,local,count>4);float x=0;
            engine::read_float(m,pool+g*engine::bike::stride+engine::bike::body_position,x);
            check(x==(slot==local ? -999.f : 100.f+slot));
            engine::read_float(m,0x80300000+g*engine::rider::stride+0x8c,x);
            check(x==(slot==local ? -999.f : 200.f+slot));
            if(slot!=local) {
                const auto b=pool+g*engine::bike::stride, rp=0x80300000+g*engine::rider::stride;
                engine::read_float(m,b+0x16c,x);check(x==10.f+slot);
                engine::read_float(m,b+0x198,x);check(x==50.f+slot);
                engine::read_float(m,b+0x180,x);check(x==90.f+slot);
                engine::read_float(m,rp+0xb8,x);check(x==110.f+slot);
                engine::read_float(m,b+0x21c,x);check(x==0.125f+slot);
                engine::read_float(m,b+0x4ac,x);check(x==-0.25f);
                engine::read_float(m,b+0x4b8,x);check(x==0.75f);
                netplay::RiderState captured{};check(online_race_sync::capture_bike(m,b,captured));
                check(captured.root.bike_display_angles==std::array<float,3>{0.125f+slot,-0.25f,0.75f});
                check(captured.weapon==5 && captured.root.equipment_valid && captured.root.inventory[5]==3 && captured.root.inventory[2]==0);
                check(captured.root.rider_impact_reserve==27.5f+slot);
                check(captured.root.durability==87.25f+slot && captured.root.durability_capacity==150.5f);
                check(captured.root.rider_anchor[0]==130.f+slot && captured.root.rider_anchor[2]==150.f);
                check(captured.root.bike_origin[0]==10.f+slot && captured.root.rider_velocity[1]==110.f+slot);

                engine::read_float(m,pool+g*engine::bike::stride+0x550,x);check(x==300.f+slot);
                engine::read_float(m,0x80300000+g*engine::rider::stride+0x238,x);check(x==400.f+slot);
                unsigned short attached=0;engine::read_u16(m,pool+g*engine::bike::stride+engine::bike::rider_attached,attached);check(attached==slot%2);
                engine::read_u16(m,0x80300000+g*engine::rider::stride+engine::rider::bike_attached,attached);check(attached==slot%2);
            }
        }
        online_race_sync::g_render_race.store(true);
        online_race_sync::apply_remote_riders(m,true);
        const auto before_pose=memory;
        rr64_online_attack_pose_begin(m);
        rr64_online_attack_pose_begin(m);
        for(unsigned slot=0;slot<count;++slot) {
            const auto g=13-online_flow::mapped_slot(slot,local,count>4);
            float phase=0;engine::read_float(m,0x80300000+g*engine::rider::stride+0x558,phase);
            check(phase==(slot==local ? 0.f : 0.5f));
        }
        rr64_online_attack_pose_end(m);
        check(memory!=before_pose); // Nested exit must not restore early.
        rr64_online_attack_pose_end(m);
        check(memory==before_pose); // All14 mappings restore, local untouched.
        status.host_disconnected=true;auto before=memory;
        online_race_sync::apply_remote_riders(m);check(before==memory);status.host_disconnected=false;
        online_race_sync::g_render_race.store(true);
        engine::write_u32(m,0x800A4F24,2);
        rr64_online_render_begin(m);
        unsigned views=0;engine::read_u32(m,0x8009DB88,views);check(views==(count>4?1u:count));
        check(rr64_online_graphics_viewport(m,local)==0);
        rr64_online_graphics_viewport_end(m);
        unsigned active=0;engine::read_u32(m,engine::globals::active_viewport,active);check(active==(count>4?0u:local));
        check(rr64_online_render_loop_continue(1)==0);
        rr64_online_render_end(m);check(rr64_online_render_loop_continue(1)==1);
        engine::read_u32(m,0x8009DB88,views);check(views==(count>4?1u:count));
    }
    // Captured regression: highlights restore two logical players in native
    // mode30. Results must still draw only this peer, retaining camera bank1
    // and the complete simulation roster; leaving the scene ends the override.
    status={};status.active=status.connected=true;status.phase=netplay::Phase::Race;
    status.local_slot=1;status.connected_players=2;
    engine::write_u32(m,engine::globals::pending_mode,0x1e);
    engine::write_u32(m,0x800A4F24,2);
    online_race_sync::before_guest_update(m,0x1e);
    rr64_online_render_begin(m);
    check(online_race_sync::g_viewport_render_plan.peer_fullscreen);
    check(online_race_sync::g_viewport_render_plan.first_viewport==1);
    check(online_race_sync::g_viewport_render_plan.layout==0);
    unsigned logical_views=0;engine::read_u32(m,0x8009DB88,logical_views);check(logical_views==2);
    rr64_online_render_end(m);
    engine::write_u32(m,engine::globals::pending_mode,0x1f);
    online_race_sync::before_guest_update(m,0x1f);
    rr64_online_render_begin(m);check(!online_race_sync::g_viewport_render_plan.peer_fullscreen);
    rr64_online_render_end(m);
    status.active=false;
    engine::write_u32(m,engine::globals::pending_mode,0x1e);
    online_race_sync::before_guest_update(m,0x1e);
    rr64_online_render_begin(m);check(!online_race_sync::g_viewport_render_plan.peer_fullscreen);
    rr64_online_render_end(m);
    // Actual bridge: host AI must update an existing matching client roster
    // actor, reject a different model, and never override the host simulation.
    status.active=status.connected=true;status.phase=netplay::Phase::Race;
    status.local_slot=1;status.connected_players=2;status.replicated_riders=false;
    status.is_host=false;for(unsigned i=0;i<14;++i)status.players[i].connected=i<2;
    npc_fixture=true;
    const unsigned npc_actor=0x800D8570+13*0x118;
    engine::write_u32(m,npc_actor+0x18,2);engine::write_u32(m,npc_actor+0x1C,3);
    engine::write_float(m,pool+engine::bike::body_position,-999);
    online_race_sync::apply_remote_riders(m);
    float npc_x=0;engine::read_float(m,pool+engine::bike::body_position,npc_x);check(npc_x==113);
    engine::write_u32(m,npc_actor+0x18,1);
    engine::write_float(m,pool+engine::bike::body_position,-888);
    online_race_sync::apply_remote_riders(m);
    engine::read_float(m,pool+engine::bike::body_position,npc_x);check(npc_x==-888);
    engine::write_u32(m,npc_actor+0x18,2);status.is_host=true;
    online_race_sync::apply_remote_riders(m);
    engine::read_float(m,pool+engine::bike::body_position,npc_x);check(npc_x==-888);
    status.is_host=false;npc_fixture=false;
    // Reject incomplete/broken ownership without even moving the bike.
    netplay::RiderState invalid{};netplay::get_rider_state(1,invalid);
    invalid.root.valid=0;auto intact=memory;
    online_race_sync::apply_bike(m,pool,invalid);check(memory==intact);
    invalid.root.valid=1;engine::write_u32(m,0x80300000+engine::rider::bike_pointer,pool+engine::bike::stride);
    intact=memory;online_race_sync::apply_bike(m,pool,invalid);check(memory==intact);
    // The dispatcher must hold simulation until both native ownership and
    // the network loading barrier are satisfied, even after menu confirmation.
    status.authoritative=true;status.authority_humans=3;status.phase=netplay::Phase::Race;
    status.local_slot=1;status.replicated_riders=false;
    check(rr64_online_wait_for_race(m,0x17)==1);
    for(unsigned slot=0;slot<2;++slot){
        const auto a=0x800d8570+slot*0x118,b=0x80100000+slot*engine::bike::stride,r=0x80300000+slot*engine::rider::stride;
        engine::write_u16(m,a+0x24,1);engine::write_u32(m,a+0xe0,b);engine::write_u32(m,a+0xe4,r);
        engine::write_u32(m,b+engine::bike::rider_pointer,r);engine::write_u32(m,r+engine::rider::bike_pointer,b);
    }
    engine::write_u32(m,0x800a656c,2);
    check(rr64_online_wait_for_race(m,0x17)==1 && gate_loaded);
    gate_released=true;check(rr64_online_wait_for_race(m,0x17)==0);
    status.authoritative=false;
    check(rr64_online_wait_for_race(m,0x17)==1 && !status.authoritative);
    status.is_host=true;check(rr64_online_wait_for_race(m,0x17)==0 && status.authoritative);
    status.is_host=false;
    engine::write_u32(m,0x800d8570+0xe4,0);
    check(rr64_online_wait_for_race(m,0x17)==1 && !gate_loaded);
    finish_mode=0x19;
    check(rr64_online_wait_for_race(m,0x17)==0);
    unsigned target=0;engine::read_u32(m,engine::globals::pending_mode,target);check(target==0x19);
    engine::write_u32(m,engine::globals::pending_mode,0x39);
    check(rr64_online_wait_for_race(m,0x39)==0);
    engine::read_u32(m,engine::globals::pending_mode,target);check(target==0x39);
    finish_mode=0;
    status.authoritative=false;
    // Notice survives until the stock dispatcher reaches the main menu.
    status.host_disconnected=true; status.host_disconnect_age_ms=2999;
    engine::write_u32(m,engine::globals::pending_mode,0x17);
    check(rr64_online_wait_for_race(m,0x17)==1);
    unsigned pending=0; engine::read_u32(m,engine::globals::pending_mode,pending);check(pending==0x17);
    status.host_disconnect_age_ms=3000;
    check(rr64_online_wait_for_race(m,0x17)==1);
    engine::read_u32(m,engine::globals::pending_mode,pending);check(pending==0x20 && status.active);
    engine::write_u32(m,engine::globals::pending_mode,0x39);
    engine::write_u32(m,0x8009CC50,123);
    check(rr64_online_wait_for_race(m,0x39)==0);
    engine::read_u32(m,engine::globals::pending_mode,pending);check(pending==0x39);
    engine::read_u32(m,0x8009CC50,pending);check(pending==123);
    check(rr64_online_wait_for_race(m,0x20)==0 && !status.active);
    status.active=false;auto before=memory;
    online_race_sync::apply_remote_riders(m);check(before==memory);
    {
        // Recovery must clear the receiver's stale native crash pose gate.
        engine::write_u32(m,0x80300000+engine::rider::bike_pointer,pool);
        netplay::RiderState recovered{};netplay::get_rider_state(1,recovered);
        recovered.root.drive_lockout=1;online_race_sync::apply_bike(m,pool,recovered);
        unsigned short lockout=0;engine::read_u16(m,pool+engine::bike::drive_control_lockout,lockout);check(lockout==1);
        recovered.root.drive_lockout=0;online_race_sync::apply_bike(m,pool,recovered);
        engine::read_u16(m,pool+engine::bike::drive_control_lockout,lockout);check(lockout==0);
    }
    {
        using namespace online_race_sync;
        constexpr unsigned node=0x80500000,model=0x80501000,sr=0x80502000,mat=0x80503000,weapon=0x80504000;
        const unsigned b=pool,r=0x80300000;
        engine::write_u32(m,r+engine::rider::bike_pointer,b);
        engine::write_u32(m,node,1);engine::write_u32(m,node+4,b);engine::write_u32(m,node+0x28,model);
        engine::write_u32(m,model+0x14,sr);
        draw_offsets={};draw_offsets[13]={b,r,{1,2,3}};
        {std::lock_guard lock(presentation_mutex);published_offsets=draw_offsets;published_mapping=m;}
        status.active=status.connected=true;status.host_disconnected=false;
        status.phase=netplay::Phase::Race;status.local_slot=0;status.connected_players=2;
        g_render_race.store(true);
        std::thread render_thread([&] {
            rr64_online_render_begin(m);
            check(draw_offsets[13].bike==b && draw_offsets[13].delta[0]==1);
            rr64_online_render_end(m);
        });render_thread.join();
        presentation_mapping=m;presentation_draw=true;
        for(unsigned source=0;source<3;++source) {
            engine::write_u16(m,sr+0x12,source);engine::write_float(m,0x8009DBAC+source*4,source==1?100.f:10.f);
            for(unsigned i=0;i<16;++i) engine::write_float(m,mat+i*4,float(i));
            const auto before=memory;
            rr64_online_presentation_matrix(m,node,model,mat,3);
            for(unsigned i=0;i<memory.size();++i) if(i<(mat&0xffffff)+48 || i>=(mat&0xffffff)+60) check(memory[i]==before[i]);
            float v=0;engine::read_float(m,mat+48,v);check(v==12+(source==1?100:10));
        }
        // Detailed LOD graph may differ from node.current_model.
        const unsigned detail=0x80505000;
        engine::write_u32(m,node+engine::actor_scene::lod_models,detail);
        engine::write_u32(m,detail+0x14,sr);
        engine::write_float(m,mat+48,0);rr64_online_presentation_matrix(m,node,detail,mat,3);
        float detailed_x=0;engine::read_float(m,mat+48,detailed_x);check(detailed_x==10);
        // Weapon root gets the same offset in its borrowed parent-source units.
        engine::write_u32(m,node,2);engine::write_u32(m,node+4,r);engine::write_u32(m,r+0x5BC,weapon);
        engine::write_float(m,mat+48,0);rr64_online_presentation_matrix(m,node,weapon,mat,1);
        float v=0;engine::read_float(m,mat+48,v);check(v==100);
        auto unchanged=memory;
        rr64_online_presentation_matrix(m,node,weapon+0x18,mat,1);check(memory==unchanged);
        presentation_draw=false;rr64_online_presentation_matrix(m,node,weapon,mat,1);check(memory==unchanged);
        presentation_draw=true;draw_offsets={};rr64_online_presentation_matrix(m,node,weapon,mat,1);check(memory==unchanged);
    }
    {
        // Use the actual production capture, not a manually mirrored packet.
        // Its overlapping pose/integrator fields must form a coherent write set.
        std::fill(memory.begin(),memory.end(),0);
        const unsigned a=0x800d8570,b=pool,r=0x80300000,route=0x80500000;
        engine::write_u32(m,0x800a656c,1);engine::write_u16(m,a+0x24,1);
        engine::write_u32(m,a+0xe0,b);engine::write_u32(m,a+0xe4,r);engine::write_u32(m,a+0xe8,route);
        engine::write_u32(m,b+engine::bike::rider_pointer,r);engine::write_u32(m,r+engine::rider::bike_pointer,b);
        for(unsigned i=0;i<6;++i)engine::write_float(m,0x8009cba8+i*4,0.01f);
        prediction::IntegratorState physics;
        for(unsigned i=0;i<physics.translation.size();++i)physics.translation[i]=float(i+1);
        for(unsigned i=0;i<physics.rotation.size();++i)physics.rotation[i]=float(i+1)*0.01f;
        check(prediction::restore_integrator(m,b+0x108,physics));
        check(prediction::restore_integrator(m,r+0x28,physics));
        netplay::AuthorityFrame frame{};
        check(authority::capture_frame(m,authority::Stamp{7,9,{}},1,100,frame,online_race_sync::capture_bike));
        const auto original=memory;
        engine::write_float(m,b+engine::bike::body_position,900);
        engine::write_float(m,r+0x8c,-900);engine::write_float(m,b+0x108+0x170,90);
        prediction::MovementCorrection correction;
        check(correction.prepare(m,frame,0,false));
        check(correction.commit([](void*) noexcept {return true;},nullptr));
        check(memory==original);
        // Production remote application must preserve local results as well
        // as pose. A retired remote outcome must still be applied.
        engine::write_u32(m,engine::globals::bike_pool_pointer,pool);
        engine::write_u32(m,0x800a656c,2);
        engine::write_u32(m,a+0x118+0xe8,route+0x64);
        engine::write_u32(m,route+0x58,77);
        supplied_frame=frame;supplied_frame.outcomes[0].busts=1;
        supplied_frame.outcomes[1].valid=1;supplied_frame.outcomes[1].busts=9;
        status={};status.active=status.connected=status.authoritative=true;
        status.phase=netplay::Phase::Race;status.game_setup.revision=7;status.local_slot=0;
        authority_fixture=true;online_race_sync::apply_remote_riders(m);authority_fixture=false;
        unsigned value=0;engine::read_u32(m,route+0x58,value);check(value==77);
        engine::read_u32(m,route+0x64+0x58,value);check(value==9);
    }
    std::puts("race memory, production-capture correction and render matrix isolation checks passed");
}
