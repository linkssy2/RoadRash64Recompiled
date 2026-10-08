#include "hle/rt64_state.h"
#include "hle/rt64_interpreter.h"
#include "gbi/rt64_gbi_extended.h"
#include "include/rt64_extended_gbi.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
using namespace RT64;
extern "C" void rr64_record_authored_sample(unsigned long long, unsigned long long,
    unsigned long long, unsigned int, unsigned int, unsigned long long, unsigned long long) { std::abort(); }
extern "C" void rr64_record_source_cadence(unsigned int, unsigned int, unsigned int,
    unsigned int, unsigned int, unsigned int) { std::abort(); }
static void noInterrupts() { std::abort(); }
static void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "triangle batch FAIL: %s\n", message); std::exit(1); }
}
static uint32_t encoded(unsigned a, unsigned b, unsigned c) {
    return 0x05010101u | ((a & 127u) << 17) | ((b & 127u) << 9) | ((c & 127u) << 1);
}
static void scalar(RSP &rsp, const uint32_t *words, size_t count) {
    for (size_t i = 0; i < count; i++) rsp.drawIndexedTri((words[i] >> 17) & 127u,
        (words[i] >> 9) & 127u, (words[i] >> 1) & 127u);
}
static void batched(RSP &rsp, const uint32_t *words, size_t count) {
#ifdef RR64_TRIANGLE_SCALAR_ONLY
    scalar(rsp, words, count); // Allows the same logical fixture to link the preserved R22 library.
#else
    rsp.drawIndexedTriBatch(words, uint32_t(count));
#endif
}
static std::vector<uint32_t> packet(unsigned slots, unsigned count, unsigned salt) {
    std::vector<uint32_t> result;
    for (unsigned i = 0; i < count; i++) {
        unsigned a = (i * 7 + salt) % slots, b = (i * 11 + 1 + salt) % slots, c = (i * 17 + 2 + salt) % slots;
        if (i % 9 == 0) b = a;
        if (i % 13 == 0) c = b;
        if (i % 7 == 0) std::swap(a, c);
        result.push_back(encoded(a,b,c));
    }
    return result;
}
struct Fixture {
    std::vector<uint8_t> ram = std::vector<uint8_t>(4 * 1024 * 1024);
    uint32_t interrupts = 0;
    std::unique_ptr<State> state = std::make_unique<State>(ram.data(), &interrupts, noInterrupts);
    std::unique_ptr<WorkloadQueue> queue = std::make_unique<WorkloadQueue>();
    std::unique_ptr<Interpreter> interpreter = std::make_unique<Interpreter>();
    EmulatorConfiguration emulator;
    GBI gbi{};
    RSP &rsp() { return *state->rsp; }
    RDP &rdp() { return *state->rdp; }
    Workload &workload() { return queue->workloads[0]; }
    Fixture() {
        interpreter->hleGBI = &gbi;
        emulator.framebuffer.copyWithGPU = false;
        state->ext = {}; state->ext.workloadQueue = queue.get();
        state->ext.interpreter = interpreter.get(); state->ext.emulatorConfig = &emulator;
    }
    void reset(unsigned cull, unsigned tileCount, unsigned scissor, unsigned camera, unsigned slots, bool depth) {
        queue->writeCursor = 0; workload().reset(); state->reset();
        state->drawCall.callIndex = 0;
        auto &r = rsp(); auto &d = workload().drawData;
        r.cullFrontMask = 0x200; r.cullBothMask = 0x600;
        r.geometryModeStack[0] = G_SHADE | ((cull & 1) ? 0x200 : 0) | ((cull & 2) ? 0x400 : 0);
        r.otherModeStack[0] = {depth ? Z_UPD : 0u, G_TL_LOD}; rdp().otherMode = r.otherModeStack[0];
        r.textureState.on = tileCount ? 1 : 0; r.textureState.levels = uint8_t(tileCount);
        r.textureState.sc = 65535; r.textureState.tc = 49152;
        r.projectionIndex = 0; r.projectionMatrixChanged = false; r.viewportChanged = false;
        r.modelViewProjChanged = false; r.modelViewProjInserted = false;
        r.extended.modelMatrixIdStackChanged = false; r.extended.viewProjMatrixIdStackChanged = false;
        r.lightsChanged = false; r.lookAtChanged = false; r.fogChanged = false;
        r.curViewProjIndex = 0;
        r.viewportStack[0].scale = {160,120,512}; r.viewportStack[0].translate = {160,120,512};
        r.projMatrixStack[0] = hlslpp::float4x4::identity();
        if (camera) r.projMatrixStack[0][3][3] = 0.0f;
        const float wz = camera == 2 ? 0.011f : (camera == 1 ? 0.002f : 0.0f);
        const float ww = camera == 3 ? 0.02f : (camera == 2 ? 0.251f : 1.0f);
        r.modelViewProjMatrix = hlslpp::float4x4(0.008f,0.001f,0,0, 0,0.008f,0,0,
            0,0,0.002f,wz, 0,0,0,ww);
        rdp().colorImage = {}; rdp().colorImage.width = 320; rdp().colorImage.siz = G_IM_SIZ_16b;
        rdp().depthImage = {};
        for (auto &tile : rdp().tiles) {
            tile = {}; tile.fmt = G_IM_FMT_I; tile.siz = G_IM_SIZ_8b; tile.line = 2;
            tile.lrs = 60; tile.lrt = 60; tile.masks = 4; tile.maskt = 4;
        }
        std::fill(std::begin(rdp().tileReplacementHashes), std::end(rdp().tileReplacementHashes), 0);
        rdp().scissorRectStack[0] = {0,0,1280,960};
        if (scissor == 1) rdp().scissorRectStack[0] = {200,100,850,750};
        if (scissor == 2) rdp().scissorRectStack[0] = {2200,1900,2600,2300};
        if (scissor == 3) rdp().scissorRectStack[0].reset();
        d.worldTransforms.resize(64, hlslpp::float4x4::identity());
        auto *source = reinterpret_cast<RSP::Vertex *>(ram.data() + 0x1000);
        for (unsigned i = 0; i < 128; i++) {
            source[i] = {}; source[i].x = int16_t((i * 47) % 301 - 150);
            source[i].y = int16_t((i * 83) % 261 - 130); source[i].z = int16_t(int(i) * 4 - 100);
            source[i].s = int16_t(int(i) * 619 - 14000); source[i].t = int16_t(int(i) * 431 - 16000);
            source[i].color.r = uint8_t(i * 7); source[i].color.g = uint8_t(i * 11);
            source[i].color.b = uint8_t(i * 23); source[i].color.a = uint8_t(i * 29);
        }
        for (unsigned i = 0; i < slots; i += 8) {
            r.curTransformIndex = 1 + ((i / 8) * 11) % 53;
            r.setVertex(0x1000 + i * sizeof(RSP::Vertex), 8, i);
        }
        for (const auto &p : d.posScreen) {
            require(std::isfinite(float(p.x)) && std::isfinite(float(p.y)) && std::isfinite(float(p.z)), "screen fixture must be finite");
        }
    }
    void changeState(unsigned tileCount, bool depth) {
        rdp().primColorStack[0] = {0.125f,0.5f,0.75f,1.0f};
        rdp().scissorRectStack[0] = {-111,-73,901,831};
        rsp().otherModeStack[0].L = depth ? Z_UPD : 0u; rdp().otherMode = rsp().otherModeStack[0];
        rsp().textureState.on = tileCount ? 1 : 0; rsp().textureState.levels = uint8_t(tileCount);
        state->updateDrawStatusAttribute(DrawAttribute::PrimColor);
        state->updateDrawStatusAttribute(DrawAttribute::Scissor);
        state->updateDrawStatusAttribute(DrawAttribute::OtherMode);
    }
    void changeProjection() { rsp().curViewProjIndex++; }
    void modify(unsigned index) {
        rsp().modifyVertex(uint16_t(index), G_MWO_POINT_ST, 0xFFA1102F);
        rsp().modifyVertex(uint16_t(index), G_MWO_POINT_RGBA, 0x13ABCDFF);
        rsp().modifyVertex(uint16_t(index), G_MWO_POINT_XYSCREEN, 0xFEA304B7);
        rsp().modifyVertex(uint16_t(index), G_MWO_POINT_ZSCREEN, 0x0040C000);
    }
};
// Serialize logical fields individually: never compare struct padding or the
// unused fourth SIMD lane in a float3. Every checkpoint compares full vectors.
struct Snapshot {
    std::vector<uint64_t> values;
    template<class T> void add(T x) { values.push_back(uint64_t(x)); }
    void f(float value) { uint32_t bits; std::memcpy(&bits,&value,sizeof(bits)); add(bits); }
    void rect(const FixedRect &r) { add(r.ulx);add(r.uly);add(r.lrx);add(r.lry); }
    template<class T> void ints(const std::vector<T> &v) { add(v.size());for(auto x:v)add(x); }
    void floats(const std::vector<float> &v) { add(v.size());for(float x:v)f(x); }
    void call(const DrawCall &c) {
        add(c.uid);add(c.callIndex);add(c.triangleCount);add(c.minWorldMatrix);add(c.maxWorldMatrix);
        rect(c.rect);rect(c.scissorRect);add(c.scissorMode);add(c.scissorLeftOrigin);add(c.scissorRightOrigin);
        add(c.geometryMode);add(c.objRenderMode);add(c.cullBothMask);add(c.shadingSmoothMask);add(c.NoN);
        add(c.textureOn);add(c.textureTile);add(c.textureLevels);add(c.tileIndex);add(c.tileCount);
        add(c.loadIndex);add(c.loadCount);add(c.otherMode.L);add(c.otherMode.H);
        add(c.colorCombiner.L);add(c.colorCombiner.H);add(c.fillColor);add(c.extendedType);add(c.drawStatusChanges);
        for(unsigned i=0;i<4;i++){f(c.rdpParams.primColor[i]);f(c.rdpParams.envColor[i]);f(c.rdpParams.fogColor[i]);f(c.rdpParams.blendColor[i]);}
    }
};
static Snapshot snapshot(Fixture &f) {
    Snapshot s;auto &w=f.workload();auto &d=w.drawData;
    s.ints(d.faceIndices);s.ints(d.worldIndices);s.ints(d.viewProjIndices);
    s.floats(d.posFloats);s.floats(d.velFloats);s.floats(d.tcFloats);s.floats(d.tcVelFloats);
    s.ints(d.normColBytes);s.ints(d.fogIndices);s.ints(d.lightIndices);s.ints(d.lightCounts);s.ints(d.lookAtIndices);s.ints(d.modifyPosUints);
    s.add(d.posScreen.size());for(const auto &p:d.posScreen){s.f(p.x);s.f(p.y);s.f(p.z);}
    s.add(d.posTransformed.size());for(const auto &p:d.posTransformed)for(unsigned i=0;i<4;i++)s.f(p[i]);
    for(auto i:f.rsp().indices)s.add(i);for(size_t i=0;i<f.rsp().used.size();i++)s.add(f.rsp().used[i]);
    s.add(f.rsp().curViewProjIndex);s.add(f.rsp().projectionIndex);s.add(f.state->drawStatus.changed);s.call(f.state->drawCall);
    s.add(d.callTiles.size());for(const auto &t:d.callTiles){
        s.add(t.minTexcoord.x);s.add(t.minTexcoord.y);s.add(t.maxTexcoord.x);s.add(t.maxTexcoord.y);
        s.add(t.valid);s.add(t.tileCopyUsed);s.add(t.syncRequired);s.add(t.tmemHashOrID);
        s.add(t.sampleWidth);s.add(t.sampleHeight);s.add(t.lineWidth);s.add(t.tlut);
    }
    s.add(d.rdpTiles.size());for(const auto &t:d.rdpTiles){
        s.add(t.fmt);s.add(t.siz);s.add(t.stride);s.add(t.address);s.add(t.palette);s.add(t.masks);s.add(t.maskt);
        s.f(t.shifts);s.f(t.shiftt);s.f(t.uls);s.f(t.ult);s.f(t.lrs);s.f(t.lrt);s.add(t.cms);s.add(t.cmt);s.add(t.nativeSampler);
    }
    s.add(w.gameCallCount);s.add(w.fbPairCount);s.add(w.fbPairSubmitted);
    for(unsigned i=0;i<w.fbPairCount;i++){
        const auto &p=w.fbPairs[i];s.add(p.projectionCount);s.add(p.projectionStart);s.add(p.gameCallCount);
        s.add(p.depthRead);s.add(p.depthWrite);s.add(p.fillRectOnly);s.add(p.syncRequired);s.add(p.flushReason);
        s.rect(p.drawColorRect);s.rect(p.drawDepthRect);s.rect(p.scissorRect);
        for(auto count:p.ditherPatterns)s.add(count);
        for(unsigned j=0;j<p.projectionCount;j++){
            const auto &projection=p.projections[j];s.add(projection.type);s.add(projection.transformsIndex);
            s.add(projection.gameCallCount);s.rect(projection.scissorRect);
            for(unsigned k=0;k<projection.gameCallCount;k++)s.call(projection.gameCalls[k].callDesc);
        }
    }
    return s;
}
static uint64_t digest(const Snapshot &s) {return XXH3_64bits(s.values.data(),s.values.size()*sizeof(uint64_t));}
static std::vector<uint64_t> checkpoints;
static void compare(Fixture &a,Fixture &b,unsigned scenario,const char *phase) {
    auto x=snapshot(a),y=snapshot(b);
    if(x.values!=y.values){
        size_t i=0;while(i<std::min(x.values.size(),y.values.size())&&x.values[i]==y.values[i])i++;
        std::fprintf(stderr,"scenario=%u phase=%s field=%zu scalar_size=%zu batch_size=%zu\n",scenario,phase,i,x.values.size(),y.values.size());
        require(false,"scalar/batch logical output differs");
    }
    checkpoints.push_back(digest(x));
}
static void submit(Fixture &a,Fixture &b,const std::vector<uint32_t>& words) {
    scalar(a.rsp(),words.data(),words.size());batched(b.rsp(),words.data(),words.size());
}
static void extended(Fixture &f,std::vector<DisplayList>& commands) {
    DisplayList *cursor=commands.data();
#ifdef RR64_TRIANGLE_SCALAR_ONLY
    const uint32_t count=cursor->w1;
    for(uint32_t i=1;i<=count;i++){
        scalar(f.rsp(),&commands[i].w0,1);
        if((commands[i].w0>>24)==6)scalar(f.rsp(),&commands[i].w1,1);
    }
    cursor+=count;
#else
    GBI_EXTENDED::extendedOp(f.state.get(),&cursor);
#endif
    require(cursor==commands.data()+commands[0].w1,"extended command pointer must end at final payload command");
    cursor++;require(cursor->w0==0xDF000000u,"interpreter increment must reach untouched next command");
}
int main(int argc,char **argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
#endif
    GBI_EXTENDED::initialize();Fixture a,b;unsigned scenarios=0,handlers=0;
    for(unsigned cull=0;cull<4;cull++)for(unsigned tiles=0;tiles<3;tiles++)for(unsigned scissor=0;scissor<4;scissor++)
    for(unsigned camera=0;camera<4;camera++)for(unsigned slots:{32u,128u})for(bool depth:{false,true}){
        a.reset(cull,tiles,scissor,camera,slots,depth);b.reset(cull,tiles,scissor,camera,slots,depth);
        auto before=snapshot(b);batched(b.rsp(),nullptr,0);require(before.values==snapshot(b).values,"zero triangles must preserve even pending state");
        submit(a,b,packet(slots,1,0));compare(a,b,scenarios,"one");
        submit(a,b,packet(slots,40,3));compare(a,b,scenarios,"packet");
        a.modify(slots/3);b.modify(slots/3);submit(a,b,packet(slots,129,17));compare(a,b,scenarios,"modified vertex");
        a.changeState((tiles+1)%3,!depth);b.changeState((tiles+1)%3,!depth);
        submit(a,b,packet(slots,2,4));compare(a,b,scenarios,"state boundary");
        a.changeProjection();b.changeProjection();submit(a,b,packet(slots,67,9));compare(a,b,scenarios,"projection boundary");
        a.state->flush();b.state->flush();compare(a,b,scenarios,"final flush");scenarios++;
    }
    for(unsigned count:{1u,2u,63u,64u,65u,85u,86u,127u,128u,129u,257u})for(unsigned mode:{0u,1u,2u}){
        a.reset(mode,2,1,2,128,true);b.reset(mode,2,1,2,128,true);
        auto words=packet(128,count*2,19);std::vector<DisplayList> commands(count+2);
        commands[0].w0=0xE0000034;commands[0].w1=count;unsigned t=0;
        for(unsigned i=0;i<count;i++){
            bool two=mode==1||(mode==2&&i%3);commands[i+1].w0=(words[t++]&0x00FFFFFFu)|(two?0x06000000u:0x05000000u);
            commands[i+1].w1=two?words[t++]:0xBAADF00Du;
            scalar(a.rsp(),&commands[i+1].w0,1);if(two)scalar(a.rsp(),&commands[i+1].w1,1);
        }
        commands.back().w0=0xDF000000;commands.back().w1=0xC0FFEE;
        const auto original=commands;extended(b,commands);
        require(std::memcmp(original.data(),commands.data(),commands.size()*sizeof(DisplayList))==0,"batch handler must not edit command words");
        compare(a,b,scenarios+handlers,"extended handler");a.state->flush();b.state->flush();compare(a,b,scenarios+handlers,"extended flush");handlers++;
    }
    const auto allDigest=XXH3_64bits(checkpoints.data(),checkpoints.size()*sizeof(uint64_t));
    std::printf("triangle_batch scenarios=%u checkpoints=%zu handler_cases=%u all_fields_equal=1 digest=%llu\n",scenarios,checkpoints.size(),handlers,(unsigned long long)allDigest);
    if(argc>2&&std::strcmp(argv[1],"--dump")==0){FILE *f=std::fopen(argv[2],"wb");require(f!=nullptr,"open digest record output");
        require(std::fwrite(checkpoints.data(),sizeof(uint64_t),checkpoints.size(),f)==checkpoints.size(),"write digest records");std::fclose(f);}
    if(argc>3&&std::strcmp(argv[3],"--no-benchmark")==0)return 0;
    const unsigned packetTriangles = argc > 1 && std::strcmp(argv[1],"--size")==0 ? unsigned(std::atoi(argv[2])) : 40u;
    std::vector<double> scalarMs,batchMs;auto words=packet(32,packetTriangles,1);uint64_t checksum=0;
    for(unsigned round=0;round<25;round++)for(unsigned method=0;method<2;method++){
        Fixture &f=method?b:a;f.reset(0,1,0,0,32,true);
        const auto start=std::chrono::steady_clock::now();
        for(unsigned p=0;p<5000;p++){
            f.rsp().setVertex(0x1000,32,0);
            if(method)batched(f.rsp(),words.data(),words.size());else scalar(f.rsp(),words.data(),words.size());
        }
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(round>=5)(method?batchMs:scalarMs).push_back(ms);
        require(f.workload().drawData.faceIndices.size()==size_t(5000)*packetTriangles*3&&f.state->drawCall.triangleCount==5000*packetTriangles,"benchmark must submit every triangle");
        checksum+=f.workload().drawData.vertexCount()+f.workload().drawData.faceIndices.back();
    }
    compare(a,b,scenarios+handlers,"benchmark final exact output");
    std::sort(scalarMs.begin(),scalarMs.end());std::sort(batchMs.begin(),batchMs.end());
    std::printf("triangle_batch packets=5000 vertices_per_packet=32 triangles_per_packet=%u scalar_median_ms=%.6f batch_median_ms=%.6f checksum=%llu\n",
        packetTriangles,scalarMs[scalarMs.size()/2],batchMs[batchMs.size()/2],(unsigned long long)checksum);
    return 0;
}

