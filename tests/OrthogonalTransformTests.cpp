#include "image/OrthogonalTransform.h"
#include <array>
#include <iostream>
#include <limits>
#include <utility>

namespace {
using namespace sipi;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
RasterImage8 pattern(std::uint32_t width, std::uint32_t height, PixelLayout layout) {
    RasterImage8 image{width,height,{}, {9,8,7},layout};
    for (std::size_t i=0; i<std::size_t{width}*height; ++i)
        for (std::size_t c=0; c<image.channel_count(); ++c)
            image.pixels.push_back(static_cast<std::uint8_t>(c==3 && i%2==0 ? 0 : i*10+c));
    return image;
}
template<class F> void rejects(F f) {
    bool failed = false;
    try { f(); } catch (const TransformError&) { failed=true; }
    check(failed,"invalid transform or budget accepted");
}
void run() {
    constexpr std::array<QuarterTurn,4> turns{QuarterTurn::none, QuarterTurn::clockwise90,
        QuarterTurn::half_turn, QuarterTurn::clockwise270};
    // Explicit non-square golden arrangements; rows are [0 1 2] [3 4 5].
    constexpr std::array<std::array<unsigned,6>,8> golden{{
        {{0,1,2,3,4,5}}, {{3,0,4,1,5,2}}, {{5,4,3,2,1,0}}, {{2,5,1,4,0,3}},
        {{2,1,0,5,4,3}}, {{5,2,4,1,3,0}}, {{3,4,5,0,1,2}}, {{0,3,1,4,2,5}}
    }};
    for (const auto layout : {PixelLayout::gray,PixelLayout::rgb,PixelLayout::rgba}) {
        const auto input = pattern(3,2,layout);
        for (unsigned mirror=0; mirror<2; ++mirror) {
            for (unsigned turn=0; turn<4; ++turn) {
                const auto result = transform_orthogonal(input, mirror!=0, turns[turn]);
                check(result.width==(turn%2 ? 2u : 3u) && result.height==(turn%2 ? 3u : 2u),"swapped dimensions");
                check(result.layout==layout && result.icc_profile==input.icc_profile,"layout or ICC changed");
                for (unsigned i=0; i<6; ++i)
                    for (std::size_t c=0; c<input.channel_count(); ++c)
                        check(result.pixels[i*input.channel_count()+c] ==
                            input.pixels[golden[mirror*4+turn][i]*input.channel_count()+c],"golden pixel permutation");
                if (turn%2==0) {
                    auto owned=input;
                    const auto* pixels=owned.pixels.data();
                    const auto* icc=owned.icc_profile.data();
                    auto in_place=transform_orthogonal(std::move(owned),mirror!=0,turns[turn]);
                    check(in_place.pixels.data()==pixels && in_place.icc_profile.data()==icc,"unnecessary copy");
                }
            }
        }
        // Degenerate axes, odd/even dimensions, reversibility and repeated rotations.
        for (std::uint32_t w=1; w<=5; ++w) for (std::uint32_t h=1; h<=5; ++h) {
            const auto original=pattern(w,h,layout);
            auto result=original;
            for (unsigned i=0; i<4; ++i) result=transform_orthogonal(std::move(result),false,QuarterTurn::clockwise90);
            check(result.pixels==original.pixels && result.width==w && result.height==h,"four turns identity");
            result=transform_orthogonal(original,true,QuarterTurn::none);
            result=transform_orthogonal(std::move(result),true,QuarterTurn::none);
            check(result.pixels==original.pixels,"double mirror identity");
            for (const auto turn : turns) {
                result=transform_orthogonal(original,true,turn);
                result=transform_orthogonal(std::move(result),true,turn);
                check(result.pixels==original.pixels && result.width==w && result.height==h,"reflection involution");
            }
        }
        const auto bytes=input.pixels.size(), profile=input.icc_profile.size();
        (void)transform_orthogonal(input,true,QuarterTurn::half_turn,bytes+profile);
        rejects([&]{ (void)transform_orthogonal(input,false,QuarterTurn::none,bytes+profile-1); });
        (void)transform_orthogonal(input,true,QuarterTurn::clockwise90,2*bytes+profile);
        rejects([&]{ (void)transform_orthogonal(input,true,QuarterTurn::clockwise270,2*bytes+profile-1); });
        rejects([&]{ (void)transform_orthogonal(input,false,static_cast<QuarterTurn>(99)); });
    }
    for (unsigned mode=0; mode<6; ++mode) {
        auto invalid=pattern(3,2,PixelLayout::rgb);
        switch (mode) {
        case 0: invalid.width=0; break;
        case 1: invalid.height=0; break;
        case 2: invalid.pixels.pop_back(); break;
        case 3: invalid.pixels.push_back(1); break;
        case 4: invalid.layout=static_cast<PixelLayout>(99); break;
        case 5: invalid.width=UINT32_MAX; invalid.height=UINT32_MAX; break;
        }
        rejects([&]{ (void)transform_orthogonal(invalid,false,QuarterTurn::none,std::numeric_limits<std::size_t>::max()); });
    }
}
}
int main() {
    try { run(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "Exact orthogonal transforms, ownership and memory checks passed\n";
}
