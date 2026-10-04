#include "image/AlphaComposite.h"
#include "image/AreaResize.h"
#include "imgformats/TiffReader.h"
#include "imgformats/TiffReadPlan.h"
#include "imgformats/JpegWriter.h"
#include "storage/LocalFileSource.h"
#include "TiffAlphaFixture.h"
#include <iostream>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Error, class F> void rejects(F action) {
    try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("Expected alpha error");
}
using sipi::PixelLayout;
using sipi::RasterImage8;

/** Independent common-grid replication oracle. Integer samples and products
 * represent exact covered areas; production uses overlap weights in double. */
void check_window(const RasterImage8& image, const sipi::PixelWindow& window,
                  unsigned w, unsigned h) {
    const auto out = sipi::downsample_area(image, window, w, h);
    const auto& ax = window.horizontal; const auto& ay = window.vertical;
    const auto count = ax.extent_numerator * ay.extent_numerator;
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            std::uint64_t alpha = 0;
            std::array<std::uint64_t,3> color{};
            for (std::uint64_t j = 0; j < ay.extent_numerator; ++j)
                for (std::uint64_t i = 0; i < ax.extent_numerator; ++i) {
                    const auto sx = (ax.offset_numerator*w+x*ax.extent_numerator+i)/(ax.denominator*w);
                    const auto sy = (ay.offset_numerator*h+y*ay.extent_numerator+j)/(ay.denominator*h);
                    const auto index = (sy*image.width+sx)*4;
                    const auto a = image.pixels[index+3];
                    alpha += a;
                    for (unsigned c=0;c<3;++c) color[c] += unsigned{image.pixels[index+c]}*a;
                }
            const auto index = (y*w+x)*4;
            const auto expected_alpha = (2*alpha+count)/(2*count);
            check(std::abs(int(out.pixels[index+3])-int(expected_alpha)) <= 1, "Alpha area oracle");
            for (unsigned c=0;c<3;++c) {
                const auto expected = expected_alpha ? (2*color[c]+alpha)/(2*alpha) : 0;
                check(std::abs(int(out.pixels[index+c])-int(expected)) <= 1, "Premultiplied color oracle");
            }
        }
    check(out.layout == PixelLayout::rgba && out.icc_profile == image.icc_profile, "RGBA layout/profile");
}
}
int main(int argc,char** argv) {
    if (argc!=2) return 2;
    const std::filesystem::path root{argv[1]}; std::filesystem::create_directories(root);
    const auto path = root/"alpha.tif";
    try {
        RasterImage8 edge{2,1,{255,0,0,255, 0,0,255,0},{},PixelLayout::rgba};
        const auto reduced = sipi::downsample_area(edge,1,1);
        check(reduced.pixels == std::vector<std::uint8_t>({255,0,0,128}), "Hidden blue must not bleed");
        auto white = sipi::composite_on_white(reduced);
        check(white.pixels == std::vector<std::uint8_t>({255,127,127}) && white.layout==PixelLayout::rgb, "White composition");
        RasterImage8 clear{2,1,{100,20,200,0, 0,30,10,0},{},PixelLayout::rgba};
        check(sipi::downsample_area(clear,1,1).pixels == std::vector<std::uint8_t>(4,0), "Zero coverage");
        check(sipi::composite_on_white(clear).pixels == std::vector<std::uint8_t>(6,255), "Transparent is white for JPEG");
        auto owned = edge; const auto* storage=owned.pixels.data();
        auto same=sipi::downsample_area(std::move(owned),2,1,8);
        check(same.pixels.data()==storage && same.pixels==edge.pixels, "RGBA identity ownership");
        auto compact=sipi::composite_on_white(std::move(same),8);
        check(compact.pixels.data()==storage, "In-place alpha compaction");
        rejects<sipi::ResizeError>([&]{ (void)sipi::downsample_area(edge,1,1,11); });
        (void)sipi::downsample_area(edge,1,1,12);
        rejects<sipi::CompositeError>([&]{ (void)sipi::composite_on_white(edge,7); });
        rejects<sipi::JpegWriteError>([&]{ sipi::write_jpeg(edge,[](auto){}); });
        for (const auto layout : {PixelLayout::rgba, static_cast<PixelLayout>(99)}) {
            RasterImage8 invalid{1,1,{1,2,3},{},layout};
            rejects<sipi::ResizeError>([&]{ (void)sipi::downsample_area(invalid,1,1); });
            rejects<sipi::CompositeError>([&]{ (void)sipi::composite_on_white(invalid); });
        }
        RasterImage8 pattern{5,4,{},fixture::icc_header(),PixelLayout::rgba};
        for(unsigned y=0;y<4;++y) for(unsigned x=0;x<5;++x) {
            const auto pixel=fixture::alpha_pixel(x,y); pattern.pixels.insert(pattern.pixels.end(),pixel.begin(),pixel.end());
        }
        for(unsigned w=1;w<=4;++w) for(unsigned h=1;h<=3;++h) {
            check_window(pattern,{{0,5,1},{0,4,1}},w,h);
            check_window(pattern,{{1,13,3},{1,10,3}},w,h);
        }
        for (int mode : {EXTRASAMPLE_ASSOCALPHA,EXTRASAMPLE_UNASSALPHA}) {
            fixture::alpha_tiff(path,mode,true,true);
            sipi::LocalFileSource source{path};
            const auto metadata=sipi::read_tiff_metadata(source);
            check(metadata.levels.size()==1 && sipi::supports_tiff_region(metadata.image), "RGBA pyramid metadata");
            const auto plan=sipi::plan_tiff_read(metadata,{{3,1,29,17},14,8});
            check(plan.directory.width==18, "RGBA pyramid selected");
            const auto crop=sipi::read_tiff_region(source,plan.directory,plan.read_region);
            check(crop.layout==PixelLayout::rgba && crop.icc_profile==fixture::icc_header(), "RGBA ICC inheritance");
            for(unsigned y=0;y<crop.height;++y) for(unsigned x=0;x<crop.width;++x) {
                auto expected=fixture::alpha_pixel(x+plan.read_region.x,y+plan.read_region.y);
                if(mode==EXTRASAMPLE_ASSOCALPHA) for(unsigned c=0;c<3;++c) {
                    const unsigned a=expected[3], premult=(unsigned{expected[c]}*a+127)/255;
                    expected[c]=a ? (premult*255+a/2)/a : 0;
                }
                for(unsigned c=0;c<4;++c)
                    check(crop.pixels[(y*crop.width+x)*4+c]==expected[c], "TIFF alpha decoding");
            }
            const auto scaled=sipi::downsample_area(crop,{plan.horizontal,plan.vertical},14,8);
            check(scaled.layout==PixelLayout::rgba, "RGBA fractional pipeline");
            rejects<sipi::TiffReadError>([&]{ (void)sipi::read_tiff_region(source,{0,0,35,19}); });
            const auto bytes=crop.pixels.size()+16*16*4;
            (void)sipi::read_tiff_region(source,plan.directory,plan.read_region,bytes);
            rejects<sipi::TiffReadError>([&]{ (void)sipi::read_tiff_region(source,plan.directory,plan.read_region,bytes-1); });
        }
        for(int mode:{-1,EXTRASAMPLE_UNSPECIFIED}) {
            fixture::alpha_tiff(path,mode);
            sipi::LocalFileSource source{path};
            check(!sipi::supports_tiff_region(sipi::read_tiff_metadata(source).image), "Undeclared extra sample is not alpha");
            rejects<sipi::TiffUnsupportedError>([&]{ (void)sipi::read_tiff_region(source,{0,0,1,1}); });
        }
        fixture::alpha_tiff(path,EXTRASAMPLE_ASSOCALPHA,false,false,true);
        {
            sipi::LocalFileSource source{path};
            rejects<sipi::TiffReadError>([&]{ (void)sipi::read_tiff_region(source,{0,0,1,1}); });
            (void)sipi::read_tiff_region(source,{1,0,1,1}); // Ignore corrupt pixel outside requested crop.
        }
        std::filesystem::remove(path);
        std::cout<<"Alpha decoding, filtering, composition, ownership and limits passed\n";
    } catch(const std::exception& e) { std::filesystem::remove(path); std::cerr<<e.what()<<'\n'; return 1; }
}
