#include "hle/rt64_rr64_owned_frame_batch.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>
using namespace RT64;
using namespace RT64::RR64FramePacing;
static void require(bool v,const char* message){if(!v){std::fprintf(stderr,"Texture reuse FAIL: %s\n",message);std::exit(1);}}
struct Texture final : RenderTexture {
    uint64_t content;
    explicit Texture(uint64_t value):content(value){}
    std::unique_ptr<RenderTextureView> createTextureView(const RenderTextureViewDesc&) const override {return {};}
    void setName(const std::string&) override {}
};
static PresentationTargetIdentity target(uint32_t address=0x100000){return {address,320,2};}
static auto image(uint64_t writer,uint32_t width=1920,uint32_t height=1080,bool hdr=false){
    auto value=std::make_unique<RenderTarget>(target().address,Framebuffer::Type::Color,RenderMultisampling(),hdr);
    value->width=width;value->height=height;value->format=RenderTarget::colorBufferFormat(hdr);
    value->texture=std::make_unique<Texture>(writer);return value;
}
static auto batch(uint64_t writer, uint32_t address=0x100000, SceneSnapshot scene={1,true},uint64_t epoch=1){
    auto value=std::make_unique<OwnedFrameBatch>();value->metadata={writer,scene,target(address),60,60};
    value->resourceEpoch=epoch;value->images.push_back(image(writer));return value;
}
static void fill(OwnedFrameBatchCache& cache){for(uint64_t i=1;i<=4;++i)require(cache.publishAfterGpuWait(batch(i)),"publish fixture");}
#include "rr64_texture_reuse_gpu_fixture.hpp"
int main(int argc,char**argv){
    if(argc==2 && std::string(argv[1])=="--gpu")return gpuBenchmark();
    {
        OwnedFrameBatchCache cache;fill(cache);
        auto reused=cache.takeRecycledImage({1,true},1,target(),1920,1080,false);
        require(reused && static_cast<Texture*>(reused->texture.get())->content==1,"only evicted image reused");
        auto* oldTexture=reused->texture.get();
        require(!reused->resize(nullptr,1920,1080) && reused->texture.get()==oldTexture,"real resize avoids allocating compatible texture");
        require(!cache.find({1,{1,true},target(),60,60},1),"evicted writer not findable");
        auto live=cache.find({4,{1,true},target(),60,60},1);
        require(live && static_cast<Texture*>(live->images[0]->texture.get())->content==4,"live writer untouched");
    }
    {
        OwnedFrameBatchCache cache;require(cache.publishAfterGpuWait(batch(1)),"initial");
        auto lease=cache.find({1,{1,true},target(),60,60},1);
        std::atomic<bool> finished{false};
        std::thread consumer([&]{while(!finished.load())require(static_cast<Texture*>(lease->images[0]->texture.get())->content==1,"leased pixels immutable");});
        for(uint64_t i=2;i<200;++i){require(cache.publishAfterGpuWait(batch(i)),"publish while leased");
            auto reuse=cache.takeRecycledImage({1,true},1,target(),1920,1080,false);
            if(reuse)require(static_cast<Texture*>(reuse->texture.get())->content!=1,"present lease cannot be recycled");}
        cache.clear();require(static_cast<Texture*>(lease->images[0]->texture.get())->content==1,"clear preserves present lease");
        finished=true;consumer.join();lease.reset();
        require(!cache.takeRecycledImage({1,true},1,target(),1920,1080,false),"clear drains pool");
    }
    for(int change=0;change<7;++change){
        OwnedFrameBatchCache cache;fill(cache);
        auto scene=SceneSnapshot{1,true};uint64_t epoch=1;auto identity=target();unsigned width=1920,height=1080;bool hdr=false;
        switch(change){case 0:scene.epoch=2;break;case 1:epoch=2;break;case 2:width=1280;break;
            case 3:height=720;break;case 4:hdr=true;break;case 5:identity.width=640;break;case 6:cache.eraseAddress(identity.address);break;}
        require(!cache.takeRecycledImage(scene,epoch,identity,width,height,hdr),"incompatible or invalidated image rejected");
        require(cache.reuseStats().images==0,"obsolete image released");
    }
    {
        OwnedFrameBatchCache cache;
        for(uint64_t i=1;i<=30;++i){auto b=batch(i);b->images[0]=image(i,4096,4096);require(cache.publishAfterGpuWait(std::move(b)),"large batch");}
        require(cache.reuseStats().bytes<=OwnedFrameBatchCache::MaximumRecycledBytes,"pool byte bound");
        require(cache.reuseStats().images==1,"64MiB pool holds one64MiB target");
        require(cache.estimatedBytes()<=OwnedFrameBatchCache::MaximumCacheBytes+OwnedFrameBatchCache::MaximumRecycledBytes,"total base storage bounded");
    }
    {
        OwnedFrameBatchCache cache;uint64_t allocated=0,reusedCount=0;
        // Three rotating guest framebuffer addresses, with real cache version
        // eviction and two generated pictures per authored writer.
        for(uint64_t i=1;i<=10000;++i){
            const auto identity=target(0x100000+uint32_t(i%3)*0x30000);
            auto b=batch(i,identity.address);b->images.clear();b->metadata.targetRate=120;
            for(unsigned f=0;f<2;++f){auto next=cache.takeRecycledImage({1,true},1,identity,1920,1080,false);
                if(next){++reusedCount;require(!next->resize(nullptr,1920,1080),"steady state no allocation");}
                else{next=image(i);++allocated;}
                static_cast<Texture*>(next->texture.get())->content=i;b->images.push_back(std::move(next));}
            require(cache.publishAfterGpuWait(std::move(b)),"multi-image publish");
            auto lease=cache.find({i,{1,true},identity,60,120},1);
            require(lease && lease->images.size()==2,"exact writer lookup after reuse");
            for(const auto& entry:lease->images)require(static_cast<Texture*>(entry->texture.get())->content==i,"recycled picture has current writer");
        }
        require(allocated<=24 && reusedCount>=19976,"allocation count bounded after warmup");
        std::printf("Texture reuse: %llu creations /20000 images, %llu reused; synthetic resources, no GPU timing.\n",
            static_cast<unsigned long long>(allocated),static_cast<unsigned long long>(reusedCount));
    }
    std::puts("Leases, concurrent reader, clear/address/epoch/size/HDR invalidation, bounds and real resize fast path passed.");
}
