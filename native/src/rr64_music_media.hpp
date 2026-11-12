#pragma once

#include <filesystem>
#include <vector>
#include <cstdint>
#include <chrono>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#else
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#endif

namespace rr64::music {
// Called only by the decode worker. No video stream, network URL or UI work.
inline std::vector<std::int16_t> decode_media(const std::filesystem::path &path,
                                              std::size_t maximum_frames) {
#ifdef _WIN32
    struct Runtime {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT mf = FAILED(com) ? E_FAIL : MFStartup(MF_VERSION);
        ~Runtime() {
            if (SUCCEEDED(mf))
                MFShutdown();
            if (SUCCEEDED(com))
                CoUninitialize();
        }
    } runtime;
    if (FAILED(runtime.mf))
        return {};
    using Microsoft::WRL::ComPtr;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> format, actual;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)) ||
        FAILED(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE)) ||
        FAILED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE)) ||
        FAILED(MFCreateMediaType(&format)) ||
        FAILED(format->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
        FAILED(format->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM)) ||
        FAILED(format->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2)) ||
        FAILED(format->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000)) ||
        FAILED(format->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)) ||
        FAILED(format->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4)) ||
        FAILED(format->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000)) ||
        FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
                                           format.Get())) ||
        FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)))
        return {};
    const auto valid_format = [](IMFMediaType *type) {
        GUID subtype{};
        return SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFAudioFormat_PCM &&
               MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 0) == 2 &&
               MFGetAttributeUINT32(type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0) == 48000 &&
               MFGetAttributeUINT32(type, MF_MT_AUDIO_BITS_PER_SAMPLE, 0) == 16;
    };
    if (!valid_format(actual.Get()))
        return {};
    std::vector<std::int16_t> pcm;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (unsigned calls = 0; calls < 600000 && std::chrono::steady_clock::now() < deadline;
         ++calls) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags,
                                      nullptr, &sample)) ||
            (flags & MF_SOURCE_READERF_ERROR))
            return {};
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            actual.Reset();
            if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)) ||
                !valid_format(actual.Get()))
                return {};
        }
        if (sample) {
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
                return {};
            BYTE *data = nullptr;
            DWORD length = 0;
            if (FAILED(buffer->Lock(&data, nullptr, &length)))
                return {};
            struct Unlock {
                IMFMediaBuffer *b;
                ~Unlock() {
                    b->Unlock();
                }
            } unlock{buffer.Get()};
            if (length % 4 || length / 2 > maximum_frames * 2 - pcm.size())
                return {};
            const auto old = pcm.size();
            pcm.resize(old + length / 2);
            if (length)
                std::memcpy(pcm.data() + old, data, length);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            return pcm;
    }
#else
    struct FormatCtx {
        AVFormatContext* ctx=nullptr;
        ~FormatCtx(){if(ctx)avformat_close_input(&ctx);}
    } format;
    if(avformat_open_input(&format.ctx,path.c_str(),nullptr,nullptr)<0)return {};
    if(avformat_find_stream_info(format.ctx,nullptr)<0)return {};
    const AVCodec* codec=nullptr;
    const int stream_index=av_find_best_stream(format.ctx,AVMEDIA_TYPE_AUDIO,-1,-1,&codec,0);
    if(stream_index<0||!codec)return {};
    const AVStream* stream=format.ctx->streams[stream_index];
    struct CodecCtx {
        AVCodecContext* ctx=nullptr;
        ~CodecCtx(){avcodec_free_context(&ctx);}
    } decoder;
    decoder.ctx=avcodec_alloc_context3(codec);
    if(!decoder.ctx)return {};
    if(avcodec_parameters_to_context(decoder.ctx,stream->codecpar)<0)return {};
    if(avcodec_open2(decoder.ctx,codec,nullptr)<0)return {};
    AVChannelLayout out_layout=AV_CHANNEL_LAYOUT_STEREO;
    struct SwrCtx {
        SwrContext* ctx=nullptr;
        ~SwrCtx(){swr_free(&ctx);}
    } resampler;
    if(swr_alloc_set_opts2(&resampler.ctx,&out_layout,AV_SAMPLE_FMT_S16,48000,
        &decoder.ctx->ch_layout,decoder.ctx->sample_fmt,decoder.ctx->sample_rate,0,nullptr)<0 ||
        !resampler.ctx || swr_init(resampler.ctx)<0)return {};
    struct PacketPtr {
        AVPacket* p=av_packet_alloc();
        ~PacketPtr(){av_packet_free(&p);}
    } packet;
    struct FramePtr {
        AVFrame* p=av_frame_alloc();
        ~FramePtr(){av_frame_free(&p);}
    } frame;
    if(!packet.p||!frame.p)return {};
    std::vector<std::int16_t> pcm;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    const auto append=[&](const AVFrame* src)->bool{
        const auto delay=swr_get_delay(resampler.ctx,decoder.ctx->sample_rate);
        const auto in_samples=src?src->nb_samples:0;
        const auto out_samples=av_rescale_rnd(delay+in_samples,48000,decoder.ctx->sample_rate,AV_ROUND_UP);
        if(out_samples<=0)return true;
        if(std::size_t(out_samples)>(maximum_frames*2-pcm.size())/2)return false;
        const auto old=pcm.size();
        pcm.resize(old+std::size_t(out_samples)*2);
        std::uint8_t* out_planes[1]={reinterpret_cast<std::uint8_t*>(pcm.data()+old)};
        const int converted=swr_convert(resampler.ctx,out_planes,int(out_samples),
            src?const_cast<const std::uint8_t**>(src->data):nullptr,int(in_samples));
        if(converted<0)return false;
        pcm.resize(old+std::size_t(converted)*2);
        return true;
    };
    // Returns 1 once the decoder has drained (only reachable after a flush
    // packet), 0 when its buffer is temporarily empty, -1 on a hard error.
    const auto drain_decoder=[&]()->int{
        for(;;){
            const int received=avcodec_receive_frame(decoder.ctx,frame.p);
            if(received==AVERROR(EAGAIN))return 0;
            if(received==AVERROR_EOF)return 1;
            if(received<0)return -1;
            const bool ok=append(frame.p);
            av_frame_unref(frame.p);
            if(!ok)return -1;
        }
    };
    for(;;){
        if(std::chrono::steady_clock::now()>=deadline)return {};
        const int read=av_read_frame(format.ctx,packet.p);
        if(read<0){
            if(avcodec_send_packet(decoder.ctx,nullptr)<0)return {};
            break;
        }
        if(packet.p->stream_index==stream_index){
            const int sent=avcodec_send_packet(decoder.ctx,packet.p);
            av_packet_unref(packet.p);
            if(sent<0)return {};
        }
        else{
            av_packet_unref(packet.p);
        }
        if(drain_decoder()<0)return {};
    }
    if(drain_decoder()<0)return {};
    if(!append(nullptr))return {};
    return pcm;
#endif
    return {};
}
}
