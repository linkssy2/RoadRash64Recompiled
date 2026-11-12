// Included inside netplay's private namespace. All state uses g_mutex.
// Ordered stop-and-wait per producer/recipient makes UDP retries idempotent.
void reset_hits_locked() {
    if(g_session.hits.round!=g_session.game_setup.revision) {
        g_session.hits={};g_session.hits.round=g_session.game_setup.revision;
    }
}
bool valid_hit_locked(const HitEvent &e,unsigned producer) {
    if(e.round!=g_session.game_setup.revision || !e.round || e.attacker>=kMaximumPlayers ||
       e.victim>=kMaximumPlayers || e.attacker==e.victim || e.kind>1 ||
       !std::isfinite(e.strength) || e.strength<0 || !valid_attack_visual(e.attack) ||
       (e.kind==0 && !e.attack.valid)) return false;
    const bool ai=!(g_session.race_player_mask&(1u<<e.attacker));
    if(e.attacker!=producer && !(producer==0 && ai)) return false;
    const auto &a=g_session.riders[e.attacker], &v=g_session.riders[e.victim];
    if(!a.active || !v.active) return false;
    // Broad plausibility guard only, not server rewind or cheat prevention.
    const float dx=a.position_x-v.position_x,dy=a.position_y-v.position_y,dz=a.position_z-v.position_z;
    return std::isfinite(dx*dx+dy*dy+dz*dz) && dx*dx+dy*dy+dz*dz<=512.f*512.f;
}
bool route_hit_locked(HitEvent e) {
    const bool human=g_session.race_player_mask&(1u<<e.victim);
    const unsigned target=human ? e.victim : 0;
    if(target && !g_session.peers[target].connected) return true;
    if(!target) {
        if(g_session.hits.delivered.size()>=64) return false;
        g_session.hits.delivered.push_back(e);return true;
    }
    auto &queue=g_session.hits.commits[target];
    if(queue.size()>=64) return false;
    e.id=++g_session.hits.next_commit[target];queue.push_back({e,{}});return true;
}
void send_hit_locked(PacketType type,const HitEvent &e,const sockaddr_in &endpoint) {
    HitPacket p{};initialize_packet(p,type);p.event=e;send_packet_locked(p,endpoint);
}
bool handle_hit_locked(const std::uint8_t *bytes,int size,unsigned sender,const sockaddr_in &endpoint,Clock::time_point now) {
    const auto type=reinterpret_cast<const PacketHeader*>(bytes)->type;
    if(type!=PacketType::HitRequest && type!=PacketType::HitCommit &&
       type!=PacketType::HitRequestAck && type!=PacketType::HitCommitAck) return false;
    if(g_session.authoritative || size!=sizeof(HitPacket) || g_session.phase!=Phase::Race) return true;
    reset_hits_locked();const auto &e=reinterpret_cast<const HitPacket*>(bytes)->event;
    if(!e.id || e.round!=g_session.hits.round) return true;
    auto &h=g_session.hits;
    if(is_host_locked() && type==PacketType::HitRequest) {
        auto &last=h.received_request[sender];
        if(e.id==last+1) {
            if(valid_hit_locked(e,sender) && !route_hit_locked(e)) return true;
            last=e.id; // Reject invalid proposals explicitly, without retry storms.
        }
        if(e.id<=last) send_hit_locked(PacketType::HitRequestAck,e,endpoint);
    } else if(is_host_locked() && type==PacketType::HitCommitAck) {
        auto &q=h.commits[sender];if(!q.empty() && q.front().event.id==e.id) q.pop_front();
    } else if(is_client_locked() && type==PacketType::HitRequestAck) {
        if(!h.proposals.empty() && h.proposals.front().event.id==e.id) h.proposals.pop_front();
    } else if(is_client_locked() && type==PacketType::HitCommit) {
        if(e.victim!=g_session.local_slot || e.attacker>=kMaximumPlayers || e.kind>1 ||
           !std::isfinite(e.strength) || e.strength<0 || !valid_attack_visual(e.attack) ||
           (e.kind==0 && !e.attack.valid)) return true;
        if(e.id==h.received_commit+1) {
            if(h.delivered.size()>=64) return true;
            h.delivered.push_back(e);h.received_commit=e.id;
        }
        if(e.id<=h.received_commit) send_hit_locked(PacketType::HitCommitAck,e,endpoint);
    }
    return true;
}
void service_hits_locked(Clock::time_point now) {
    reset_hits_locked();
    if(g_session.authoritative || g_session.phase!=Phase::Race || g_session.host_disconnected) return;
    const auto send=[&](auto &q,PacketType type,const sockaddr_in &endpoint) {
        if(!q.empty() && now-q.front().sent>=std::chrono::milliseconds(60)) {
            send_hit_locked(type,q.front().event,endpoint);q.front().sent=now;
        }
    };
    if(is_host_locked()) {
        for(unsigned i=1;i<kMaximumPlayers;++i)
            if(g_session.peers[i].connected) send(g_session.hits.commits[i],PacketType::HitCommit,g_session.peers[i].endpoint);
    } else if(is_client_locked()) send(g_session.hits.proposals,PacketType::HitRequest,g_session.host_endpoint);
}
