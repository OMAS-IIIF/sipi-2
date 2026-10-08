#include "imgformats/WebpWriter.h"
#include "image/SrgbProfiles.h"
#include <webp/decode.h>
#include <webp/demux.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::vector<std::uint8_t> encode(const sipi::RasterImage8& image, bool lossless = true) {
    std::vector<std::uint8_t> result;
    sipi::write_webp(image,[&](auto bytes) {
        check(!bytes.empty() && bytes.size() <= 16384,"Sink block size");
        result.insert(result.end(),bytes.begin(),bytes.end());
    },lossless);
    return result;
}
std::vector<std::uint8_t> decode(const std::vector<std::uint8_t>& data, int width, int height) {
    int w=0,h=0;
    const std::unique_ptr<std::uint8_t,decltype(&WebPFree)> pixels(
        WebPDecodeRGBA(data.data(),data.size(),&w,&h),WebPFree);
    check(pixels && w==width && h==height,"Decoded dimensions");
    return {pixels.get(),pixels.get()+std::size_t(w)*h*4};
}
std::vector<std::uint8_t> profile(const std::vector<std::uint8_t>& data) {
    const WebPData bytes{data.data(),data.size()};
    const std::unique_ptr<WebPDemuxer,decltype(&WebPDemuxDelete)> mux(WebPDemux(&bytes),WebPDemuxDelete);
    check(bool(mux),"Invalid mux");
    WebPChunkIterator chunk{};
    if (!WebPDemuxGetChunk(mux.get(),"ICCP",1,&chunk)) return {};
    std::vector<std::uint8_t> result(chunk.chunk.bytes,chunk.chunk.bytes+chunk.chunk.size);
    WebPDemuxReleaseChunkIterator(&chunk);
    return result;
}
template<class F> void rejects(F action) {
    try { action(); } catch (const sipi::WebpWriteError&) { return; }
    throw std::runtime_error("Expected WebP error");
}
}
int main(int argc,char** argv) {
    try {
        // HTTP integration reuses the independent decoder, never the encoder.
        if (argc==5) {
            std::ifstream file(argv[1],std::ios::binary);
            const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(file),{}};
            const auto pixels=decode(data,std::stoi(argv[2]),std::stoi(argv[3]));
            if (std::string_view(argv[4])=="binary")
                for (std::size_t i=0;i<pixels.size();i+=4)
                    check((pixels[i]==0 || pixels[i]==255) && pixels[i]==pixels[i+1] &&
                          pixels[i]==pixels[i+2] && pixels[i+3]==255,"Bitonal pixels");
            return 0;
        }
        using namespace sipi;
        RasterImage8 rgba{3,1,{255,0,0,0, 0,255,0,128, 0,0,255,255},{},PixelLayout::rgba};
        check(decode(encode(rgba),3,1)==rgba.pixels,"Lossless transparent pixels");
        const auto lossy=decode(encode(rgba,false),3,1);
        for (unsigned i=3;i<12;i+=4) check(lossy[i]==rgba.pixels[i],"Lossless alpha in lossy output");
        check(profile(encode(rgba)).empty(),"Untagged RGB unexpectedly tagged");
        rgba.icc_profile=srgb_profile(PixelLayout::rgb,2);
        check(profile(encode(rgba))==rgba.icc_profile,"RGB ICC changed");
        rgba.icc_profile.clear(); rgba.srgb_intent=3;
        check(profile(encode(rgba))==srgb_profile(PixelLayout::rgb,3),"Explicit sRGB missing");
        RasterImage8 gray{4,1,{0,64,128,255},{},PixelLayout::gray};
        auto encoded=encode(gray);
        auto pixels=decode(encoded,4,1);
        for (unsigned x=0;x<4;++x) {
            const double linear=std::pow(gray.pixels[x]/255.0,2.2);
            const double expected=255*(linear<=0.0031308 ? 12.92*linear : 1.055*std::pow(linear,1/2.4)-0.055);
            check(std::abs(pixels[x*4]-expected)<2 && pixels[x*4]==pixels[x*4+1] &&
                  pixels[x*4]==pixels[x*4+2],"Gray Gamma2.2 to sRGB");
        }
        check(profile(encoded)==srgb_profile(PixelLayout::rgb,1),"Gray has wrong output profile");
        gray.srgb_intent=0;
        pixels=decode(encode(gray),4,1);
        for (unsigned x=0;x<4;++x) check(std::abs(int(pixels[x*4])-gray.pixels[x])<=1,"Gray sRGB curve changed");
        gray.srgb_intent.reset(); gray.icc_profile=srgb_profile(PixelLayout::gray,0);
        check(decode(encode(gray),4,1)==pixels,"Explicit and ICC gray disagree");
        RasterImage8 ga{2,1,{64,0,128,123},{},PixelLayout::gray_alpha};
        pixels=decode(encode(ga),2,1);
        check(pixels[3]==0 && pixels[7]==123,"Gray alpha changed");
        ga.srgb_intent=0;
        pixels=decode(encode(ga),2,1);
        check(pixels[3]==0 && pixels[7]==123 && std::abs(int(pixels[4])-128)<=1,
              "Explicit gray-alpha sRGB conversion");
        rejects([&]{ auto bad=gray; bad.icc_profile[16]='R'; (void)encode(bad); });
        rejects([&]{ auto bad=gray; bad.icc_profile.resize(132); (void)encode(bad); });
        rejects([&]{ auto bad=rgba; bad.width=16384; (void)encode(bad); });
        rejects([&]{ auto bad=rgba; bad.pixels.pop_back(); (void)encode(bad); });
        rejects([&]{ write_webp(rgba,{},false); });
        rejects([&]{ write_webp(rgba,[](auto){},false,101); });
        bool propagated=false;
        try { write_webp(rgba,[](auto){throw 42;},true); } catch (int value) { propagated=value==42; }
        check(propagated,"Sink exception lost");
        check(encode(gray)==encode(gray),"Nondeterministic profile/encoding");
        std::cout << "WebP color, alpha, profiles, limits and errors passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
