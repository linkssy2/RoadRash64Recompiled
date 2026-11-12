#include "hle/rt64_state.h"
#include "hle/rt64_interpreter.h"
#include "hle/rt64_workload_queue.h"
#include "include/rt64_extended_gbi.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
using namespace RT64;
extern "C" void rr64_record_authored_sample(unsigned long long, unsigned long long,
    unsigned long long, unsigned int, unsigned int, unsigned long long, unsigned long long) { std::abort(); }
extern "C" void rr64_record_source_cadence(unsigned int, unsigned int, unsigned int,
    unsigned int, unsigned int, unsigned int) { std::abort(); }
static void noInterrupts() { std::abort(); }
template<class T> void dump(FILE *f, const char *name, const std::vector<T> &v) {
    const uint32_t length = uint32_t(std::strlen(name));
    const uint64_t size = v.size();
    std::fwrite(&length, sizeof(length), 1, f); std::fwrite(name, 1, length, f);
    std::fwrite(&size, sizeof(size), 1, f);
    std::fwrite(v.data(), sizeof(T), v.size(), f);
}
// hlslpp float3 has an unused fourth SIMD lane: compare its three logical values.
template<> void dump(FILE *f, const char *name, const std::vector<hlslpp::float3> &v) {
    std::vector<float> out; out.reserve(v.size() * 3);
    for (const auto &x : v) { out.push_back(float(x.x)); out.push_back(float(x.y)); out.push_back(float(x.z)); }
    dump(f, name, out);
}
static void dumpAll(FILE *file, const DrawData &d) {
#include "rr64_rsp_vertex_fields.inc"
}
int main(int argc, char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const unsigned batch = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 24;
    const unsigned vertices = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 120000;
    const unsigned mode = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 0;
    const unsigned frames = argc > 5 ? std::strtoul(argv[5], nullptr, 10) : 170;
    if (!batch || batch > 32) return 2;
    std::vector<uint8_t> ram(4 * 1024 * 1024);
    uint32_t interrupts = 0;
    auto state = std::make_unique<State>(ram.data(), &interrupts, noInterrupts);
    auto queue = std::make_unique<WorkloadQueue>();
    auto interpreter = std::make_unique<Interpreter>();
    GBI gbi{}; interpreter->hleGBI = &gbi;
    state->ext = {}; state->ext.workloadQueue = queue.get(); state->ext.interpreter = interpreter.get();
    auto &rsp = *state->rsp;
    auto &workload = queue->workloads[0]; queue->writeCursor = 0;
    auto *source = reinterpret_cast<RSP::Vertex *>(ram.data() + 0x1000);
    for (unsigned i = 0; i < 32; ++i) {
        source[i] = {};
        source[i].x = int16_t(i * 213 - 3100); source[i].y = int16_t(1600 - i * 197);
        source[i].z = int16_t(i * 39 - 760); source[i].s = int16_t(i * 519 - 2190);
        source[i].t = int16_t(i * 787 - 2744);
        source[i].color.r = uint8_t(i * 7); source[i].color.g = uint8_t(i * 11);
        source[i].color.b = uint8_t(i * 23); source[i].color.a = uint8_t(i * 29);
        auto &ex = reinterpret_cast<RSP::VertexEXV1 *>(ram.data() + 0x2000)[i];
        ex = {}; ex.v = source[i]; ex.xp = source[i].x - 7;
        ex.yp = source[i].y + 11; ex.zp = source[i].z - 3;
        auto &pd = reinterpret_cast<RSP::VertexPD *>(ram.data() + 0x3000)[i];
        pd = {}; pd.x = source[i].x; pd.y = source[i].y; pd.z = source[i].z;
        pd.s = source[i].s; pd.t = source[i].t; pd.ci = uint16_t(i * 4);
        for (unsigned k = 0; k < 4; ++k) ram[0x4000 + i * 4 + k] = uint8_t(i * 29 + k * 13);
        for (unsigned k = 0; k < 3; ++k) {
            reinterpret_cast<float *>(ram.data() + 0x6000)[i * 3 + k] = float(i * 13 + k) * 0.25f;
            reinterpret_cast<float *>(ram.data() + 0x7000)[i * 3 + k] = float(i * 3 + k) * -0.125f;
        }
    }
    if (mode & 1024) {
        if ((uint64_t(vertices)+batch-1)/batch*32*sizeof(RSP::Vertex) > ram.size()-0x10000) return 2;
        for (unsigned j=0;j<vertices;j+=batch) {
            auto *packet=reinterpret_cast<RSP::Vertex *>(ram.data()+0x10000+(j/batch)*32*sizeof(RSP::Vertex));
            std::copy_n(source,32,packet);
            packet[0].x=int16_t(j%30000);
        }
    }
    auto prepare = [&]() {
        workload.reset(); rsp.reset();
        rsp.projectionIndex = 0; rsp.projectionMatrixChanged = false; rsp.viewportChanged = false;
        rsp.modelViewProjChanged = false; rsp.modelViewProjInserted = false;
        rsp.extended.modelMatrixIdStackChanged = false;
        rsp.modelViewProjMatrix = hlslpp::float4x4(1.2f,0.1f,0,0, 0,0.8f,0,0, 0,0,0.005f,0, 9,13,1,1);
        rsp.viewportStack[0].scale = {160,120,512}; rsp.viewportStack[0].translate = {160,120,512};
        rsp.textureState.sc = 49152; rsp.textureState.tc = 16384;
        rsp.geometryModeStack[0] = mode & 1 ? G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR : 0;
        if (mode & 2) rsp.geometryModeStack[0] |= G_FOG;
        rsp.lightsChanged = false; rsp.lookAtChanged = false; rsp.fogChanged = false;
        rsp.vertexLightIndex = 3; rsp.vertexLightCount = 2; rsp.vertexFogIndex = 2; rsp.vertexLookAtIndex = 1;
        rsp.curViewProjIndex = 5; rsp.curTransformIndex = 7;
        if (mode & 128) {
            TransformGroup terrain{}; terrain.matrixId=0x52510000;
            workload.drawData.transformGroups.push_back(terrain);
            rsp.extended.curModelMatrixIdGroupIndex=int(workload.drawData.transformGroups.size()-1);
        }
        rsp.vertexColorPDAddress = 0x4000;
        setUserFogEnabled((mode & 4) == 0);
    };
    std::vector<double> times;
    uint64_t checksum = 0;
    for (unsigned frame = 0; frame < frames + 20; ++frame) {
        prepare();
        if (mode & 256) source[0].x=int16_t(frame*17-600);
        if (mode & 512) {rsp.textureState.sc=uint16_t(65535-frame*123);rsp.textureState.tc=uint16_t(frame*419);}
        if (mode & 2048) rsp.modelViewProjMatrix[3][0]=float(frame)*0.125f;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned j = 0; j < vertices; j += batch) {
            const unsigned format = (mode & 64) ? ((j / batch) % 3) : ((mode & 16) ? 2 : ((mode & 8) ? 1 : 0));
            const uint32_t address = (mode & 1024) ? 0x10000+(j/batch)*32*sizeof(RSP::Vertex) : 0x1000 + format * 0x1000;
            if (mode & 32) {
                rsp.setVertexSegmentV1(true, G_EX_VERTEX_POSITION, 0x6000, address);
                rsp.setVertexSegmentV1(true, G_EX_VERTEX_VELOCITY, 0x7000, address);
            }
            if (format == 1) rsp.setVertexEXV1(address, std::min(batch, vertices - j), j % 16);
            else if (format == 2) rsp.setVertexPD(address, std::min(batch, vertices - j), j % 16);
            else rsp.setVertex(address, std::min(batch, vertices - j), j % 16);
        }
        const auto end = std::chrono::steady_clock::now();
        if (frame >= 20) times.push_back(std::chrono::duration<double,std::milli>(end-start).count());
        checksum += workload.drawData.vertexCount() + workload.drawData.worldIndices.back();
    }
    std::sort(times.begin(), times.end());
    std::printf("batch=%u vertices=%u mode=%u warmed_frames=%zu min_ms=%.6f median_ms=%.6f p90_ms=%.6f checksum=%llu\n",
        batch, vertices, mode, times.size(), times.front(), times[times.size()/2], times[times.size()*9/10],
        static_cast<unsigned long long>(checksum));
    if (argc > 4) {
        FILE *file = std::fopen(argv[4], "wb"); if (!file) return 3;
        dumpAll(file, workload.drawData);
        std::fwrite(rsp.indices.data(), sizeof(uint32_t), rsp.indices.size(), file);
        const auto bits = rsp.used.to_string(); std::fwrite(bits.data(), 1, bits.size(), file);
        std::fclose(file);
    }
    return 0;
}

