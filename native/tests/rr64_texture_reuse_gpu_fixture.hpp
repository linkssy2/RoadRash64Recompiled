#pragma once
#include <chrono>
#include <algorithm>
#ifdef _WIN32
namespace plume { std::unique_ptr<RenderInterface> CreateD3D12Interface(); }
// No ROM, window, swap chain or game runtime. Exercise actual allocation,
// full-frame GPU copies and readback against the cache's reuse policy.
static int gpuBenchmark(){
    std::fprintf(stderr,"GPU fixture: interface\n");
    auto api=plume::CreateD3D12Interface();require(bool(api),"D3D12 interface");
    std::fprintf(stderr,"GPU fixture: device\n");
    auto device=api->createDevice();require(bool(device),"D3D12 device");
    std::fprintf(stderr,"GPU fixture: worker\n");
    RenderWorker worker(device.get(),"Texture reuse offline test",RenderCommandListType::DIRECT);
    constexpr unsigned width=1920,height=1080;
    RenderTarget source(0x100000,Framebuffer::Type::Color,RenderMultisampling(),false);
    std::fprintf(stderr,"GPU fixture: source\n");
    {RenderWorkerExecution commands(&worker);source.resize(&worker,width,height);source.setupColorFramebuffer(&worker);}
    auto readback=device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(width)*height*4));
    require(bool(readback),"readback buffer");
    std::fprintf(stderr,"GPU fixture: copies\n");
    std::vector<double> baseline,recycled;
    for(unsigned round=0;round<6;++round){
        const bool enabled=(round%2)!=0;
        OwnedFrameBatchCache cache(enabled);std::vector<double> samples;
        for(uint64_t writer=1;writer<=160;++writer){
            auto identity=target(0x100000+uint32_t(writer%3)*0x30000);
            auto value=std::make_unique<OwnedFrameBatch>();value->metadata={writer,{1,true},identity,60,60};value->resourceEpoch=1;
            worker.commandList->begin();
            const auto start=std::chrono::steady_clock::now();
            auto saved=enabled?cache.takeRecycledImage({1,true},1,identity,width,height,false):nullptr;
            if(!saved)saved=std::make_unique<RenderTarget>(identity.address,Framebuffer::Type::Color,RenderMultisampling(),false);
            saved->resize(&worker,width,height);
            if(writer==1)std::fprintf(stderr,"GPU fixture: allocated\n");
            const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
            if(writer>32)samples.push_back(us);
            auto* cmd=worker.commandList.get();
            cmd->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(source.texture.get(),RenderTextureLayout::COLOR_WRITE));
            cmd->setFramebuffer(source.textureFramebuffer.get());RenderColor color;color.r=float(writer%251)/255.0f;
            cmd->clearColor(0,color);
            if(writer==1)std::fprintf(stderr,"GPU fixture: cleared\n");
            RenderTextureBarrier barriers[]={RenderTextureBarrier(source.texture.get(),RenderTextureLayout::COPY_SOURCE),RenderTextureBarrier(saved->texture.get(),RenderTextureLayout::COPY_DEST)};
            cmd->barriers(RenderBarrierStage::COPY,barriers,2);cmd->copyTexture(saved->texture.get(),source.texture.get());
            cmd->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(saved->texture.get(),RenderTextureLayout::COPY_SOURCE));
            auto destination=RenderTextureCopyLocation::PlacedFootprint(readback.get(),saved->format,width,height,1,width);
            // This Plume version unconditionally reads destination.texture for
            // sample positions, even for a buffer footprint. The footprint
            // conversion uses destination.buffer; supply the non-MSAA source
            // only for that sample-position check. No production API changes.
            destination.texture=saved->texture.get();
            cmd->copyTextureRegion(destination,RenderTextureCopyLocation::Subresource(saved->texture.get()));
            if(writer==1)std::fprintf(stderr,"GPU fixture: recorded\n");
            cmd->end();worker.execute();worker.wait();
            if(writer==1)std::fprintf(stderr,"GPU fixture: waited\n");
            auto* bytes=static_cast<unsigned char*>(readback->map());require(bytes!=nullptr,"readback map");
            for(uint64_t pixel:{uint64_t(0),uint64_t(width)*height/2,uint64_t(width)*height-1}){
                require(bytes[pixel*4]==writer%251 && bytes[pixel*4+1]==0 && bytes[pixel*4+2]==0 && bytes[pixel*4+3]==255,"GPU copy contains current writer, not stale pixels");
            }
            readback->unmap();value->images.emplace_back(std::move(saved));require(cache.publishAfterGpuWait(std::move(value)),"GPU-finished batch publish");
        }
        std::sort(samples.begin(),samples.end());const double median=samples[samples.size()/2];
        (enabled?recycled:baseline).push_back(median);
        std::printf("D3D12 round%u reuse=%u median-allocation-us=%.3f hits=%llu misses=%llu\n",round,enabled,median,
            static_cast<unsigned long long>(cache.reuseStats().hits),static_cast<unsigned long long>(cache.reuseStats().misses));
    }
    std::sort(baseline.begin(),baseline.end());std::sort(recycled.begin(),recycled.end());
    std::printf("D3D12 1920x1080 median-of-rounds: allocate=%.3fus reuse=%.3fus saving=%.3fus. 960 current-writer readbacks passed. Not game FPS.\n",baseline[1],recycled[1],baseline[1]-recycled[1]);
    return 0;
}
#else
static int gpuBenchmark(){
    std::fprintf(stderr,"The optional GPU benchmark requires Windows/D3D12; CPU ownership checks work on either platform.\n");
    return 77;
}
#endif
