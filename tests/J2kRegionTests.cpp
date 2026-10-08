#include "imgformats/J2kReadPlan.h"
#include "storage/LocalFileSource.h"
#include "image/AreaResize.h"
#include "image/Watermark.h"
#include <filesystem>
#include <iostream>
#include <future>
#include <vector>
#include <stdexcept>

void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    try {
        require(argc==2,"fixture directory required");
        for (const auto* name : {"source.jp2","source.j2k","offset.jp2","profile.jp2","metadata-box.jp2"}) {

            sipi::LocalFileSource source(std::filesystem::path(argv[1])/name);
            const auto m = sipi::read_j2k_metadata(source);
            require(sipi::supports_j2k_pixels(m),"fixture layout unsupported");
            for (const auto region : {sipi::PixelRegion{0,0,37,23}, {0,0,1,1}, {3,5,29,13}, {36,22,1,1}}) {
                const auto plan = sipi::plan_j2k_read(m,{region,region.width,region.height});
                auto pixels = sipi::read_j2k_region(source,m,plan);
                pixels = sipi::downsample_area(std::move(pixels),plan.window,region.width,region.height);
                const auto channels = pixels.channel_count();
                for (unsigned y=0;y<region.height;++y)
                    for (unsigned x=0;x<region.width;++x)
                        for (unsigned c=0;c<channels;++c)
                            require(pixels.pixels[(y*region.width+x)*channels+c] ==
                                ((x+region.x)*7+(y+region.y)*11+c*31)%256,"lossless crop pixels");
                require(pixels.icc_profile == m.icc_profile,"ICC preservation");
            }
            // Compare a selectively reconstructed crop with the same samples
            // from a complete reconstruction at that wavelet resolution.
            for (const auto region : {sipi::PixelRegion{1,1,35,21}, {3,5,29,13}, {0,0,7,9}, {30,16,7,7}}) {
                const auto plan = sipi::plan_j2k_read(m,{region,3,3});
                const std::uint64_t step = 1ULL << plan.reduction;
                const auto ceil_div = [](auto n, auto d) { return n/d+(n%d!=0); };
                const auto w = static_cast<unsigned>(ceil_div(m.origin_x+m.width,step)-ceil_div(m.origin_x,step));
                const auto h = static_cast<unsigned>(ceil_div(m.origin_y+m.height,step)-ceil_div(m.origin_y,step));
                const auto full_plan = sipi::plan_j2k_read(m,{{0,0,m.width,m.height},w,h});
                require(full_plan.reduction == plan.reduction,"reference resolution");
                require(plan.level_width == w && plan.level_height == h,"planned full level dimensions");
                auto full = sipi::read_j2k_region(source,m,full_plan);
                auto crop = sipi::read_j2k_region(source,m,plan);
                for (unsigned y=0;y<crop.height;++y)
                    for (unsigned x=0;x<crop.width;++x)
                        for (unsigned c=0;c<crop.channel_count();++c)
                            require(crop.pixels[(y*crop.width+x)*crop.channel_count()+c] ==
                                full.pixels[((y+plan.reduced_region.y)*w+x+plan.reduced_region.x)*full.channel_count()+c],
                                "selective reduced decode differs from full resolution reference");
                // The same globally anchored mask must survive independent
                // Grok window decoding and the final fractional resampling.
                sipi::RasterImage8 mask{3,2,{0,255,80,255,20,160},{},sipi::PixelLayout::gray};
                const auto mark = sipi::prepare_watermark(std::move(mask),m.width,m.height,0.65);
                const std::vector<std::uint8_t> color(full.channel_count(),255);
                full = sipi::apply_watermark(std::move(full),mark,color,0.6,
                    sipi::WatermarkGrid{w,h});
                crop = sipi::apply_watermark(std::move(crop),mark,color,0.6,
                    sipi::WatermarkGrid{plan.level_width,plan.level_height,
                        plan.reduced_region.x,plan.reduced_region.y});
                sipi::PixelWindow exact{
                    {std::uint64_t{region.x}*w,std::uint64_t{region.width}*w,m.width},
                    {std::uint64_t{region.y}*h,std::uint64_t{region.height}*h,m.height}};
                full = sipi::downsample_area(std::move(full),exact,3,3);
                crop = sipi::downsample_area(std::move(crop),plan.window,3,3);
                require(full.pixels == crop.pixels,"fractional field of view");
            }
            for (auto dims : {std::pair{19u,12u}, {10u,6u}, {1u,1u}}) {
                const auto plan = sipi::plan_j2k_read(m,{{0,0,37,23},dims.first,dims.second});

                auto pixels = sipi::read_j2k_region(source,m,plan);
                pixels = sipi::downsample_area(std::move(pixels),plan.window,dims.first,dims.second);
                require(pixels.width == dims.first && pixels.height == dims.second,"reduced dimensions");
            }
        }
        for (const auto* name : {"rgb-unknown.j2k","sixteen.jp2","signed.jp2","ycc.jp2"}) {
            sipi::LocalFileSource source(std::filesystem::path(argv[1])/name);
            const auto m = sipi::read_j2k_metadata(source);
            require(!sipi::supports_j2k_pixels(m),"unsupported metadata layout");
            bool caught = false;
            try { (void)sipi::read_j2k_region(source,m,sipi::plan_j2k_read(m,{{0,0,37,23},37,23})); }
            catch (const sipi::J2kUnsupportedError&) { caught=true; }
            require(caught,"unsupported pixels explicitly rejected");
        }
        sipi::LocalFileSource source(std::filesystem::path(argv[1])/"corrupt-tile.jp2");
        const auto m = sipi::read_j2k_metadata(source);
        auto plan = sipi::plan_j2k_read(m,{{0,0,5,5},5,5});
        require(sipi::read_j2k_region(source,m,plan).width==5,"unused corrupt tile is not decoded");
        bool caught = false;
        try { (void)sipi::read_j2k_region(source,m,plan,1); }
        catch (const sipi::J2kReadError&) { caught=true; }
        require(caught,"memory budget checked before decoding");
        caught=false;
        try { (void)sipi::read_j2k_region(source,m,sipi::plan_j2k_read(m,{{0,0,37,23},37,23})); }
        catch (const sipi::J2kReadError&) { caught=true; }
        require(caught,"requested corrupt tile fails");
        sipi::LocalFileSource valid(std::filesystem::path(argv[1])/"source.jp2");
        const auto valid_metadata = sipi::read_j2k_metadata(valid);
        std::vector<std::future<bool>> jobs;
        for (unsigned i=0;i<8;++i) jobs.push_back(std::async(std::launch::async,[&,i] {
            try {
                auto pixels = sipi::read_j2k_region(i%2 ? source : valid, valid_metadata,
                    sipi::plan_j2k_read(valid_metadata,{{0,0,37,23},37,23}));
                return i%2 == 0 && pixels.width == 37;
            } catch (const sipi::J2kReadError&) { return i%2 == 1; }
        }));
        for (auto& job : jobs) require(job.get(),"concurrent codec diagnostics stay request-local");

    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
