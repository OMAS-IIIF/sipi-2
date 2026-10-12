/** Offline baseline for the unchanged production resampler. No HTTP service,
 * cache mutation or repository writes. Run a Release build on a quiet machine.
 * Synthetic input copies and output hashes are outside timed regions. Codec
 * cases reuse metadata and an opened source, as with warm metadata-cache reads.
 */
#include "image/AreaResize.h"
#include "imgformats/TiffReader.h"
#include "imgformats/TiffReadPlan.h"
#include "imgformats/J2kReader.h"
#include "imgformats/J2kReadPlan.h"
#include "imgformats/JpegWriter.h"
#include "storage/LocalFileSource.h"
#include <json/json.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>

namespace {
using namespace sipi;
using Clock = std::chrono::steady_clock;
constexpr int warmups = 2;
constexpr std::size_t budget = 512 * 1024 * 1024;

double milliseconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b-a).count();
}

/** Noncryptographic output fingerprint; validates repeated work, not image quality. */
std::uint64_t fingerprint(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t value = 14695981039346656037ULL;
    for (auto byte : bytes) { value ^= byte; value *= 1099511628211ULL; }
    return value;
}

Json::Value summary(const std::vector<double>& samples) {
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    Json::Value result;
    result["min_ms"] = sorted.front();
    result["median_ms"] = sorted[sorted.size()/2];
    result["max_ms"] = sorted.back();
    for (double time : samples) result["samples_ms"].append(time);
    return result;
}

PixelWindow full_window(unsigned w, unsigned h) { return {{0,w,1},{0,h,1}}; }

/** Deterministic varied channel values, including zero and partial alpha. */
RasterImage8 pattern(unsigned w, unsigned h, PixelLayout layout) {
    RasterImage8 result{w,h,{}, {},layout};
    result.pixels.resize(static_cast<std::size_t>(w)*h*result.channel_count());
    for (std::size_t i=0;i<result.pixels.size();++i)
        result.pixels[i] = static_cast<std::uint8_t>((i*73+i/11*29)%256);
    return result;
}

/** Retain raw samples and verify every timed result against the warmup output. */
Json::Value synthetic(const char* name, PixelLayout layout, unsigned sw, unsigned sh,
                      unsigned tw, unsigned th, bool fractional, int repetitions) {
    const auto input = pattern(sw,sh,layout);
    const auto window = fractional
        ? PixelWindow{{1,sw*3-2,3},{1,sh*3-2,3}} : full_window(sw,sh);
    std::vector<double> times;
    std::uint64_t expected = 0;
    for (int i=-warmups;i<repetitions;++i) {
        auto copy = input; // Copy excluded: production transfers ownership.
        const auto start = Clock::now();
        const auto output = resize_image(std::move(copy),window,tw,th,budget);
        const auto end = Clock::now();
        const auto hash = fingerprint(output.pixels);
        if (i==-warmups) expected=hash;
        if (hash!=expected || output.width!=tw || output.height!=th || output.layout!=layout)
            throw std::runtime_error("Unstable synthetic output");
        if (i>=0) times.push_back(milliseconds(start,end));
    }
    Json::Value result;
    result["kind"]="synthetic"; result["case"]=name;
    result["channels"]=static_cast<unsigned>(input.channel_count());
    result["source_width"]=sw; result["source_height"]=sh;
    result["output_width"]=tw; result["output_height"]=th;
    result["fractional_window"]=fractional;
    result["resize"]=summary(times);
    result["fingerprint"]=std::to_string(expected);
    return result;
}

/** Measure the warm-source render core, using production read plans and kernels.
 * Includes decoder allocation/I/O, resampler allocation/input destruction, and
 * JPEG encoding/buffering. Excludes HTTP, Lua, metadata lookup, color conversion,
 * watermark, rotation, output-cache reads/writes and file-open time.
 */
