// Internal netplay implementation, under g_mutex and existing endpoint/token
// validation. No new socket, service, or player-facing configuration.
bool valid_finish_mode(unsigned mode) {
    return mode==0 || mode==0x0b || mode==0x14 || mode==0x19 || mode==0x1e || mode==0x20;
}
void commit_authority_frame_locked(const AuthorityFrame &frame) {
    g_session.previous_riders=g_session.riders;
    g_session.riders=frame.riders;
    g_session.authority_frame=frame;
    g_session.world={frame.stamp.round,frame.stamp.tick,frame.traffic};
}
bool enable_authority_locked(unsigned round,unsigned humans) {
    if(round!=g_session.game_setup.revision || !round || !humans || humans>>kMaximumPlayers || !(humans&1))return false;
    if(g_session.authoritative && g_session.authority_round==round)return g_session.authority_humans==humans;
    // Receiving an AuthorityBegin packet cannot stand in for validating the
    // local course. The host additionally needs every admitted rider's ACK.
    if(!g_session.game_setup.valid || g_session.local_slot>=kMaximumPlayers ||
       g_session.course_ack[g_session.local_slot]!=round)return false;
    if(is_host_locked())for(unsigned slot=0;slot<kMaximumPlayers;++slot)
        if((humans&(1u<<slot)) && g_session.course_ack[slot]!=round)return false;
    if(!g_session.authority_host.reset(round,humans))return false;
    g_session.authority_client.reset(round);
    g_session.highlights.reset(is_host_locked(),round);
    g_session.authority_local_sequence=0;
    g_session.authority_frame={};g_session.authority_assemblies={};
    g_session.riders={};g_session.previous_riders={};
    g_session.world={};g_session.world_receiver.reset();
    g_session.authority_replay_stamp={};g_session.authority_replay_ticket=0;g_session.authority_replay_last=0;
    g_session.authority_round=round;g_session.authority_humans=humans;g_session.authoritative=true;
    g_session.authority_loaded=0;g_session.authority_released=false;g_session.authority_finish_mode=0;g_session.authority_finish_tick=0;
    g_session.authority_sent={};g_session.hits={};return true;
}
// Rider-first and traffic-first delivery use the same bounded assembly.
AuthorityAssembly *authority_assembly_locked(const authority::Stamp &stamp,unsigned riders,unsigned world){
    if(stamp.round!=g_session.authority_round || !stamp.tick || stamp.tick<=g_session.authority_frame.stamp.tick)return nullptr;
    auto &a=g_session.authority_assemblies[stamp.tick%g_session.authority_assemblies.size()];
    if(a.frame.stamp.tick>stamp.tick)return nullptr;
    if(a.frame.stamp.tick!=stamp.tick){a={};a.frame.stamp=stamp;a.payload_mask=riders;a.world_mask=world;}
    if(a.payload_mask!=riders || a.world_mask!=world || a.frame.stamp.round!=stamp.round ||
       a.frame.stamp.acknowledged!=stamp.acknowledged || a.frame.stamp.simulated_us!=stamp.simulated_us)return nullptr;
    return &a;
}
void complete_authority_assembly_locked(AuthorityAssembly &a){
    if(a.mask!=(a.payload_mask?a.payload_mask:1u) || a.received_world!=a.world_mask)return;
    for(unsigned s=0;s<kMaximumPlayers;++s)
        if(a.frame.stamp.acknowledged[s]<g_session.authority_frame.stamp.acknowledged[s] ||
           a.frame.stamp.simulated_us[s]<g_session.authority_frame.stamp.simulated_us[s])return;
    if(!world_sync::valid(world_sync::Snapshot{a.frame.stamp.round,a.frame.stamp.tick,a.frame.traffic}) ||
       !valid_course_item_state(a.frame.course_items) || !valid_course_hazard_state(a.frame.course_hazards) ||
       !mk64_items::valid(a.frame.mk64_items) ||
       authority_world_mask(a.frame)!=a.world_mask)return;
    commit_authority_frame_locked(a.frame);
}
bool handle_authority_locked(const std::uint8_t *bytes,int size,unsigned sender,const sockaddr_in &) {
    const auto type=reinterpret_cast<const PacketHeader*>(bytes)->type;
    if(type!=PacketType::AuthorityBegin && type!=PacketType::AuthorityInput && type!=PacketType::AuthorityState && type!=PacketType::AuthorityReady && type!=PacketType::AuthorityWorld && type!=PacketType::AuthorityItems && type!=PacketType::AuthorityHazards && type!=PacketType::AuthorityMk64Riders && type!=PacketType::AuthorityMk64Objects)return false;
    if(g_session.phase!=Phase::Race)return true;
    if(is_client_locked() && type==PacketType::AuthorityBegin && size==sizeof(AuthorityBeginPacket)) {
        const auto &p=*reinterpret_cast<const AuthorityBeginPacket*>(bytes);
        if(p.released<=1 && valid_finish_mode(p.finish_mode) && p.humans==g_session.race_player_mask && enable_authority_locked(p.round,p.humans)) {
            g_session.authority_released|=p.released!=0;
            if(!g_session.authority_finish_mode && p.finish_mode) {
                g_session.authority_finish_mode=p.finish_mode;g_session.authority_finish_tick=p.finish_tick;
            }
        }
    } else if(is_host_locked() && g_session.authoritative && type==PacketType::AuthorityReady && size==sizeof(AuthorityReadyPacket)) {
        const auto &p=*reinterpret_cast<const AuthorityReadyPacket*>(bytes);
        if(p.round==g_session.authority_round && sender<kMaximumPlayers && (g_session.authority_humans&(1u<<sender)))
            g_session.authority_loaded|=1u<<sender;
    } else if(is_host_locked() && g_session.authoritative && type==PacketType::AuthorityInput && size==sizeof(AuthorityInputPacket)) {
        const auto &p=*reinterpret_cast<const AuthorityInputPacket*>(bytes);
        // Loading UI actions are not race inputs. A peer retransmits its first
        // admitted command after release; rejecting early packets consumes no
        // command sequence and cannot create a later gap.
        if(g_session.authority_released && sender<kMaximumPlayers &&
           (g_session.authority_loaded&(1u<<sender)))g_session.authority_host.receive(sender,p.batch);
    } else if(is_client_locked() && g_session.authoritative && type==PacketType::AuthorityState && size==sizeof(AuthorityStatePacket)) {
        const auto &p=*reinterpret_cast<const AuthorityStatePacket*>(bytes);
        if(p.stamp.round!=g_session.authority_round || !p.stamp.tick || p.part>=kAuthorityParts ||
           (p.payload_mask>>kMaximumPlayers) || !p.world_mask || (p.world_mask>>kAuthorityWorldParts) ||
           !((p.payload_mask?p.payload_mask:1u)&(1u<<p.part)) ||
           p.stamp.tick<=g_session.authority_frame.stamp.tick || !authority::valid_timing(p.timing) ||
           p.cop_mode>1 || !std::isfinite(p.cop_win_age) || p.cop_win_age < -1)return true;
        // Validate before allocating or changing an in-flight assembly.
        const unsigned first=p.part*kAuthorityPartRiders,count=std::min(kAuthorityPartRiders,kMaximumPlayers-first);
        std::array<RiderState,kAuthorityPartRiders> states{};
        for(unsigned i=0;i<count;++i) {
            states[i]=from_wire(p.riders[i]);
            if(!authority::valid_outcome(p.outcomes[i]) ||
               (states[i].active || p.outcomes[i].valid)!=bool(p.payload_mask&(1u<<(first+i))))return true;
            if(states[i].active && (!finite_rider_state(states[i]) || !authority::valid_outcome(p.outcomes[i]) || !authority::valid_dynamics(p.dynamics[i]) ||
               (states[i].host_ai && (g_session.authority_humans&(1u<<(first+i))))))return true;
        }
        auto *pending=authority_assembly_locked(p.stamp,p.payload_mask,p.world_mask);
        if(!pending)return true;
        auto &assembly=*pending;
        if(!assembly.mask){
            assembly.frame.timing=p.timing;assembly.frame.cop_mode=p.cop_mode;
            assembly.frame.cop_win_age=p.cop_win_age;
        }
        if(assembly.frame.timing!=p.timing || assembly.frame.cop_mode!=p.cop_mode || assembly.frame.cop_win_age!=p.cop_win_age)return true;
        // A repeated part is immutable. Never mix two versions of one host tick.
        if(assembly.mask&(1u<<p.part))return true;
        for(unsigned i=0;i<count;++i){assembly.frame.riders[first+i]=states[i];assembly.frame.outcomes[first+i]=p.outcomes[i];assembly.frame.dynamics[first+i]=p.dynamics[i];}
        assembly.mask|=1u<<p.part;
        complete_authority_assembly_locked(assembly);
    } else if(is_client_locked() && g_session.authoritative && type==PacketType::AuthorityWorld && size==sizeof(AuthorityWorldPacket)) {
        const auto &p=*reinterpret_cast<const AuthorityWorldPacket*>(bytes);
        if(p.part>=world_sync::batches || !p.world_mask || (p.world_mask>>kAuthorityWorldParts) ||
           !(p.world_mask&(1u<<p.part)) || (p.payload_mask>>kMaximumPlayers))return true;
        for(unsigned i=0;i<p.traffic.size();++i)
            if(!world_sync::valid(p.traffic[i]) ||
               (p.part*world_sync::batch_size+i>=world_sync::capacity && p.traffic[i].active))return true;
        auto *pending=authority_assembly_locked(p.stamp,p.payload_mask,p.world_mask);
        if(!pending || (pending->received_world&(1u<<p.part)))return true;
        for(unsigned i=0;i<p.traffic.size() && p.part*world_sync::batch_size+i<world_sync::capacity;++i)
            pending->frame.traffic[p.part*world_sync::batch_size+i]=p.traffic[i];
        pending->received_world|=1u<<p.part;
        complete_authority_assembly_locked(*pending);
    } else if(is_client_locked() && g_session.authoritative && type==PacketType::AuthorityItems && size==sizeof(AuthorityItemsPacket)) {
        const auto &p=*reinterpret_cast<const AuthorityItemsPacket*>(bytes);
        if(!p.items.count || !valid_course_item_state(p.items) || g_session.game_setup.course.version==0 ||
           !(p.world_mask&kAuthorityItemsBit) || (p.world_mask>>kAuthorityWorldParts) ||
           (p.payload_mask>>kMaximumPlayers))return true;
        auto *pending=authority_assembly_locked(p.stamp,p.payload_mask,p.world_mask);
        if(!pending || (pending->received_world&kAuthorityItemsBit))return true;
        pending->frame.course_items=p.items;
        pending->received_world|=kAuthorityItemsBit;
        complete_authority_assembly_locked(*pending);
    } else if(is_client_locked() && g_session.authoritative && type==PacketType::AuthorityHazards && size==sizeof(AuthorityHazardsPacket)) {
        const auto &p=*reinterpret_cast<const AuthorityHazardsPacket*>(bytes);
        if(!p.count || p.count>kMaximumCourseHazards || p.part>=(p.count+kHazardsPerPacket-1)/kHazardsPerPacket ||
           g_session.game_setup.course.version==0 || (p.world_mask>>kAuthorityWorldParts) ||
           (p.payload_mask>>kMaximumPlayers) ||
           (p.world_mask&hazard_world_mask(kMaximumCourseHazards))!=hazard_world_mask(p.count))return true;
        for(unsigned i=0;i<kHazardsPerPacket;++i)
            if(!valid_course_hazard_pose(p.poses[i]) ||
               (p.part*kHazardsPerPacket+i>=p.count && p.poses[i]!=CourseHazardPose{}))return true;
        auto *pending=authority_assembly_locked(p.stamp,p.payload_mask,p.world_mask);
        const unsigned bit=1u<<(kAuthorityHazardFirst+p.part);
        if(!pending || (pending->received_world&bit))return true;
        auto &hazards=pending->frame.course_hazards;
        if(hazards.count && (hazards.count!=p.count || hazards.clock!=p.clock))return true;
        hazards.count=p.count;hazards.clock=p.clock;
        for(unsigned i=0;i<kHazardsPerPacket && p.part*kHazardsPerPacket+i<p.count;++i)
            hazards.poses[p.part*kHazardsPerPacket+i]=p.poses[i];
        pending->received_world|=bit;
        complete_authority_assembly_locked(*pending);
    } else if(is_client_locked() && g_session.authoritative &&
              type==PacketType::AuthorityMk64Riders && size==sizeof(AuthorityMk64RidersPacket)) {
        const auto &p=*reinterpret_cast<const AuthorityMk64RidersPacket*>(bytes);
        if(!valid_mk64_metadata(p.meta) || !g_session.game_setup.course.version ||
           !(p.world_mask&kAuthorityMk64RidersBit) || (p.world_mask>>kAuthorityWorldParts) ||
           (p.payload_mask>>kMaximumPlayers))return true;
        for(const auto &rider:p.riders)if(!mk64_items::valid(rider,p.meta.clock))return true;
        auto *pending=authority_assembly_locked(p.stamp,p.payload_mask,p.world_mask);
        if(!pending || (pending->received_world&kAuthorityMk64RidersBit))return true;
        auto &items=pending->frame.mk64_items;
        if((pending->received_world&kAuthorityMk64Mask) && mk64_metadata(items)!=p.meta)return true;
        items.enabled=p.meta.enabled;items.clock=p.meta.clock;
        items.next_generation=p.meta.next_generation;items.random=p.meta.random;
        items.riders=p.riders;
        pending->received_world|=kAuthorityMk64RidersBit;
        complete_authority_assembly_locked(*pending);
    } else if(is_client_locked() && g_session.authoritative &&
              type==PacketType::AuthorityMk64Objects && size==sizeof(AuthorityMk64ObjectsPacket)) {
        const auto &p=*reinterpret_cast<const AuthorityMk64ObjectsPacket*>(bytes);
        if(!valid_mk64_metadata(p.meta) || !g_session.game_setup.course.version ||
           p.part>=kMk64ObjectParts || !(p.world_mask&kAuthorityMk64RidersBit) ||
           (p.world_mask>>kAuthorityWorldParts) || (p.payload_mask>>kMaximumPlayers))return true;
        const unsigned bit=1u<<(kAuthorityMk64First+1+p.part);
        if(!(p.world_mask&bit))return true;
        bool populated=false;
        for(unsigned i=0;i<kMk64ObjectsPerPacket;++i) {
            const auto &object=p.objects[i];
            if(!mk64_items::valid(object,p.meta.clock) || object.generation>p.meta.next_generation ||
               (p.part*kMk64ObjectsPerPacket+i>=mk64_items::object_capacity && object!=mk64_items::Object{}))return true;
            populated|=object.generation!=0;
        }
        if(!populated)return true;
        auto *pending=authority_assembly_locked(p.stamp,p.payload_mask,p.world_mask);
        if(!pending || (pending->received_world&bit))return true;
        auto &items=pending->frame.mk64_items;
        if((pending->received_world&kAuthorityMk64Mask) && mk64_metadata(items)!=p.meta)return true;
        items.enabled=p.meta.enabled;items.clock=p.meta.clock;
        items.next_generation=p.meta.next_generation;items.random=p.meta.random;
        for(unsigned i=0;i<kMk64ObjectsPerPacket && p.part*kMk64ObjectsPerPacket+i<mk64_items::object_capacity;++i)
            items.objects[p.part*kMk64ObjectsPerPacket+i]=p.objects[i];
        pending->received_world|=bit;
        complete_authority_assembly_locked(*pending);
    }
    return true;
}
void service_authority_locked(Clock::time_point now) {
    if(!g_session.authoritative)return;
    if(g_session.authority_round!=g_session.game_setup.revision) {g_session.authoritative=false;return;}
    if(g_session.phase!=Phase::Race || g_session.host_disconnected || now-g_session.authority_sent<std::chrono::milliseconds(16))return;
    g_session.authority_sent=now;
    if(is_host_locked()) {
        AuthorityBeginPacket p{};initialize_packet(p,PacketType::AuthorityBegin);
        p.round=g_session.authority_round;p.humans=g_session.authority_humans;
        p.released=g_session.authority_released?1:0;
        p.finish_mode=g_session.authority_finish_mode;
        p.finish_tick=g_session.authority_finish_tick;
        for(unsigned s=1;s<kMaximumPlayers;++s)if(g_session.peers[s].connected)send_packet_locked(p,g_session.peers[s].endpoint);
        const auto payload_mask=authority_payload_mask(g_session.authority_frame);
        // Empty slots need no payload. A retired actor can still carry its
        // final result; retain that part without resurrecting its visible pose.
        if(g_session.authority_frame.stamp.tick)for(unsigned part=0;part<kAuthorityParts;++part) {
            if(!((payload_mask?payload_mask:1u)&(1u<<part)))continue;
            AuthorityStatePacket state{};initialize_packet(state,PacketType::AuthorityState);
            state.stamp=g_session.authority_frame.stamp;state.part=part;
            state.payload_mask=payload_mask;state.world_mask=authority_world_mask(g_session.authority_frame);
            state.timing=g_session.authority_frame.timing;
            state.cop_mode=g_session.authority_frame.cop_mode;state.cop_win_age=g_session.authority_frame.cop_win_age;
            for(unsigned i=0;i<kAuthorityPartRiders && part*kAuthorityPartRiders+i<kMaximumPlayers;++i){
                state.riders[i]=to_wire(g_session.authority_frame.riders[part*kAuthorityPartRiders+i]);
                state.outcomes[i]=g_session.authority_frame.outcomes[part*kAuthorityPartRiders+i];
                state.dynamics[i]=g_session.authority_frame.dynamics[part*kAuthorityPartRiders+i];
            }
            for(unsigned s=1;s<kMaximumPlayers;++s)if(g_session.peers[s].connected)send_packet_locked(state,g_session.peers[s].endpoint);
        }
        if(g_session.authority_frame.stamp.tick)for(unsigned part=0;part<world_sync::batches;++part){
            const auto mask=authority_world_mask(g_session.authority_frame);
            if(!(mask&(1u<<part)))continue;
            AuthorityWorldPacket state{};initialize_packet(state,PacketType::AuthorityWorld);
            state.stamp=g_session.authority_frame.stamp;state.part=part;
            state.payload_mask=payload_mask;state.world_mask=mask;
            for(unsigned i=0;i<state.traffic.size() && part*world_sync::batch_size+i<world_sync::capacity;++i)
                state.traffic[i]=g_session.authority_frame.traffic[part*world_sync::batch_size+i];
            for(unsigned peer=1;peer<kMaximumPlayers;++peer)if(g_session.peers[peer].connected)
                send_packet_locked(state,g_session.peers[peer].endpoint);
        }
        if(g_session.authority_frame.stamp.tick && g_session.authority_frame.course_items.count){
            AuthorityItemsPacket state{};initialize_packet(state,PacketType::AuthorityItems);
            state.stamp=g_session.authority_frame.stamp;state.payload_mask=payload_mask;
            state.world_mask=authority_world_mask(g_session.authority_frame);
            state.items=g_session.authority_frame.course_items;
            for(unsigned peer=1;peer<kMaximumPlayers;++peer)if(g_session.peers[peer].connected)
                send_packet_locked(state,g_session.peers[peer].endpoint);
        }
        const auto &hazards=g_session.authority_frame.course_hazards;
        if(g_session.authority_frame.stamp.tick && hazards.count)
            for(unsigned part=0;part<(hazards.count+kHazardsPerPacket-1)/kHazardsPerPacket;++part){
                AuthorityHazardsPacket state{};initialize_packet(state,PacketType::AuthorityHazards);
                state.stamp=g_session.authority_frame.stamp;state.payload_mask=payload_mask;
                state.world_mask=authority_world_mask(g_session.authority_frame);
                state.count=hazards.count;state.clock=hazards.clock;state.part=static_cast<std::uint16_t>(part);
                for(unsigned i=0;i<kHazardsPerPacket && part*kHazardsPerPacket+i<hazards.count;++i)
                    state.poses[i]=hazards.poses[part*kHazardsPerPacket+i];
                for(unsigned peer=1;peer<kMaximumPlayers;++peer)if(g_session.peers[peer].connected)
                    send_packet_locked(state,g_session.peers[peer].endpoint);
            }
        const auto &items=g_session.authority_frame.mk64_items;
        if(g_session.authority_frame.stamp.tick && items.enabled) {
            const unsigned mask=authority_world_mask(g_session.authority_frame);
            AuthorityMk64RidersPacket riders{};initialize_packet(riders,PacketType::AuthorityMk64Riders);
            riders.stamp=g_session.authority_frame.stamp;riders.payload_mask=payload_mask;riders.world_mask=mask;
            riders.meta=mk64_metadata(items);riders.riders=items.riders;
            for(unsigned peer=1;peer<kMaximumPlayers;++peer)if(g_session.peers[peer].connected)
                send_packet_locked(riders,g_session.peers[peer].endpoint);
            for(unsigned part=0;part<kMk64ObjectParts;++part) {
                if(!(mask&(1u<<(kAuthorityMk64First+1+part))))continue;
                AuthorityMk64ObjectsPacket objects{};initialize_packet(objects,PacketType::AuthorityMk64Objects);
                objects.stamp=riders.stamp;objects.payload_mask=payload_mask;objects.world_mask=mask;
                objects.part=part;objects.meta=riders.meta;
                for(unsigned i=0;i<kMk64ObjectsPerPacket && part*kMk64ObjectsPerPacket+i<mk64_items::object_capacity;++i)
                    objects.objects[i]=items.objects[part*kMk64ObjectsPerPacket+i];
                for(unsigned peer=1;peer<kMaximumPlayers;++peer)if(g_session.peers[peer].connected)
                    send_packet_locked(objects,g_session.peers[peer].endpoint);
            }
        }
    } else {
        if(g_session.local_slot<kMaximumPlayers && (g_session.authority_loaded&(1u<<g_session.local_slot))){
            AuthorityReadyPacket ready{};initialize_packet(ready,PacketType::AuthorityReady);ready.round=g_session.authority_round;
            send_packet_locked(ready,g_session.host_endpoint);
        }
        AuthorityInputPacket p{};initialize_packet(p,PacketType::AuthorityInput);p.batch.round=g_session.authority_round;
        p.batch.count=g_session.authority_client.resend(p.batch.commands);
        if(p.batch.count)send_packet_locked(p,g_session.host_endpoint);
    }
}
