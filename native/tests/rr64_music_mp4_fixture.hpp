#pragma once
// Generate our own one-second sine-wave AAC/MP4 fixture using the Windows encoder.
inline bool write_mp4_fixture(const std::filesystem::path& path) {
    using Microsoft::WRL::ComPtr;
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(com))return false;
    const auto mf=MFStartup(MF_VERSION);
    struct Cleanup {HRESULT mf;~Cleanup(){if(SUCCEEDED(mf))MFShutdown();CoUninitialize();}} cleanup{mf};
    if(FAILED(mf))return false;
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFMediaType> encoded,raw;
    DWORD stream=0;
    if(FAILED(MFCreateSinkWriterFromURL(path.c_str(),nullptr,nullptr,&writer)) ||
        FAILED(MFCreateMediaType(&encoded)) || FAILED(MFCreateMediaType(&raw)))return false;
    encoded->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio);
    encoded->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_AAC);
    encoded->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2);
    encoded->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000);
    encoded->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16);
    encoded->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,16000);
    encoded->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE,0);
    encoded->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION,0x29);
    raw->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio);
    raw->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM);
    raw->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2);
    raw->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000);
    raw->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16);
    raw->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,4);
    raw->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,192000);
    if(FAILED(writer->AddStream(encoded.Get(),&stream)) ||
        FAILED(writer->SetInputMediaType(stream,raw.Get(),nullptr)) || FAILED(writer->BeginWriting()))return false;
    for(int chunk=0;chunk<50;++chunk){
        ComPtr<IMFSample> sample;ComPtr<IMFMediaBuffer> buffer;
        if(FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(960*4,&buffer)))return false;
        BYTE* bytes=nullptr;
        if(FAILED(buffer->Lock(&bytes,nullptr,nullptr)))return false;
        auto* data=reinterpret_cast<std::int16_t*>(bytes);
        for(int i=0;i<960;++i){auto value=std::int16_t(10000*std::sin((chunk*960+i)*6.283185307179586*440/48000));data[i*2]=value;data[i*2+1]=-value;}
        buffer->Unlock();buffer->SetCurrentLength(960*4);sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(chunk*200000LL);sample->SetSampleDuration(200000);
        if(FAILED(writer->WriteSample(stream,sample.Get())))return false;
    }
    return SUCCEEDED(writer->Finalize());
}
