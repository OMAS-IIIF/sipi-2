/** Offline fixture preparation, deliberately separate from the delivery server.
 * Read one small/medium top-left uint8/uint16 strip TIFF; convert tagged colors
 * at source precision to RGB8 sRGB or Gray8 Gamma 2.2 D50. Generate an identical
 * full-resolution reference, LZW TIFF pyramid and lossless Grok JP2.
 * Reject nonopaque 8-bit alpha rather than silently choosing a background.
 * Own all codec handles; single-threaded, bounded to 300 million source pixels.
 */
#include <grok.h>
#include <lcms2.h>
#include <tiffio.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
using Tiff = std::unique_ptr<TIFF, decltype(&TIFFClose)>;
struct Profile {
    cmsHPROFILE handle;
    explicit Profile(cmsHPROFILE p) : handle(p) { if (!p) throw std::runtime_error("Invalid ICC profile"); }
    ~Profile() { cmsCloseProfile(handle); }
    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
};
struct Transform {
    cmsHTRANSFORM handle;
    explicit Transform(cmsHTRANSFORM p) : handle(p) { if (!p) throw std::runtime_error("Cannot build ICC transform"); }
    ~Transform() { cmsDeleteTransform(handle); }
    Transform(const Transform&) = delete;
    Transform& operator=(const Transform&) = delete;
};
struct Raster { unsigned w, h, channels; Bytes pixels, profile; };
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void save(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(bool(out), "Cannot write output");
}
/** Return an owned ICC handle; the caller wraps it in Profile immediately. */
cmsHPROFILE target_profile(bool gray) {
    if (!gray) return cmsCreate_sRGBProfile();
    const auto curve = cmsBuildGamma(nullptr, 2.2);
    require(curve != nullptr, "Cannot allocate gamma curve");
    const auto profile = cmsCreateGrayProfile(cmsD50_xyY(), curve);
    cmsFreeToneCurve(curve);
    return profile;
}
/** Serialize a deterministic JP2-compatible target profile. */
Bytes profile_bytes(cmsHPROFILE profile) {
    cmsSetProfileVersion(profile, 2.1); // JP2-compatible matrix/gray ICC profile.
    cmsSetHeaderRenderingIntent(profile, INTENT_RELATIVE_COLORIMETRIC);
    cmsUInt32Number size = 0;
    require(cmsSaveProfileToMem(profile, nullptr, &size), "Cannot size ICC");
    Bytes bytes(size);
    require(cmsSaveProfileToMem(profile, bytes.data(), &size), "Cannot serialize ICC");
    require(bytes.size() >= 128, "Short ICC");
    // Stable artifact bytes: LCMS creates profiles with the current wall time.
    constexpr std::array<std::uint8_t,12> date{7,234,0,1,0,1,0,0,0,0,0,0};
    std::copy(date.begin(), date.end(), bytes.begin()+24);
    std::fill(bytes.begin()+84, bytes.begin()+100, 0);
    return bytes;
}
/** Normalize source precision/colour; export source/target ICC for the audit. */
Raster normalize(const std::filesystem::path& source, const std::filesystem::path& output) {
    Tiff file(TIFFOpen(source.c_str(), "r"), TIFFClose);
    require(bool(file), "Cannot open TIFF");
    std::uint32_t w=0,h=0; std::uint16_t bits=0,samples=0,photo=0,planar=0,orientation=0,format=0;
    require(TIFFGetField(file.get(),TIFFTAG_IMAGEWIDTH,&w) && TIFFGetField(file.get(),TIFFTAG_IMAGELENGTH,&h), "Missing size");
    TIFFGetFieldDefaulted(file.get(),TIFFTAG_BITSPERSAMPLE,&bits);
    TIFFGetFieldDefaulted(file.get(),TIFFTAG_SAMPLESPERPIXEL,&samples);
    TIFFGetFieldDefaulted(file.get(),TIFFTAG_PHOTOMETRIC,&photo);
    TIFFGetFieldDefaulted(file.get(),TIFFTAG_PLANARCONFIG,&planar);
    TIFFGetFieldDefaulted(file.get(),TIFFTAG_ORIENTATION,&orientation);
    TIFFGetFieldDefaulted(file.get(),TIFFTAG_SAMPLEFORMAT,&format);
    require(w && h && std::uint64_t(w)*h<=300000000, "Source exceeds 300 MP preparation limit");
    require(!TIFFIsTiled(file.get()) && TIFFNumberOfDirectories(file.get())==1 &&
        planar==PLANARCONFIG_CONTIG && orientation==ORIENTATION_TOPLEFT && format==SAMPLEFORMAT_UINT &&
        (bits==8 || bits==16), "Expected one top-left uint8/uint16 strip TIFF");
    const bool gray=photo==PHOTOMETRIC_MINISBLACK;
    const unsigned channels=gray ? 1 : 3;
    require((gray || photo==PHOTOMETRIC_RGB) && (samples==channels || samples==channels+1), "Unsupported channels");
    std::uint16_t extras=0; std::uint16_t* extra=nullptr;
    TIFFGetFieldDefaulted(file.get(),TIFFTAG_EXTRASAMPLES,&extras,&extra);
    const bool alpha=samples==channels+1;
    require(!alpha || (extras==1 && (extra[0]==EXTRASAMPLE_ASSOCALPHA || extra[0]==EXTRASAMPLE_UNASSALPHA)), "Undefined alpha");
    Profile target(target_profile(gray));
    std::uint32_t icc_size=0; void* icc=nullptr;
    const bool tagged=TIFFGetField(file.get(),TIFFTAG_ICCPROFILE,&icc_size,&icc);
    require(!tagged || icc_size<=16*1024*1024, "ICC too large");
    Profile input(tagged ? cmsOpenProfileFromMem(icc,icc_size) : target_profile(gray));
    require(cmsGetColorSpace(input.handle)==(gray ? cmsSigGrayData : cmsSigRgbData), "ICC/channel mismatch");
    if (tagged) save(output/"source.icc",Bytes(static_cast<std::uint8_t*>(icc),static_cast<std::uint8_t*>(icc)+icc_size));
    const auto in_type=gray ? (bits==16 ? TYPE_GRAY_16 : TYPE_GRAY_8) : (bits==16 ? TYPE_RGB_16 : TYPE_RGB_8);
    Transform transform(cmsCreateTransform(input.handle,in_type,target.handle,gray ? TYPE_GRAY_8 : TYPE_RGB_8,
        INTENT_RELATIVE_COLORIMETRIC,cmsFLAGS_BLACKPOINTCOMPENSATION));
    Raster raster{w,h,channels,Bytes(std::size_t(w)*h*channels),profile_bytes(target.handle)};
    require(TIFFScanlineSize64(file.get())==std::uint64_t(w)*samples*(bits/8), "Unexpected TIFF scanline layout");
    std::vector<std::uint16_t> row16(std::size_t(w)*samples),color16(std::size_t(w)*channels);
    Bytes row8(std::size_t(w)*samples),color8(std::size_t(w)*channels);
    unsigned alpha_min=bits==16 ? 65535 : 255,alpha_max=0;
    for (unsigned y=0;y<h;++y) {
        void* row=bits==16 ? static_cast<void*>(row16.data()) : row8.data();
        require(TIFFReadScanline(file.get(),row,y,0)>=0,"Cannot decode scanline");
        const unsigned max=bits==16 ? 65535 : 255;
        for (unsigned x=0;x<w;++x) {
            const auto base=std::size_t(x)*samples;
            const unsigned a=alpha ? (bits==16 ? row16[base+channels] : row8[base+channels]) : max;
            alpha_min=std::min(alpha_min,a);alpha_max=std::max(alpha_max,a);
            require((std::uint64_t(a)*255+max/2)/max==255,"Nonopaque 8-bit alpha needs an explicit preparation policy");
            for (unsigned c=0;c<channels;++c) {
                unsigned value=bits==16 ? row16[base+c] : row8[base+c];
                // TIFF associated alpha must be undone before the ICC transform.
                if (alpha && extra[0]==EXTRASAMPLE_ASSOCALPHA)
                    value=static_cast<unsigned>(std::min<std::uint64_t>(max,(std::uint64_t(value)*max+a/2)/a));
                if (bits==16) color16[std::size_t(x)*channels+c]=static_cast<std::uint16_t>(value);
                else color8[std::size_t(x)*channels+c]=static_cast<std::uint8_t>(value);
            }
        }
        auto* destination=raster.pixels.data()+std::size_t(y)*w*channels;
        // Untagged 8-bit inputs already have the declared benchmark encoding.
        if (!tagged && bits==8) std::copy(color8.begin(),color8.end(),destination);
        else cmsDoTransform(transform.handle,bits==16 ? static_cast<void*>(color16.data()) : color8.data(),destination,w);
    }
    save(output/"target.icc",raster.profile);
    std::cout << w << ' ' << h << ' ' << channels << ' ' << bits << ' ' << samples << ' '
              << alpha_min << ' ' << alpha_max << ' ' << tagged << '\n';
    return raster;
}
/** Ceil-sized 2x2 mean; odd edges average only the existing samples, half-up. */
Raster reduce(const Raster& input) {
    Raster out{(input.w+1)/2,(input.h+1)/2,input.channels,{},input.profile};
    out.pixels.resize(std::size_t(out.w)*out.h*out.channels);
    for (unsigned y=0;y<out.h;++y) for(unsigned x=0;x<out.w;++x) for(unsigned c=0;c<out.channels;++c) {
        unsigned sum=0,n=0;
        for(unsigned dy=0;dy<2;++dy) for(unsigned dx=0;dx<2;++dx)
            if(2*x+dx<input.w && 2*y+dy<input.h) {sum+=input.pixels[(std::size_t(2*y+dy)*input.w+2*x+dx)*out.channels+c];++n;}
        out.pixels[(std::size_t(y)*out.w+x)*out.channels+c]=static_cast<std::uint8_t>((sum+n/2)/n);
    }
    return out;
}
/** Write a reference or main-IFD pyramid; edge tiles are zero-padded outside the image. */
void write_tiff(const std::filesystem::path& path, Raster image, bool pyramid) {
    Tiff file(TIFFOpen(path.c_str(),"w"),TIFFClose); require(bool(file),"Cannot create TIFF");
    unsigned level=0;
    for (;;) {
        require(TIFFSetField(file.get(),TIFFTAG_IMAGEWIDTH,image.w) && TIFFSetField(file.get(),TIFFTAG_IMAGELENGTH,image.h) &&
            TIFFSetField(file.get(),TIFFTAG_SAMPLESPERPIXEL,image.channels) && TIFFSetField(file.get(),TIFFTAG_BITSPERSAMPLE,8) &&
            TIFFSetField(file.get(),TIFFTAG_PHOTOMETRIC,image.channels==1 ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB) &&
            TIFFSetField(file.get(),TIFFTAG_PLANARCONFIG,PLANARCONFIG_CONTIG) && TIFFSetField(file.get(),TIFFTAG_ORIENTATION,ORIENTATION_TOPLEFT) &&
            TIFFSetField(file.get(),TIFFTAG_COMPRESSION,pyramid ? COMPRESSION_LZW : COMPRESSION_NONE) &&
            TIFFSetField(file.get(),TIFFTAG_SUBFILETYPE,level ? FILETYPE_REDUCEDIMAGE : 0) &&
            TIFFSetField(file.get(),TIFFTAG_ICCPROFILE,static_cast<std::uint32_t>(image.profile.size()),image.profile.data()),"Cannot set TIFF tags");
        if (pyramid) {
            require(TIFFSetField(file.get(),TIFFTAG_PREDICTOR,PREDICTOR_HORIZONTAL) &&
                TIFFSetField(file.get(),TIFFTAG_TILEWIDTH,256) && TIFFSetField(file.get(),TIFFTAG_TILELENGTH,256),"Cannot set tiles");
            Bytes tile(256*256*image.channels);
            for(unsigned y=0;y<image.h;y+=256) for(unsigned x=0;x<image.w;x+=256) {
                std::fill(tile.begin(),tile.end(),0);
                for(unsigned dy=0;dy<std::min(256u,image.h-y);++dy)
                    std::copy_n(image.pixels.data()+(std::size_t(y+dy)*image.w+x)*image.channels,
                        std::min(256u,image.w-x)*image.channels,tile.data()+std::size_t(dy)*256*image.channels);
                require(TIFFWriteEncodedTile(file.get(),TIFFComputeTile(file.get(),x,y,0,0),tile.data(),static_cast<tmsize_t>(tile.size()))>=0,"Cannot encode tile");
            }
        } else {
            require(TIFFSetField(file.get(),TIFFTAG_ROWSPERSTRIP,32),"Cannot set strips");
            for(unsigned y=0;y<image.h;++y)
                require(TIFFWriteScanline(file.get(),image.pixels.data()+std::size_t(y)*image.w*image.channels,y,0)>=0,"Cannot write scanline");
        }
        require(TIFFWriteDirectory(file.get()),"Cannot finish TIFF level");
        if (!pyramid || std::max(image.w,image.h)<=256) break;
        image=reduce(image); ++level;
    }
}
/** Encode the same full-resolution pixels, including ICC, with reversible RGB MCT. */
void write_jp2(const std::filesystem::path& path,const Raster& raster) {
    std::vector<grk_image_comp> components(raster.channels);
    for(auto& c:components) {c.w=raster.w;c.h=raster.h;c.prec=8;c.dx=c.dy=1;}
    const auto destroy=[](grk_image* p){if(p)grk_object_unref(&p->obj);};
    std::unique_ptr<grk_image,decltype(destroy)> image(grk_image_new(static_cast<std::uint16_t>(raster.channels),components.data(),
        raster.channels==1 ? GRK_CLRSPC_GRAY : GRK_CLRSPC_SRGB,true),destroy);
    require(bool(image),"Cannot allocate JP2"); image->x1=raster.w;image->y1=raster.h;
    for(unsigned c=0;c<raster.channels;++c) for(unsigned y=0;y<raster.h;++y) for(unsigned x=0;x<raster.w;++x)
        static_cast<std::int32_t*>(image->comps[c].data)[std::size_t(y)*image->comps[c].stride+x]=raster.pixels[(std::size_t(y)*raster.w+x)*raster.channels+c];
    image->meta=grk_image_meta_new();require(image->meta!=nullptr,"Cannot allocate JP2 ICC metadata");
    image->meta->color.icc_profile_len=static_cast<std::uint32_t>(raster.profile.size());
    image->meta->color.icc_profile_buf=new std::uint8_t[raster.profile.size()];
    std::copy(raster.profile.begin(),raster.profile.end(),image->meta->color.icc_profile_buf);
    Bytes bytes(raster.pixels.size()*2+1024*1024);
    grk_stream_params stream{};stream.buf=bytes.data();stream.buf_len=bytes.size();
    grk_cparameters params{};grk_compress_set_default_params(&params);
    params.cod_format=GRK_FMT_JP2;params.numresolution=6;params.irreversible=false;
    params.mct=raster.channels==3 ? 1 : 0; // reversible RGB decorrelation, no color loss
    params.tile_size_on=true;params.t_width=params.t_height=1024;
    const std::unique_ptr<grk_object,decltype(&grk_object_unref)> codec(grk_compress_init(&stream,&params,image.get()),grk_object_unref);
    require(bool(codec),"Cannot initialize JP2 encoder");
    const auto count=grk_compress(codec.get(),nullptr);require(count && count<=bytes.size(),"Cannot encode JP2");
    bytes.resize(count);save(path,bytes);
}
}
int main(int argc,char** argv) {
    if(argc!=3){std::cerr<<"Usage: prepare-archive-image SOURCE_TIFF NEW_DIRECTORY\n";return 2;}
    grk_initialize(nullptr,1,nullptr);
    try {
        const std::filesystem::path out(argv[2]);
        require(std::filesystem::create_directory(out),"Output directory must be new");
        const auto raster=normalize(argv[1],out);
        write_tiff(out/"reference.tif",raster,false);
        write_tiff(out/"pyramid.tif",raster,true);
        write_jp2(out/"image.jp2",raster);
        std::cout<<"libtiff="<<TIFFLIB_VERSION<<" lcms="<<cmsGetEncodedCMMversion()<<" grok="<<grk_version()<<'\n';
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';grk_deinitialize();return 1;}
    grk_deinitialize();
}
