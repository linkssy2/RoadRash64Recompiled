#include "../src/rr64_netplay.cpp"
#include <cstdlib>
#include <source_location>
using namespace rr64::netplay;
void check(bool b,std::source_location at=std::source_location::current()){
 if(!b){std::fprintf(stderr,"authority channel check failed at line %u\n",at.line());std::exit(1);}
}
int main(){
 g_session={};g_session.config.mode=Mode::Host;g_session.phase=Phase::Race;
 g_session.local_slot=0;g_session.token=123;g_session.game_setup.revision=7;g_session.race_player_mask=3;
 sockaddr_in remote{};remote.sin_family=AF_INET;remote.sin_addr.s_addr=htonl(INADDR_LOOPBACK);remote.sin_port=htons(18001);
 g_session.peers[1].connected=true;g_session.peers[1].endpoint=remote;
 check(!authority_start()); // Merely entering Race cannot skip course validation.
 g_session.game_setup.valid=true;g_session.course_ack[0]=7;
 check(!authority_start()); // One missing peer ACK still holds the round.
 g_session.course_ack[1]=6;check(!authority_start());
 g_session.course_ack[1]=7;
 check(authority_start());
 rr64::authority::Command not_admitted{};not_admitted.sequence=99;
 check(!authority_queue_input_recorded(0x8000,1,0,not_admitted) && not_admitted.sequence==99);
 check(g_session.authority_local_sequence==0);
 AuthorityInputPacket early{};initialize_packet(early,PacketType::AuthorityInput);
 early.batch.round=7;early.batch.count=1;early.batch.commands[0]={7,1,0x4000,9,0,0};
 handle_host_packet_locked(reinterpret_cast<const std::uint8_t*>(&early),sizeof(early),remote,Clock::now());
 check(!authority_race_gate(true));
 AuthorityReadyPacket loaded{};initialize_packet(loaded,PacketType::AuthorityReady);loaded.round=6;
 handle_host_packet_locked(reinterpret_cast<const std::uint8_t*>(&loaded),sizeof(loaded),remote,Clock::now());
 check(!authority_race_gate(false));
 loaded.round=7;
 handle_host_packet_locked(reinterpret_cast<const std::uint8_t*>(&loaded),sizeof(loaded),remote,Clock::now());
 check(authority_race_gate(false));
 check(!authority_set_finish_mode(0x39) && !authority_set_finish_mode(0));
 check(authority_set_finish_mode(0x19) && authority_finish_mode()==0x19);
 check(!authority_set_finish_mode(0x20));g_session.authority_finish_mode=0;
 { rr64::prediction::ReplayScope replay;check(replay.valid() && !get_status().active && !get_status().authoritative); }
 check(get_status().authoritative);
 AuthorityInputPacket p{};initialize_packet(p,PacketType::AuthorityInput);p.batch.round=7;p.batch.count=1;p.batch.commands[0]={7,1,0x9001,27,0,rr64::authority::action_eject};
 // The outer dispatcher, not the payload, authenticates slot ownership.
 auto bad=p;bad.header.session=124;
 handle_host_packet_locked(reinterpret_cast<const std::uint8_t*>(&bad),sizeof(bad),remote,Clock::now());
 auto stranger=remote;stranger.sin_port=htons(18002);
 handle_host_packet_locked(reinterpret_cast<const std::uint8_t*>(&p),sizeof(p),stranger,Clock::now());
 rr64::authority::Step rejected;rr64::authority::Stamp rejectedStamp;
 check(authority_begin_step(rejected));check(rejected.inputs[1].sequence==0);
 check(authority_finish_step(rejected.tick,rejectedStamp));
 handle_host_packet_locked(reinterpret_cast<const std::uint8_t*>(&p),sizeof(p),remote,Clock::now());
 rr64::authority::Step step;check(authority_begin_step(step));check(step.inputs[1].sequence==1 && step.inputs[1].buttons==0x8001 && step.inputs[1].actions==rr64::authority::action_eject);
 check(g_session.authority_host.stamp().acknowledged[1]==0);
 rr64::authority::Stamp stamp;check(authority_finish_step(step.tick,stamp));check(stamp.acknowledged[1]==1);
 ClientRiderStatePacket old{};initialize_packet(old,PacketType::ClientRiderState);old.round=7;old.slot=1;old.state.active=1;old.state.tick=1;old.state.position_x=900;
 handle_host_packet_locked(reinterpret_cast<const std::uint8_t*>(&old),sizeof(old),remote,Clock::now());check(!g_session.riders[1].active);
 HitEvent hit{};hit.round=7;check(!submit_hit(hit)); // competing authority path closed
 p.batch.commands[0].sequence=2;p.batch.commands[0].round=6;
 handle_host_packet_locked(reinterpret_cast<const std::uint8_t*>(&p),sizeof(p),remote,Clock::now());
 check(authority_begin_step(step));check(step.inputs[1].sequence==0);check(authority_finish_step(step.tick,stamp));
 // A host must run beyond the 256-command resend window without waiting for
 // an acknowledgement from itself. Rejected inputs cannot leave sequence gaps.
 check(!authority_queue_input(0,-128,0));
 for(unsigned i=1;i<=600;++i){
  check(authority_queue_input(0x8000,12,0));check(authority_begin_step(step));
  check(step.inputs[0].sequence==i);check(authority_finish_step(step.tick,stamp));
  check(stamp.acknowledged[0]==i);
 }
 check(g_session.authority_client.pending()==0);
 AuthorityFrame hostFrame{};hostFrame.stamp=stamp;
 for(auto &bits:hostFrame.timing.bits)bits=std::bit_cast<std::uint32_t>(1.f);
 hostFrame.timing.substeps=2;
 for(unsigned slot=0;slot<kMaximumPlayers;++slot)for(unsigned field=0;field<9;++field)hostFrame.dynamics[slot].values[field]=float(slot*9+field);
 hostFrame.dynamics[13].damping_mode=3;
 hostFrame.dynamics[13].bike_physics.rotation[27]=12.5f;
 hostFrame.dynamics[13].rider_physics.force[5]=0.25f;
 hostFrame.cop_mode=1;hostFrame.outcomes[13].role=7;hostFrame.outcomes[13].busts=4;hostFrame.outcomes[13].siren=1;
 hostFrame.outcomes[13].finished=1;
 hostFrame.outcomes[13].progress={0.25f,120.f,950.f};hostFrame.outcomes[13].progress_gate=2;
 for(unsigned i:{0u,19u}){
  auto &car=hostFrame.traffic[i];car.active=car.motion_valid=1;car.id=i+100;car.kind=1;car.model=0xd8;
  car.position[0]=float(i+500);car.motion[0]=car.position[0];
 }
 for(unsigned s=0;s<kMaximumPlayers;++s){hostFrame.riders[s].active=true;hostFrame.riders[s].position_x=100.f+s;}
 auto wrongFrame=hostFrame;++wrongFrame.stamp.tick;check(!authority_publish_frame(wrongFrame));
 wrongFrame=hostFrame;++wrongFrame.stamp.acknowledged[0];check(!authority_publish_frame(wrongFrame));
 wrongFrame=hostFrame;++wrongFrame.stamp.simulated_us[0];check(!authority_publish_frame(wrongFrame));
 wrongFrame=hostFrame;wrongFrame.timing.substeps=3;check(!authority_publish_frame(wrongFrame));
 wrongFrame=hostFrame;wrongFrame.timing.bits[0]=0x7fc00000;check(!authority_publish_frame(wrongFrame));
 wrongFrame=hostFrame;wrongFrame.outcomes[13].siren=2;check(!authority_publish_frame(wrongFrame));
 wrongFrame=hostFrame;wrongFrame.traffic[19].id=wrongFrame.traffic[0].id;check(!authority_publish_frame(wrongFrame));
 check(authority_publish_frame(hostFrame));check(!authority_publish_frame(hostFrame));
 check(start_winsock_locked());SOCKET receiver=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 check(bind(receiver,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);socklen_type size=sizeof(address);check(getsockname(receiver,reinterpret_cast<sockaddr*>(&address),&size)==0);
 u_long nonblocking=1;check(ioctlsocket(receiver,FIONBIO,&nonblocking)==0);
 g_session={};g_session.config.mode=Mode::Join;g_session.phase=Phase::Race;g_session.local_slot=1;g_session.token=123;
 g_session.game_setup.revision=7;g_session.race_player_mask=3;g_session.host_endpoint=address;g_session.socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
 AuthorityBeginPacket begin{};initialize_packet(begin,PacketType::AuthorityBegin);begin.round=7;begin.humans=3;
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&begin),sizeof(begin),address,Clock::now());check(!g_session.authoritative);
 g_session.game_setup.valid=true;g_session.course_ack[1]=6;
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&begin),sizeof(begin),address,Clock::now());check(!g_session.authoritative);
 g_session.course_ack[1]=7;
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&begin),sizeof(begin),address,Clock::now());check(g_session.authoritative);
 check(!authority_queue_input(0x8000,0,0) && g_session.authority_client.pending()==0);
 begin.finish_mode=0x39;
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&begin),sizeof(begin),address,Clock::now());check(authority_finish_mode()==0);
 begin.finish_mode=0x19;
 begin.finish_tick=9;
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&begin),sizeof(begin),address,Clock::now());check(authority_finish_mode()==0);
 g_session.authority_frame.stamp.tick=9;
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&begin),sizeof(begin),address,Clock::now());check(authority_finish_mode()==0);
 begin.finish_mode=0;
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&begin),sizeof(begin),address,Clock::now());check(authority_finish_mode()==0);
 g_session.authority_finish_mode=0;g_session.authority_finish_tick=0;g_session.authority_frame={};begin.finish_tick=0;
 begin.released=1;
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&begin),sizeof(begin),address,Clock::now());
 check(!authority_queue_input(0x8000,0,0)); // Host released, local load still incomplete.
 check(authority_race_gate(true));
 rr64::authority::Command accepted;
 check(!authority_queue_input_recorded(1,45,-25,accepted,0x80));
 check(authority_queue_input_recorded(1,45,-25,accepted,rr64::authority::action_eject));service_authority_locked(Clock::now());
 AuthorityInputPacket received{};bool saw_ready=false,saw_input=false;
 for(unsigned n=0;n<2;++n){
  AuthorityInputPacket packet{};const int bytes=recv(receiver,reinterpret_cast<char*>(&packet),sizeof(packet),0);
  if(bytes==sizeof(AuthorityReadyPacket) && packet.header.type==PacketType::AuthorityReady)saw_ready=true;
  else {check(bytes==sizeof(packet) && packet.header.type==PacketType::AuthorityInput);received=packet;saw_input=true;}
 }
 check(saw_ready && saw_input && accepted.sequence==1);
 check(received.header.type==PacketType::AuthorityInput && received.batch.count==1 && received.batch.commands[0].x==45 && received.batch.commands[0].y==-25 && received.batch.commands[0].actions==rr64::authority::action_eject);
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&begin),sizeof(begin),address,Clock::now());check(g_session.authority_client.pending()==1);
 auto deliver_world=[&](const AuthorityFrame &f,unsigned omitted=~0u){
  for(unsigned part=0;part<rr64::world_sync::batches;++part){
   if(part==omitted || !(authority_world_mask(f)&(1u<<part)))continue;
   AuthorityWorldPacket packet{};initialize_packet(packet,PacketType::AuthorityWorld);
   packet.stamp=f.stamp;packet.part=part;packet.payload_mask=authority_payload_mask(f);packet.world_mask=authority_world_mask(f);
   for(unsigned i=0;i<packet.traffic.size() && part*rr64::world_sync::batch_size+i<rr64::world_sync::capacity;++i)
    packet.traffic[i]=f.traffic[part*rr64::world_sync::batch_size+i];
   handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&packet),sizeof(packet),address,Clock::now());
  }
 };
 auto deliver=[&](unsigned part,const AuthorityFrame &f,bool world=true){
  if(world)deliver_world(f);
  AuthorityStatePacket packet{};initialize_packet(packet,PacketType::AuthorityState);packet.stamp=f.stamp;packet.part=part;
  packet.payload_mask=authority_payload_mask(f);packet.world_mask=authority_world_mask(f);
  packet.timing=f.timing;
  packet.cop_mode=f.cop_mode;packet.cop_win_age=f.cop_win_age;
  for(unsigned i=0;i<kAuthorityPartRiders && part*kAuthorityPartRiders+i<kMaximumPlayers;++i)packet.riders[i]=to_wire(f.riders[part*kAuthorityPartRiders+i]);
  for(unsigned i=0;i<kAuthorityPartRiders && part*kAuthorityPartRiders+i<kMaximumPlayers;++i)packet.outcomes[i]=f.outcomes[part*kAuthorityPartRiders+i];
  for(unsigned i=0;i<kAuthorityPartRiders && part*kAuthorityPartRiders+i<kMaximumPlayers;++i)packet.dynamics[i]=f.dynamics[part*kAuthorityPartRiders+i];
  handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&packet),sizeof(packet),address,Clock::now());
 };
 AuthorityFrame shown{};
 // A complete rider roster alone cannot expose a newer world.
 for(unsigned part=0;part<kAuthorityParts;++part)deliver(part,hostFrame,false);
 check(!authority_get_frame(shown));
 deliver_world(hostFrame,rr64::world_sync::batches-1);check(!authority_get_frame(shown)); // missing the final car batch
 deliver_world(hostFrame);check(authority_get_frame(shown) && shown.traffic==hostFrame.traffic);
 g_session.authority_frame={};g_session.world={};
 // Reset assembly only in this fixture to exercise traffic-first permutations too.
 g_session.authority_assemblies={};

 for(unsigned part=kAuthorityParts;part-->0;)if(part!=3){deliver(part,hostFrame);check(!authority_get_frame(shown));}
 deliver(2,hostFrame);check(!authority_get_frame(shown)); // duplicate cannot fill a missing part
 deliver(3,hostFrame);check(authority_get_frame(shown));
 check(shown.stamp.tick==hostFrame.stamp.tick && shown.stamp.acknowledged==hostFrame.stamp.acknowledged &&
       shown.stamp.simulated_us==hostFrame.stamp.simulated_us);
 check(shown.timing==hostFrame.timing && shown.dynamics==hostFrame.dynamics);
 check(shown.cop_mode==1 && shown.outcomes[13]==hostFrame.outcomes[13]);
 rr64::world_sync::Snapshot sharedWorld;
 check(get_world_state(sharedWorld) && sharedWorld.tick==shown.stamp.tick && sharedWorld.traffic==shown.traffic);

 rr64::authority::Outcome result;bool mode=false;check(authority_get_outcome(13,result,mode) && mode && result.busts==4);
 for(unsigned s=0;s<kMaximumPlayers;++s)check(shown.riders[s].position_x==100.f+s);
 check(shown.riders[1].active); // includes the local client, unlike the old self-echo filter
 RiderState visible{};check(get_rider_state(13,visible) && visible.position_x==113.f);
 check(g_session.authority_client.pending()==1); // receipt isn't reconciliation
 authority_pin_frame();
 auto nextFrame=hostFrame;++nextFrame.stamp.tick;nextFrame.riders[1]={};
 auto conflicting=nextFrame;++conflicting.stamp.acknowledged[0];
 auto badDynamics=nextFrame;badDynamics.dynamics[0].values[0]=std::numeric_limits<float>::quiet_NaN();
 deliver(0,badDynamics);check(authority_get_frame(shown) && shown.stamp.tick==hostFrame.stamp.tick);
 auto badTiming=nextFrame;badTiming.timing.bits[0]=0x7f800000;
 deliver(0,badTiming);check(authority_get_frame(shown) && shown.stamp.tick==hostFrame.stamp.tick);
 deliver(0,nextFrame);deliver(2,conflicting);
 auto otherTiming=nextFrame;otherTiming.timing.substeps=1;deliver(2,otherTiming);
 auto otherResult=nextFrame;otherResult.cop_win_age=1;deliver(2,otherResult);
 for(unsigned part=3;part<kAuthorityParts;++part)deliver(part,nextFrame);
 check(authority_get_frame(shown) && shown.stamp.tick==hostFrame.stamp.tick);
 deliver(2,nextFrame);
 check(authority_get_frame(shown) && shown.stamp.tick==hostFrame.stamp.tick);
 check(get_world_state(sharedWorld) && sharedWorld.tick==hostFrame.stamp.tick);
 authority_pin_frame();
 check(authority_get_frame(shown) && shown.stamp.tick==nextFrame.stamp.tick && !shown.riders[1].active);
 authority_read.enabled=false; // remaining cases explicitly control receipt/replay timing
 check(!get_rider_state(1,visible)); // renderer sees authoritative retirement too
 for(unsigned part=0;part<kAuthorityParts;++part)deliver(part,hostFrame);
 check(authority_get_frame(shown) && shown.stamp.tick==nextFrame.stamp.tick); // no stale resurrection
 AuthorityReplayPlan plan{};
 check(authority_prepare_replay(plan) && plan.count==0);
 const auto oldTicket=plan.ticket;
 check(authority_queue_input(2,10,0));
 check(!authority_commit_replay(oldTicket) && g_session.authority_client.pending()==2);
 check(authority_prepare_replay(plan) && plan.ticket!=oldTicket && plan.count==1 && plan.commands[0].sequence==2);
 const auto replayTicket=plan.ticket;
 // A newer network frame may arrive during isolated replay. The older issued
 // replay is still valid if no new local command was appended in the meantime.
 ++nextFrame.stamp.tick;nextFrame.stamp.acknowledged[1]=2;
 for(unsigned part=0;part<kAuthorityParts;++part)deliver(part,nextFrame);
 begin.finish_mode=0x19;begin.finish_tick=nextFrame.stamp.tick;
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&begin),sizeof(begin),address,Clock::now());
 check(authority_finish_mode()==0); // Complete snapshot received, not applied.
 check(authority_commit_replay(replayTicket) && g_session.authority_client.pending()==1);
 check(authority_finish_mode()==0); // Older successful replay is insufficient.
 check(!authority_commit_replay(replayTicket) && !authority_commit_replay(oldTicket));
 check(authority_prepare_replay(plan) && plan.count==0);
 check(authority_commit_replay(plan.ticket) && g_session.authority_client.pending()==0);
 check(authority_finish_mode()==0x19);
 g_session.phase=Phase::Lobby;check(authority_finish_mode()==0);g_session.phase=Phase::Race;
 g_session.authority_finish_mode=0;g_session.authority_finish_tick=0;
 check(!authority_prepare_replay(plan)); // already reconciled this host tick
 ++nextFrame.stamp.tick;
 for(unsigned part=0;part<kAuthorityParts;++part)deliver(part,nextFrame);
 check(authority_prepare_replay(plan));
 g_session.host_disconnected=true;
 check(!authority_commit_replay(plan.ticket));
 g_session.host_disconnected=false;
 // Sparse frames need only occupied riders. Inactive identities retire with
 // the same completed tick, not via independent stale per-rider updates.
 auto sparse=nextFrame;++sparse.stamp.tick;
 for(unsigned s=2;s<kMaximumPlayers;++s)sparse.riders[s]={};
 sparse.riders[1]=hostFrame.riders[1];
 deliver(0,sparse);check(authority_get_frame(shown) && shown.stamp.tick==nextFrame.stamp.tick);
 deliver(1,sparse);check(authority_get_frame(shown) && shown.stamp.tick==sparse.stamp.tick && !shown.riders[13].active);
 auto empty=sparse;++empty.stamp.tick;empty.riders={};
 deliver(0,empty);check(authority_get_frame(shown) && shown.stamp.tick==empty.stamp.tick && !shown.riders[0].active);
 auto retired=empty;++retired.stamp.tick;retired.outcomes[1].valid=1;retired.outcomes[1].busted=1;
 deliver(1,retired);
 check(authority_get_frame(shown) && shown.stamp.tick==retired.stamp.tick && !shown.riders[1].active);
 check(authority_get_outcome(1,result,mode) && result.busted==1);
 check(!get_rider_state(1,visible));
 std::printf("authority packet %zu bytes; sparse2-rider snapshot %zu bytes vs full14 %zu bytes (payload only)\n",
    sizeof(AuthorityStatePacket),2*sizeof(AuthorityStatePacket),14*sizeof(AuthorityStatePacket));
 g_session.game_setup.revision=8;service_authority_locked(Clock::now());check(!g_session.authoritative);
 check(!authority_commit_replay(plan.ticket));
 // A simulation fault retains identity for the timed menu teardown, rejects
 // more inputs and never acknowledges a failed staged step.
 g_session={};g_session.config.mode=Mode::Host;g_session.phase=Phase::Race;
 g_session.local_slot=0;g_session.token=123;g_session.game_setup.revision=7;g_session.race_player_mask=3;
 g_session.game_setup.valid=true;g_session.course_ack[0]=g_session.course_ack[1]=7;
 check(authority_start());g_session.authority_loaded=3;check(authority_race_gate(true));
 check(authority_queue_input(0,0,0));check(authority_begin_step(step));
 const auto beforeFault=g_session.authority_host.stamp();
 authority_fail("test incomplete native step");
 check(g_session.host_disconnected && g_session.local_slot==0 && g_session.authoritative);
 check(g_session.message=="Online sync stopped");
 check(!authority_queue_input(0,0,0) && !authority_begin_step(step));
 check(!authority_finish_step(step.tick,stamp));
 check(g_session.authority_host.stamp().tick==beforeFault.tick);
 const auto failureTime=g_session.host_disconnected_at;authority_fail("again");
 check(g_session.host_disconnected_at==failureTime);
 // A lost member cannot leave either a loading round or a running round
 // consuming their last held input. Outside authority, lobby removal remains.
 for(bool released:{false,true})for(bool timeout:{false,true}) {
  g_session={};g_session.config.mode=Mode::Host;g_session.phase=Phase::Race;
  g_session.local_slot=0;g_session.token=123;g_session.game_setup.revision=7;g_session.race_player_mask=3;
  g_session.game_setup.valid=true;g_session.course_ack[0]=g_session.course_ack[1]=7;
  g_session.peers[1].connected=true;g_session.peers[1].endpoint=remote;
  g_session.peers[1].last_seen=Clock::now()-kPeerTimeout-std::chrono::seconds(1);
  check(authority_start());g_session.authority_released=released;
  if(timeout)expire_peers_locked(Clock::now());
  else {
   PacketHeader leaving{};leaving.type=PacketType::Disconnect;leaving.session=123;leaving.size=sizeof(leaving);
   handle_host_packet_locked(reinterpret_cast<const std::uint8_t*>(&leaving),sizeof(leaving),remote,Clock::now());
  }
  check(!g_session.host_disconnected && authority_race_gate(true));
  check(!g_session.peers[1].connected && authority_queue_input(0,0,0));
  rr64::authority::Step continued;check(authority_begin_step(continued));
  check(continued.inputs[1].buttons==0 && continued.inputs[1].actions==0 && continued.inputs[1].sequence==0);
 }
 g_session={};g_session.config.mode=Mode::Host;g_session.phase=Phase::Lobby;
 g_session.peers[1].connected=true;g_session.peers[1].last_seen=Clock::now()-kPeerTimeout-std::chrono::seconds(1);
 expire_peers_locked(Clock::now());check(!g_session.host_disconnected && !g_session.peers[1].connected);
 close_socket_locked();g_session={};
 // An unanswered join returns to offline with an actionable message. It must
 // never acquire a player slot or advance the game menu.
 g_session.config.mode=Mode::Join;
 g_session.socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
 ioctlsocket(g_session.socket,FIONBIO,&nonblocking);
 g_session.connect_started=Clock::now()-kConnectTimeout-std::chrono::seconds(1);
 service_locked(Clock::now());
 check(g_session.phase==Phase::Offline && g_session.local_slot==kInvalidSlot);
 check(g_session.socket==INVALID_SOCKET && g_session.message.find("Failed to connect")!=std::string::npos);
 closesocket(receiver);close_socket_locked();g_session={};
 // Native post-race progression is a persistent, ordered host command. The
 // next setup must not retire the final frame/replay before a delayed guest
 // completes both native transitions and acknowledges actual menu arrival.
 g_session.config.mode=Mode::Host;g_session.phase=Phase::Race;g_session.local_slot=0;
 g_session.authoritative=true;g_session.authority_round=8;g_session.authority_humans=3;
 g_session.game_setup.valid=true;g_session.game_setup.revision=8;g_session.setup_serial=8;
 g_session.authority_finish_mode=0x1e;g_session.token=123;
 g_session.players[0].connected=g_session.players[1].connected=true;
 g_session.peers[1].connected=true;g_session.peers[1].endpoint=remote;
 check(!host_advance_postrace(2) && !host_advance_postrace(3));
 check(host_advance_postrace(1) && host_advance_postrace(1));
 check(host_advance_postrace(2) && !host_advance_postrace(1));
 check(acknowledge_postrace({8,2}) && !host_resume_postrace_setup());
 ClientStatePacket arrived{};initialize_packet(arrived,PacketType::ClientState);
 arrived.slot=1;arrived.postrace_ack={8,2};
 auto wrong=arrived;wrong.postrace_ack={7,2};
 handle_host_packet_locked(reinterpret_cast<const std::uint8_t*>(&wrong),sizeof(wrong),remote,Clock::now());
 check(!host_resume_postrace_setup() && g_session.authoritative);
 arrived.header.sequence=wrong.header.sequence+1;
 handle_host_packet_locked(reinterpret_cast<const std::uint8_t*>(&arrived),sizeof(arrived),stranger,Clock::now());
 check(!host_resume_postrace_setup()); // Endpoint/token owns the ACK.
 handle_host_packet_locked(reinterpret_cast<const std::uint8_t*>(&arrived),sizeof(arrived),remote,Clock::now());
 check(host_resume_postrace_setup());
 check(g_session.phase==Phase::GameSetup && !g_session.authoritative && !g_session.game_setup.valid);
 check(g_session.postrace==rr64::online_flow::PostRaceState{8,2});
 check(host_commit_game_setup(GameSetupState{}));
 check(g_session.game_setup.revision==9 && g_session.phase==Phase::CharacterSelect);
 g_session={};g_session.config.mode=Mode::Join;g_session.local_slot=1;g_session.token=123;
 g_session.host_endpoint=remote;g_session.phase=Phase::Race;
 LobbySnapshotPacket resume{};initialize_packet(resume,PacketType::LobbySnapshot);
 resume.phase=static_cast<unsigned>(Phase::Race);resume.postrace={8,2};
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&resume),sizeof(resume),remote,Clock::now());
 check(g_session.postrace==resume.postrace);
 ++resume.header.sequence;resume.postrace={8,1};
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&resume),sizeof(resume),remote,Clock::now());
 check(g_session.postrace.stage==2); // Even a fresh packet cannot rewind it.
 ++resume.header.sequence;resume.postrace={7,2};
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&resume),sizeof(resume),remote,Clock::now());
 check(g_session.postrace.round==8);
 ++resume.header.sequence;resume.postrace={9,1};
 handle_client_packet_locked(reinterpret_cast<const std::uint8_t*>(&resume),sizeof(resume),remote,Clock::now());
 check(g_session.postrace==resume.postrace);
 g_session={};
 std::puts("authority channel: existing authority checks and persistent post-race setup barrier passed");
}