Json::Value pipeline(const std::filesystem::path& path, const char* name,
                     bool crop, unsigned target, int repetitions) {
    LocalFileSource source(path);
    const bool jp2 = (path.extension()==".jp2" || path.extension()==".jph");
    unsigned w, h, level_w, level_h;
    PixelWindow window;
    std::function<RasterImage8()> decode;
    const auto header_start = Clock::now();
    const auto tiff = jp2 ? TiffMetadata{} : read_tiff_metadata(source);
    const auto j2k = jp2 ? read_j2k_metadata(source) : J2kMetadata{};
    const auto header_end = Clock::now();
    w = jp2 ? j2k.width : tiff.image.width;
    h = jp2 ? j2k.height : tiff.image.height;
    PixelRegion region{0,0,w,h};
    if (crop) region={w/3,h/3,std::min(w-w/3,1023u),std::min(h-h/3,1023u)};
    const unsigned th = std::max(1u,static_cast<unsigned>(
        static_cast<std::uint64_t>(region.height)*target/region.width));
    const iiif::ImageGeometry geometry{region,target,th};
    if (jp2) {
        const auto plan=plan_j2k_read(j2k,geometry);
        window=plan.window; level_w=plan.level_width; level_h=plan.level_height;
        decode=[&,plan] {return read_j2k_region(source,j2k,plan,budget);};
    } else {
        const auto plan=plan_tiff_read(tiff,geometry);
        window={plan.horizontal,plan.vertical};
        level_w=plan.directory.width; level_h=plan.directory.height;
        decode=[&,plan] {return read_tiff_region(source,plan.directory,plan.read_region,budget);};
    }
    std::vector<double> decoding, resizing, encoding, totals;
    std::uint64_t expected=0, expected_jpeg=0;
    unsigned decoded_w=0, decoded_h=0, channels=0;
    std::size_t jpeg_bytes=0;
    for (int i=-warmups;i<repetitions;++i) {
        const auto start=Clock::now();
        auto pixels=decode();
        const auto decoded=Clock::now();
        decoded_w=pixels.width; decoded_h=pixels.height; channels=pixels.channel_count();
        pixels=resize_image(std::move(pixels),window,target,th,budget);
        const auto resized=Clock::now();
        const auto encode_start=resized;
        std::vector<std::uint8_t> bytes;
        write_jpeg(pixels,[&](auto block){bytes.insert(bytes.end(),block.begin(),block.end());});
        const auto end=Clock::now();
        // Verify only after all timed stages, avoiding a pre-encode cache warmup.
        const auto hash=fingerprint(pixels.pixels);
        jpeg_bytes=bytes.size();
        const auto jpeg_hash=fingerprint(bytes);
        if (i==-warmups) {expected=hash; expected_jpeg=jpeg_hash;}
        if (hash!=expected || jpeg_hash!=expected_jpeg || pixels.width!=target || pixels.height!=th)
            throw std::runtime_error("Unstable pipeline output");
        if (i>=0) {
            decoding.push_back(milliseconds(start,decoded));
            resizing.push_back(milliseconds(decoded,resized));
            encoding.push_back(milliseconds(encode_start,end));
            totals.push_back(decoding.back()+resizing.back()+encoding.back());
        }
    }
    Json::Value result;
    result["kind"]="pipeline"; result["case"]=name; result["file"]=path.string();
    result["source_width"]=w; result["source_height"]=h; result["channels"]=channels;
    result["level_width"]=level_w; result["level_height"]=level_h;
    result["decoded_width"]=decoded_w; result["decoded_height"]=decoded_h;
    result["output_width"]=target; result["output_height"]=th;
    result["header_once_ms"]=milliseconds(header_start,header_end);
    result["decode"]=summary(decoding); result["resize"]=summary(resizing);
    result["encode"]=summary(encoding); result["render_core"]=summary(totals);
    result["fingerprint"]=std::to_string(expected);
    result["jpeg_fingerprint"]=std::to_string(expected_jpeg);
    result["jpeg_bytes"]=Json::UInt64(jpeg_bytes);
    return result;
}
}

int main(int argc,char** argv) {
    try {
        if (argc<2) throw std::runtime_error("Usage: resize-benchmark ODD_REPETITIONS [TIFF_OR_JP2 ...]");
        const std::string count=argv[1]; std::size_t used=0;
        const int repetitions=std::stoi(count,&used);
        if (used!=count.size() || repetitions<3 || repetitions>101 || repetitions%2==0)
            throw std::runtime_error("Repetitions must be odd, in 3..101");
        Json::Value report;
        report["schema"]=1; report["compiler"]=__VERSION__;
#ifdef NDEBUG
        report["ndebug"]=true;
#else
        throw std::runtime_error("Use a Release build (NDEBUG required)");
#endif
        report["repetitions"]=repetitions; report["warmups"]=warmups;
        for (auto layout : {PixelLayout::gray,PixelLayout::rgb,PixelLayout::rgba}) {
            for (const auto& result : {
                synthetic("tile-half",layout,512,512,256,256,false,repetitions),
                synthetic("tile-fractional",layout,513,513,256,256,true,repetitions),
                synthetic("preview-noninteger",layout,2048,1536,1500,1125,false,repetitions),
                synthetic("thumbnail-eighth",layout,4096,3072,512,384,false,repetitions),
                synthetic("upscale-double",layout,512,384,1024,768,false,repetitions)})
                report["cases"].append(result);
        }
        for (int i=2;i<argc;++i) {
            std::cerr << "Measuring " << argv[i] << '\n';
            report["cases"].append(pipeline(argv[i],"thumbnail",false,256,repetitions));
            report["cases"].append(pipeline(argv[i],"preview",false,1500,repetitions));
            report["cases"].append(pipeline(argv[i],"tile",true,256,repetitions));
        }
        std::cout << report << '\n';
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
