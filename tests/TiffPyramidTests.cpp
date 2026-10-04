#include "TiffPyramidFixture.h"
#include "imgformats/TiffReadPlan.h"
#include "imgformats/TiffReader.h"
#include "image/AreaResize.h"
#include "storage/LocalFileSource.h"
#include <algorithm>
#include <iostream>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    try { action(); } catch (const sipi::TiffReadError&) { return; }
    throw std::runtime_error("Expected TIFF read failure");
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root{argv[1]};
    std::filesystem::create_directories(root);
    const auto path = root / "planner-pyramid.tif";
    try {
        for (bool subifds : {false, true}) {
            fixture::pyramid(path, subifds, true);
            sipi::LocalFileSource source{path};
            const auto metadata = sipi::read_tiff_metadata(source);
            check(metadata.levels.size() == 2, "Pyramid discovery");
            const auto plan = sipi::plan_tiff_read(metadata, {{3, 5, 65, 49}, 15, 12});
            check(plan.directory.width == 26, "Selected wrong level");
            auto image = sipi::read_tiff_region(source, plan.directory, plan.read_region);
            check(image.icc_profile == metadata.icc_profile && image.icc_profile == fixture::icc_header(),
                  "Main ICC must survive reduced IFD without profile");
            for (std::uint32_t y = 0; y < image.height; ++y)
                for (std::uint32_t x = 0; x < image.width; ++x) {
                    const auto expected = fixture::pyramid_pixel(x+plan.read_region.x, y+plan.read_region.y, 2);
                    for (unsigned c = 0; c < 3; ++c)
                        check(image.pixels[(y*image.width+x)*3+c] == expected[c], "Wrong IFD/crop pixels");
                }
            const auto scaled = sipi::downsample_area(std::move(image),
                {plan.horizontal, plan.vertical}, 15, 12);
            check(scaled.width == 15 && scaled.height == 12 && scaled.icc_profile == metadata.icc_profile,
                  "Fractional delivery pipeline");
            // Independent common-grid reference maps the ORIGINAL full-image
            // crop directly into the level, bypassing read-plan window values.
            for (unsigned y = 0; y < 12; ++y)
                for (unsigned x = 0; x < 15; ++x) {
                    unsigned red = 0, green = 0;
                    for (unsigned i = 0; i < 65*26; ++i)
                        red += fixture::pyramid_pixel((3*26*15 + x*65*26 + i)/(101*15), 0, 2)[0];
                    for (unsigned j = 0; j < 49*20; ++j)
                        green += fixture::pyramid_pixel(0, (5*20*12 + y*49*20 + j)/(77*12), 2)[1];
                    const auto index = (y*15+x)*3;
                    check(scaled.pixels[index] == (2*red + 65*26)/(2*65*26) &&
                          scaled.pixels[index+1] == (2*green + 49*20)/(2*49*20) &&
                          scaled.pixels[index+2] == 100, "Full-image crop changed during pyramid rendering");
                }
            // Corrupt base pixels prove reduced requests never decode that level.
            rejects([&] { (void)sipi::read_tiff_region(source, {0,0,101,77}); });
            auto wrong = plan.directory;
            ++wrong.width;
            rejects([&] { (void)sipi::read_tiff_region(source, wrong, plan.read_region); });
            wrong.offset = 0;
            rejects([&] { (void)sipi::read_tiff_region(source, wrong, plan.read_region); });
            const auto bytes = plan.read_region.width * plan.read_region.height * 3 + 16*16*3;
            (void)sipi::read_tiff_region(source, plan.directory, plan.read_region, bytes);
            rejects([&] { (void)sipi::read_tiff_region(source, plan.directory, plan.read_region, bytes-1); });
        }
        fixture::pyramid(path, false, false, true);
        {
            sipi::LocalFileSource source{path};
            auto metadata = sipi::read_tiff_metadata(source);
            std::erase_if(metadata.levels, [](const auto& level) { return !sipi::supports_tiff_region(level); });
            const auto plan = sipi::plan_tiff_read(metadata, {{0,0,101,77}, 26,20});
            check(plan.directory.width == 51, "Skip unsupported smaller layout");
            (void)sipi::read_tiff_region(source, plan.directory, plan.read_region);
        }
        fixture::pyramid(path, true, false, false, true);
        {
            sipi::LocalFileSource source{path};
            const auto metadata = sipi::read_tiff_metadata(source);
            const auto& level = metadata.levels.back();
            (void)sipi::read_tiff_region(source, level, {0,0,16,16});
            rejects([&] { (void)sipi::read_tiff_region(source, level, {0,0,26,20}); });
        }
        std::filesystem::remove(path);
        std::cout << "Pyramid IFD selection, fractional windows, ICC, tile isolation and budgets passed\n";
    } catch (const std::exception& error) {
        std::filesystem::remove(path);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
