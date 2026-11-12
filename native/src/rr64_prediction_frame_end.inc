} // private native update

const char *last_native_replay_error(){return frame_error;}
bool replay_native_streaming(Resources &resources,const StreamingInput &input,const ResourceInventory &schedule){
    frame_error[0]=0;
    if(active() || frame_resources)return fail_frame("streaming replay reentry");
    FloatingPointScope floating(input.entry);
    recomp_context context{};
    if(!floating.valid() || !input.entry.restore(context) || context.r4>3 ||
       (context.r29&7) || !engine::valid_guest_range(static_cast<unsigned>(context.r29)-4096,4096) ||
       !ResourceInventory::inspect(resources.memory()).can_start_worker() || !prepare_worker_entry())
        return fail_frame("streaming entry unavailable");
    ReplayScope scope;
    if(!scope.valid())return fail_frame("streaming scope invalid");
    TerrainStreamingScope terrain_scope(input.bounded_terrain);
    struct Binding {
        Binding(Resources &r){frame_resources=&r;frame_blocks=2000000;}
        ~Binding(){frame_resources=nullptr;frame_blocks=0;}
    } binding(resources);
    try{
        // Nonblocking native polls do not run the private background worker.
        // Catch it up only to the task ownership observed at this boundary;
        // do not drain every request early or copy live completion pointers.
        if(drain_resource_worker(resources,worker_entry,&schedule)!=ResourceDrain::Reached)
            return fail_frame("streaming resource schedule unavailable");
        if(!input.restore(resources.memory()))return fail_frame("streaming inputs invalid");
        func_8007B8D4(resources.memory(),&context);
        return true;
    }catch(const Resources::Invalid &e){return fail_frame(e.what());}
     catch(const Resources::Blocked&){return fail_frame("streaming resource wait blocked");}
}
bool replay_native_order(Resources &resources,const FrameInput &input){
    frame_error[0]=0;
    if(active() || frame_resources)return fail_frame("order replay reentry");
    FloatingPointScope floating(input.entry);
    if(!floating.valid())return fail_frame("order floating-point context unavailable");
    recomp_context context{};
    if(!input.entry.restore(context) || (context.r29&7) ||
       !engine::valid_guest_range(static_cast<unsigned>(context.r29)-4096,4096))return fail_frame("order stack invalid");
    if(!prepare_worker_entry())return fail_frame("invalid order worker context");
    unsigned count=0;
    if(!engine::read_u32(resources.memory(),0x800a656c,count) || !count || count>14)return fail_frame("order roster invalid");
    ReplayScope scope(input.session);
    // 6E5E0 -> 63B50 -> 636B0 can restore cop notification wording.
    // Without an explicit seed, isolated_state would clone TODAY's live cop
    // state into this historical transaction on its first notification.
    if(!scope.valid() || !seed_cop_rules(input.rules))return fail_frame("order cop state unavailable");
    if(!bind_item_state(resources.memory(),input.items))return fail_frame("order item state unavailable");
    if(!rr64::online_terrain::bind_replay(resources.memory(),resources.rom()))
        return fail_frame("historical collision bank unavailable");
    struct Binding {
        Binding(Resources &r){frame_resources=&r;frame_blocks=2000000;}
        ~Binding(){frame_resources=nullptr;frame_blocks=0;}
    } binding(resources);
    try{
        // Order also reads the physics delta (6EE64). A preceding replay can
        // leave it halved for the second substep; restore this update's entry
        // timing before order, not only before the following simulation.
        if(!restore_timing(resources.memory(),input.timing) ||
           !restore_update_counters(resources.memory(),input.counters))return fail_frame("order timing unavailable");
        native_order(resources.memory(),&context);
        // The full native routine now writes neighbors and eligibility itself.
        // Reapplying the former sorting-only approximation here would overwrite
        // legitimate race/recovery transitions with a second derived result.
        return true;
    }catch(const Resources::Invalid &e){return fail_frame(e.what());}
     catch(const rr64::online_terrain::CollisionUnavailable &e){return fail_frame(e.what());}
     catch(const Resources::Blocked&){return fail_frame("order resource wait blocked");}
}
bool replay_native_frame(Resources& resources,const FrameInput& input,FrameOutput& output){
    frame_error[0]=0;
    if(active() || frame_resources || !authority::valid(input.command,input.command.round) ||
       input.replay_duration_us>input.command.duration_us ||
       input.passes<1 || input.passes>2 || input.passes!=input.timing.substeps ||
       !authority::valid_timing(input.timing) ||
       !ResourceInventory::inspect(resources.memory()).can_start_worker())return fail_frame("input, timing or resource inventory unavailable");
    FloatingPointScope floating(input.entry);
    if(!floating.valid())return fail_frame("frame floating-point context unavailable");
    recomp_context context{};
    if(!input.entry.restore(context) ||
       context.r29!=engine::guest_address(static_cast<unsigned>(context.r29)) ||
       (context.r29&7) || !engine::valid_guest_range(static_cast<unsigned>(context.r29)-4096,4096))return fail_frame("invalid historical CPU context or game stack");
    if(!prepare_worker_entry())return fail_frame("invalid private worker context");
    ReplayScope scope(input.session);
    if(!scope.valid() || !seed_cop_rules(input.rules) || !seed_cop_posts(input.posts) ||
       !seed_manual_eject(input.eject))return fail_frame("native shadow state unavailable");
    if(!bind_item_state(resources.memory(),input.items))return fail_frame("historical item state unavailable");
    if(!rr64::online_terrain::bind_replay(resources.memory(),resources.rom()))
        return fail_frame("historical collision bank unavailable");
    std::uint16_t actor_ai=0;
    if(input.actor<0x800d8570u || (input.actor-0x800d8570u)%0x118u ||
       (input.actor-0x800d8570u)/0x118u>=14 ||
       !engine::read_u16(resources.memory(),input.actor+0x26,actor_ai))return fail_frame("invalid replay actor");
    const bool ai_replay=actor_ai!=0;
    if(ai_replay && (input.command.buttons || input.command.actions || input.previous_buttons))
        return fail_frame("AI replay cannot inject player actions");
    Controls controls(input.actor,input.command,input.previous_buttons,ai_replay);
    if(!controls.valid())return fail_frame("invalid replay controls");
    struct Binding {
        Binding(Resources& value){frame_resources=&value;frame_blocks=2000000;}
        ~Binding(){frame_resources=nullptr;frame_blocks=0;}
    } binding(resources);
    try {
        if(!restore_timing(resources.memory(),input.timing) ||
           !restore_update_counters(resources.memory(),input.counters) ||
           !validate_visibility(resources.memory(),input.visibility))return fail_frame("timing or visibility restore failed");
        const auto slot=(input.actor-0x800D8570u)/0x118u;
        auto eject=input.eject;
        if(!ai_replay && (!authority::eject_step(resources.memory(),slot,(input.command.actions&authority::action_eject)!=0,
            eject,[&](unsigned bike){
                CpuContext saved;recomp_context call{};
                if(!saved.capture(context) || !saved.restore(call))return false;
                call.r4=engine::guest_address(bike);
                func_8003F0E8(resources.memory(),&call);return true;
            }) || !seed_manual_eject(eject)))return fail_frame("eject replay rejected");
        VisibilityScope visibility_scope(input.visibility);
        TerrainAvailabilityScope terrain_scope(input.terrain);
#ifdef RR64_EXPERIMENTAL_COURSE
        // The generated private prefix deliberately excludes live entry hooks.
        // Apply only its bound historical physics effects once before native
        // substeps; this cannot consume input, step objects, or emit cues.
        rr64_mk64_items_before_physics(resources.memory());
#endif
        native_frame(resources.memory(),&context);
        FrameOutput completed{};
        if(!controls.completed(ai_replay?0:input.passes) || !replay_cop_rules(completed.rules) ||
           !replay_cop_posts(completed.posts) || !replay_manual_eject(completed.eject))return fail_frame("control pass count or native output mismatch");
        if(!complete_item_state(input.replay_duration_us?input.replay_duration_us:input.command.duration_us,
                                completed.items))return fail_frame("historical item completion unavailable");
        output=completed;return true;
    }catch(const Resources::Invalid &error){return fail_frame(error.what());}
     // Missing historical wall data discards only this correction transaction.
     catch(const rr64::online_terrain::CollisionUnavailable &error){return fail_frame(error.what());}
     catch(const Resources::Blocked&){return fail_frame("private resource wait blocked");}
}
} // rr64::prediction
