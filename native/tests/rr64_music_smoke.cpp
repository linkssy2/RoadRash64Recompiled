#define RR64_MUSIC_HEADLESS
#include "../src/rr64_music.cpp"
#include <cassert>
#include <iostream>
#include "rr64_music_mp4_fixture.hpp"
#undef main
int main(int argc,char**argv){
    if(argc!=4)return 2;
    const auto flac=rr64::music::decode(argv[2]);
    if(!flac || flac->pcm.size()!=9600)return 10;
    for(std::size_t i=0;i<flac->pcm.size();i+=2)
        if(flac->pcm[i]!=10000 || flac->pcm[i+1]!=-10000)return 11;
    if(!write_mp4_fixture(argv[3]))return 12;
    auto mp4=rr64::music::decode(argv[3]);
    if(!mp4 || mp4->pcm.size()<90000 || mp4->pcm.size()>110000)return 13;
    double energy=0;for(auto s:mp4->pcm)energy+=double(s)*s;
    if(energy/mp4->pcm.size()<1000000)return 14;
    if(!rr64::music::decode_media(argv[2],10).empty())return 15;
    if(!rr64::music::decode_media("missing.mp4",48000).empty())return 16;
    for(auto ext:{".MP4",".flac",".mp3",".m4a",".aac",".wma",".wav",".ogg"})
        if(!rr64::music::supported_extension(ext))return 17;
    if(rr64::music::supported_extension(".exe"))return 18;
    auto highres=rr64::music::decode(std::filesystem::path(argv[2]).parent_path()/"r36-music-96k.flac");
    if(!highres || highres->pcm.size()<9400 || highres->pcm.size()>9800)return 19;
    for(std::size_t i=400;i+400<highres->pcm.size();i+=2)
        if(std::abs(int(highres->pcm[i])-10000)>2 || std::abs(int(highres->pcm[i+1])+10000)>2)return 20;
    auto t=rr64::music::decode(argv[1]);
    if(!t||t->pcm.size()!=9600)return 3;
    rr64::music::current.store(t);rr64::music::playing.store(true);rr64::music::volume.store(1.0f);
    std::vector<std::int16_t> samples(19200,30000);
    rr64::music::mix(samples,48000);
    if(samples[0]!=32767||samples[1]!=20000||samples[9600]!=32767)return 4;
    if(rr64_music_stock_volume(123)!=0||rr64_music_volume_update(0,1)!=1)return 5;
    rr64::music::playing.store(false);
    if(rr64_music_stock_volume(123)!=123||rr64_music_volume_update(0,1)!=1||rr64_music_volume_update(0,1)!=0)return 6;
    const auto before=samples;rr64::music::mix(samples,32000);if(samples!=before)return 7;
    rr64::music::volume.store(.5f);
    if(rr64_music_stock_volume(10000)!=5000||rr64_music_volume_update(0,1)!=1||rr64_music_volume_update(0,2)!=1||rr64_music_volume_update(0,1)!=0)return 21;
    rr64::music::volume.store(0.f);
    if(rr64_music_stock_volume(10000)!=0||rr64_music_volume_update(0,1)!=1||rr64_music_volume_update(0,2)!=1)return 22;
    rr64::music::volume.store(.5f);rr64::music::playing.store(true);rr64::music::restart.fetch_add(1);
    std::vector<std::int16_t> half(20,100);rr64::music::mix(half,48000);
    if(half[0]!=5100||half[1]!=-4900)return 23;
    rr64::music::volume.store(1.f);rr64::music::playing.store(false);
    if(rr64::music::decode("does-not-exist.wav"))return 8;
    rr64::music::playing.store(true);rr64::music::restart.fetch_add(1);
    samples.assign(6400,0);rr64::music::mix(samples,32000);
    for(std::size_t i=0;i<samples.size();i+=2)if(samples[i]!=10000||samples[i+1]!=-10000)return 9;
    std::cout<<"Music: WAV, 16-bit FLAC, 24-bit/96kHz FLAC conversion, generated AAC/MP4 decoding, limits, mixing, clipping, looping, 48/32kHz output and stock-volume restoration passed.\n";
}

