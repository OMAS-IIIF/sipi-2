#include "TiffStripGrayFixture.h"
#include "imgformats/TiffReader.h"
#include "imgformats/TiffReadPlan.h"
#include "image/AreaResize.h"
#include "image/AlphaComposite.h"
#include "storage/LocalFileSource.h"
#include <iostream>

namespace {
void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f, const char* context) {
    try { f(); } catch(const sipi::TiffReadError&) { return; }
    throw std::runtime_error(std::string("Expected TIFF failure: ")+context);
}
void verify(const sipi::RasterImage8& image,const sipi::PixelRegion& region,const fixture::StripGraySpec& spec) {
    const auto channels=spec.gray ? 1u : (spec.alpha ? 4u : 3u);
    check(image.channel_count()==channels && image.pixels.size()==region.width*region.height*channels,"Decoded layout");
    check(image.icc_profile==(spec.gray ? fixture::gray_profile() : fixture::icc_header()),"Profile preservation");
    for(unsigned y=0;y<region.height;++y) for(unsigned x=0;x<region.width;++x) {
        const auto value=fixture::gray_pixel(x+region.x,y+region.y);
        const auto at=(y*region.width+x)*channels;
        if(spec.gray) check(image.pixels[at]==value,"Gray polarity/crop");
        else {
            const unsigned a=spec.alpha ? ((x+region.x+y+region.y)%3)*127 : 255;
            for(unsigned c=0;c<3;++c) {
                unsigned expected=(value+c*31)%256;
                if(spec.associated) expected=a ? (((expected*a+127)/255)*255+a/2)/a : 0;
                check(image.pixels[at+c]==expected,"Strip RGB/alpha crop");
            }
            if(spec.alpha) check(image.pixels[at+3]==a,"Strip alpha");
        }
    }
}
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    const std::filesystem::path root{argv[1]}; std::filesystem::create_directories(root);
    const auto path=root/"strip-gray.tif";
    try {
        for (const auto compression : {COMPRESSION_NONE,COMPRESSION_LZW,COMPRESSION_ADOBE_DEFLATE,COMPRESSION_PACKBITS})
            for (const auto rows : {1u,5u,UINT32_MAX})
                for (const bool gray : {false,true}) for (const bool variant : {false,true}) {
                    fixture::StripGraySpec spec{.gray=gray,.white_zero=gray && variant,.alpha=!gray && variant,
                        .associated=!gray && variant,.rows=rows,.compression=static_cast<std::uint16_t>(compression)};
                    fixture::strip_gray_tiff(path,spec,false,rows==1 ? "w8b" : "w");
                    sipi::LocalFileSource source{path};
                    for (const sipi::PixelRegion crop : {sipi::PixelRegion{0,0,37,23},{0,0,1,1},{36,22,1,1},{3,4,19,8},{5,20,27,3}})
                        verify(sipi::read_tiff_region(source,crop),crop,spec);
                }
        for(bool white : {false,true}) {
            fixture::StripGraySpec spec{.white_zero=white,.tiled=true};
            fixture::strip_gray_tiff(path,spec,true);
            sipi::LocalFileSource source{path};
            const auto metadata=sipi::read_tiff_metadata(source);
            const auto plan=sipi::plan_tiff_read(metadata,{{3,1,31,21},15,10});
            check(plan.directory.width==19,"Gray pyramid selection");
            auto image=sipi::read_tiff_region(source,plan.directory,plan.read_region);
            verify(image,plan.read_region,spec);
            auto reduced=sipi::downsample_area(image,{plan.horizontal,plan.vertical},15,10);
            check(reduced.layout==sipi::PixelLayout::gray && reduced.pixels.size()==150,"Gray resizing stays one channel");
            auto pass=sipi::composite_on_white(reduced);
            check(pass.pixels==reduced.pixels && pass.layout==sipi::PixelLayout::gray,"Gray bypasses composition");
        }
        for(auto compression : {COMPRESSION_ADOBE_DEFLATE,COMPRESSION_LZW}) {
            fixture::strip_gray_tiff(path,{.broken_last=true,.compression=static_cast<std::uint16_t>(compression)});
            sipi::LocalFileSource source{path};
            (void)sipi::read_tiff_region(source,{1,1,1,1});
            rejects([&]{ (void)sipi::read_tiff_region(source,{0,20,37,3}); }, "broken compressed strip");
        }
        fixture::strip_gray_tiff(path);
        {
            sipi::LocalFileSource source{path};
            (void)sipi::read_tiff_region(source,{36,22,1,1},1+37*3); // Partial last strip, not a full five rows.
            rejects([&]{ (void)sipi::read_tiff_region(source,{36,22,1,1},37*3); }, "last strip budget");
            (void)sipi::read_tiff_region(source,{3,4,1,2},2+37*5);
            rejects([&]{ (void)sipi::read_tiff_region(source,{3,4,1,2},1+37*5); }, "crossing budget");
        }
        // Gray and RGB use the same averaging weights, but gray keeps one channel.
        sipi::RasterImage8 gray{5,3,{},fixture::gray_profile(),sipi::PixelLayout::gray};
        sipi::RasterImage8 rgb{5,3,{},{}};
        for(unsigned i=0;i<15;++i) { gray.pixels.push_back(i*17); for(unsigned c=0;c<3;++c) rgb.pixels.push_back(i*17); }
        for(unsigned w=1;w<=5;++w) for(unsigned h=1;h<=3;++h) {
            const auto a=sipi::downsample_area(gray,w,h), b=sipi::downsample_area(rgb,w,h);
            for(std::size_t i=0;i<a.pixels.size();++i) check(a.pixels[i]==b.pixels[i*3],"Gray area weights");
        }
        std::filesystem::remove(path);
        std::cout<<"Strip and gray layouts, polarity, compression, crops, pyramids and budgets passed\n";
    } catch(const std::exception& e) { std::filesystem::remove(path); std::cerr<<e.what()<<'\n'; return 1; }
}
