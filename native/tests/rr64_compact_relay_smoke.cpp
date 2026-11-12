// Exercise the production decoder and sender, including invalid packet lengths
// and end-state repair. These are offline protocol checks, not a game launch.
#include "../src/rr64_netplay.cpp"
#include <cstdlib>
#include <limits>
using namespace rr64::netplay;
void check(bool value){if(!value)std::abort();}
RiderState rider(unsigned tick) {RiderState r{};r.active=true;r.tick=tick;r.position_x=float(tick);return r;}
RaceSnapshotPacket packet(unsigned mask,unsigned tick=1) {
    RaceSnapshotPacket p{};initialize_packet(p,PacketType::RaceSnapshot);
    p.round=7;p.slot_mask=mask;
    p.header.size=offsetof(RaceSnapshotPacket,riders)+std::popcount(mask)*sizeof(WireRiderState);
    for(unsigned i=0;i<4;++i)p.riders[i]=to_wire(rider(tick));
    return p;
}
void receive(RaceSnapshotPacket p,int length=-1) {
    handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&p),
        length<0?p.header.size:length,g_session.host_endpoint,Clock::now());
}
int main() {
    g_session={};g_session.token=123;g_session.phase=Phase::Race;
    g_session.game_setup.revision=7;g_session.local_slot=1;
    for(unsigned slot=0;slot<14;++slot) receive(packet(1u<<slot));
    for(unsigned slot=0;slot<14;++slot) check(g_session.riders[slot].tick==(slot==1?0u:1u));
    receive(packet(0x2005,2));check(g_session.riders[0].tick==2 && g_session.riders[2].tick==2 && g_session.riders[13].tick==2);
    receive(packet(0x2005,1));check(g_session.riders[13].tick==2);
    auto animated=packet(1u<<13,3);
    animated.riders[0].root.attack={1,7,1,0,{0.5f,0.25f,-1.f,0,0,0}};
    animated.riders[0].root.rider_impact_reserve=23.5f;
    animated.riders[0].root.bike_display_angles={0.5f,-0.75f,1.25f};
    receive(animated);check(g_session.riders[13].root.bike_display_angles==animated.riders[0].root.bike_display_angles);
    check(g_session.riders[13].root.rider_impact_reserve==23.5f);
    check(g_session.riders[13].root.attack.descriptor==7 &&
        g_session.riders[13].root.attack.clocks[1]==0.25f);
    animated.riders[0].tick=4;animated.riders[0].root.attack.descriptor=48;
    receive(animated);check(g_session.riders[13].tick==3);
    animated.riders[0].root.attack.descriptor=7;
    animated.riders[0].root.attack.clocks[0]=std::numeric_limits<float>::infinity();
    receive(animated);check(g_session.riders[13].tick==3);
    animated.riders[0].root.attack.clocks[0]=0.5f;
    animated.riders[0].root.bike_display_angles[1]=std::numeric_limits<float>::quiet_NaN();
    receive(animated);check(g_session.riders[13].tick==3);
    animated.riders[0].root.bike_display_angles[1]=0;
    animated.riders[0].root.rider_impact_reserve=std::numeric_limits<float>::infinity();
    receive(animated);check(g_session.riders[13].tick==3);
    auto malformed=packet(5,3);
    receive(malformed,malformed.header.size-1);check(g_session.riders[0].tick==2);
    malformed.riders[1].position_x=std::numeric_limits<float>::quiet_NaN();
    receive(malformed);check(g_session.riders[0].tick==2 && g_session.riders[2].tick==2);
    malformed=packet(1,3);malformed.slot_mask=1u<<14;receive(malformed);check(g_session.riders[0].tick==2);
    malformed=packet(1,3);malformed.round=8;receive(malformed);check(g_session.riders[0].tick==2);
    malformed=packet(1,3);malformed.slot_mask=0;receive(malformed);check(g_session.riders[0].tick==2);
    malformed=packet(1,3);malformed.header.session=456;receive(malformed);check(g_session.riders[0].tick==2);

    check(start_winsock_locked());
    SOCKET destination=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(bind(destination,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    socklen_type length=sizeof(address);check(getsockname(destination,reinterpret_cast<sockaddr*>(&address),&length)==0);
    u_long nonblocking=1;check(ioctlsocket(destination,FIONBIO,&nonblocking)==0);
    g_session={};g_session.socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    g_session.peers[1].connected=true;g_session.peers[1].endpoint=address;
    g_session.game_setup.revision=7;g_session.riders[0]=rider(10);g_session.riders[1]=rider(20);
    const auto now=Clock::now();std::array<char,2048> bytes{};
    send_race_snapshot_locked(now);
    check(recv(destination,bytes.data(),bytes.size(),0)==int(offsetof(RaceSnapshotPacket,riders)+sizeof(WireRiderState)));
    RaceSnapshotPacket sent{};std::memcpy(&sent,bytes.data(),sizeof(sent));check(sent.slot_mask==1);
    send_race_snapshot_locked(now+std::chrono::milliseconds(50));
    check(recv(destination,bytes.data(),bytes.size(),0)==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK);
    // Even if the first packet was lost, unchanged final state is resent.
    send_race_snapshot_locked(now+std::chrono::milliseconds(101));
    check(recv(destination,bytes.data(),bytes.size(),0)>0);
    closesocket(destination);close_socket_locked();g_session={};
    std::puts("compact relay validation, ownership and repair checks passed");
}
