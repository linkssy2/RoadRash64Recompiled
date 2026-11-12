static void seed(Workload &w, unsigned transforms, unsigned triangles, unsigned mode) {
    w.reset(); auto &d=w.drawData;
    d.transformGroups.clear(); d.worldTransformGroups.clear(); d.worldTransforms.clear(); d.worldTransformVertexIndices.clear();
    TransformGroup camera{}; d.transformGroups.push_back(camera); d.viewProjTransformGroups={0};
    auto identity=hlslpp::float4x4::identity(); d.viewTransforms={identity};d.projTransforms={identity};d.viewProjTransforms={identity};
    w.presentationScene={1,true}; w.fbPairCount=1;
    auto &pair=w.fbPairs[0];pair.projections.resize(mode==4?2:1);pair.projectionCount=unsigned(pair.projections.size());
    for(auto &p:pair.projections){p.reset();p.type=Projection::Type::Perspective;p.transformsIndex=0;}
    const unsigned vertices=triangles+2;
    for(unsigned t=0;t<transforms;++t) {
        TransformGroup group{};group.matrixId=(mode>=11&&mode<=14)?G_EX_ID_AUTO:0x52510000+t;group.ordering=(mode>=11&&mode<=14)?G_EX_ORDER_AUTO:G_EX_ORDER_LINEAR;
        d.transformGroups.push_back(group);d.worldTransformGroups.push_back(t+1);d.worldTransforms.push_back(identity);
        unsigned first=d.vertexCount();d.worldTransformVertexIndices.push_back(first);
        for(unsigned v=0;v<vertices;++v){d.posFloats.insert(d.posFloats.end(),{float(v),float(v%7),float(t%3)});d.worldIndices.push_back(uint16_t(t));d.tcFloats.insert(d.tcFloats.end(),{0,0});}
        if(mode==11||mode==13||mode==14)for(unsigned i=first*3+2;i<d.posFloats.size();i+=3)d.posFloats[i]=float(t);
        GameCall c{};c.callDesc.minWorldMatrix=c.callDesc.maxWorldMatrix=uint16_t(t);c.callDesc.triangleCount=triangles;c.meshDesc.faceIndicesStart=unsigned(d.faceIndices.size());
        for(unsigned i=0;i<triangles;++i)d.faceIndices.insert(d.faceIndices.end(),{first,first+i+1,first+i+2});
        pair.projections[0].addGameCall(c);
        if(mode==4&&t%2==0)pair.projections[1].addGameCall(c);
    }
    d.velFloats.resize(d.posFloats.size());d.tcVelFloats.resize(d.tcFloats.size());
    if(mode==1)d.worldIndices[0]=65535;
    if(mode==2)d.worldIndices[0]=1;
    if(mode==3)d.faceIndices[1]=d.faceIndices[0];
    if(mode==6)d.posFloats[0]=std::numeric_limits<float>::quiet_NaN();
    if(mode==8)d.posFloats[0]=std::numeric_limits<float>::infinity();
    if(mode==9)for(unsigned i=0;i<d.posFloats.size();i+=3)d.posFloats[i]+=float(i)*0.25f;
    if(mode==10)for(auto &p:pair.projections)for(auto &c:p.gameCalls)c.callDesc.triangleCount=0;
    if(mode==15)++d.worldTransformVertexIndices.back();
    if(mode==16){
        for(unsigned t=0;t<transforms;++t){auto& group=d.transformGroups[t+1];group.matrixId=G_EX_ID_AUTO;group.ordering=G_EX_ORDER_AUTO;}
        // reset() retains storage. Repeating inactive entries would multiply
        // the fixture every time reuse-mode 3 seeds the same workload slot.
        const auto &projection=pair.projections[0];
        const std::vector<GameCall> calls(projection.gameCalls.begin(),
            projection.gameCalls.begin()+projection.gameCallCount);
        for(unsigned repeat=0;repeat<7;++repeat)for(const auto& c:calls)pair.projections[0].addGameCall(c);
    }
    // Candidate-index holdouts: mixed explicit/automatic identity, then
    // multi-transform calls that must keep the original matching path.
    if(mode==17||mode==18){
        for(unsigned t=0;t<transforms;++t)if(mode==18||t%2){
            auto& group=d.transformGroups[t+1];group.matrixId=G_EX_ID_AUTO;group.ordering=G_EX_ORDER_AUTO;
        }
        if(mode==18)for(unsigned t=0;t+1<transforms;t+=2)
            pair.projections[0].gameCalls[t].callDesc.maxWorldMatrix=uint16_t(t+1);
    }
    if(mode>=19&&mode<=22){
        d.rdpTiles.resize(transforms);d.callTiles.resize(transforms);
        d.rspLookAt.resize(transforms);d.lookAtIndices.resize(d.vertexCount());
        for(unsigned t=0;t<transforms;++t){
            auto &group=d.transformGroups[t+1];group.matrixId=G_EX_ID_AUTO;group.ordering=G_EX_ORDER_AUTO;
            auto &call=pair.projections[0].gameCalls[t];
            if(mode==19||mode==22||(mode==21&&t%2==0)){
                group.tileInterpolation=G_EX_COMPONENT_INTERPOLATE;
                call.callDesc.tileCount=1;call.callDesc.tileIndex=t;
                auto &tile=d.rdpTiles[t];tile.masks=tile.maskt=5;tile.lrs=tile.lrt=16;
                d.callTiles[t].tmemHashOrID=t;
                // Same material hash, mixed eligibility: one unsafe previous
                // candidate must keep the whole bucket on the original path.
                if(mode==22&&t%2)group.tileInterpolation=G_EX_COMPONENT_SKIP;
            }
            if(mode==20||(mode==21&&t%2)){
                call.callDesc.geometryMode=G_LIGHTING|G_TEXTURE_GEN;
                d.rspLookAt[t].x={1,0,0};d.rspLookAt[t].y={0,1,0};
                for(unsigned v=0;v<vertices;++v)d.lookAtIndices[t*vertices+v]=t<<RSP_LOOKAT_INDEX_SHIFT;
            }
        }
    }
}
