cmake_minimum_required(VERSION 3.24)

project(sipi-2 VERSION 1.0.0 LANGUAGES CXX)

find_package(Threads REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(Drogon CONFIG REQUIRED)
find_package(TIFF CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(PNG CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(lcms CONFIG REQUIRED)
find_package(OpenSSL CONFIG REQUIRED)
find_package(SQLite3 CONFIG REQUIRED)
find_package(grok CONFIG REQUIRED)
find_package(lua CONFIG REQUIRED)
find_package(jsoncpp CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(AWSSDK CONFIG REQUIRED COMPONENTS s3)

# Encoded-result identity only: no cache storage, HTTP, or codec dependency.
add_library(cachekey src/cache/CacheKey.h src/cache/CacheKey.cpp)
target_include_directories(cachekey PUBLIC src)
target_link_libraries(cachekey PRIVATE OpenSSL::Crypto)
target_compile_features(cachekey PUBLIC cxx_std_20)
set_target_properties(cachekey PROPERTIES CXX_EXTENSIONS OFF)

# Pool-shared, externally synchronized immutable header cache; no codec or database runtime dependency.
add_library(metadatacache src/cache/MetadataCache.h src/cache/MetadataCache.cpp)
target_include_directories(metadatacache PUBLIC src)
target_compile_features(metadatacache PUBLIC cxx_std_20)
set_target_properties(metadatacache PROPERTIES CXX_EXTENSIONS OFF)

# Pool-shared immutable coverage masks; no storage or codec dependency.
add_library(variantcache src/cache/VariantCache.cpp src/cache/VariantCache.h)
target_include_directories(variantcache PUBLIC src)
target_compile_features(variantcache PUBLIC cxx_std_20)
set_target_properties(variantcache PROPERTIES CXX_EXTENSIONS OFF)

add_library(maskcache src/cache/MaskCache.cpp src/cache/MaskCache.h)
target_include_directories(maskcache PUBLIC src)
target_compile_features(maskcache PUBLIC cxx_std_20)
set_target_properties(maskcache PROPERTIES CXX_EXTENSIONS OFF)

# Persistent encoded results; SQLite stays private to the implementation.
add_library(resultcache src/cache/ResultCache.h src/cache/ResultCache.cpp)
target_include_directories(resultcache PUBLIC src)
target_link_libraries(resultcache PUBLIC cachekey PRIVATE imagestorage SQLite::SQLite3)
target_compile_features(resultcache PUBLIC cxx_std_20)
set_target_properties(resultcache PROPERTIES CXX_EXTENSIONS OFF)

# Storage ownership is separate from path authorization and image codecs.
add_library(imagestorage src/storage/ByteSource.h src/storage/LocalFileSource.h src/storage/LocalFileSource.cpp)
target_include_directories(imagestorage PUBLIC src)
target_compile_features(imagestorage PUBLIC cxx_std_20)
set_target_properties(imagestorage PROPERTIES CXX_EXTENSIONS OFF)

# SDK types stay private to the remote storage implementation.
add_library(s3storage src/storage/S3ByteSource.h src/storage/S3ByteSource.cpp)
target_include_directories(s3storage PUBLIC src)
target_link_libraries(s3storage PRIVATE AWS::aws-sdk-cpp-s3 spdlog::spdlog)
target_compile_features(s3storage PUBLIC cxx_std_20)
set_target_properties(s3storage PROPERTIES CXX_EXTENSIONS OFF)

add_library(imagesource src/image/SourceResolver.cpp src/image/SourceResolver.h src/image/ImageSource.h)
target_include_directories(imagesource PUBLIC src)
target_compile_features(imagesource PUBLIC cxx_std_20)
set_target_properties(imagesource PROPERTIES CXX_EXTENSIONS OFF)

# Explicit script settings and startup secret snapshots; no Lua/HTTP dependency.
add_library(scriptconfig src/preflight/ScriptConfig.cpp src/preflight/ScriptConfig.h)
target_include_directories(scriptconfig PUBLIC src)
target_compile_features(scriptconfig PUBLIC cxx_std_20)
set_target_properties(scriptconfig PROPERTIES CXX_EXTENSIONS OFF)

# Lua policy evaluation with generic native helpers; independent of Drogon and codecs.
add_library(preflight src/preflight/LuaPreflight.cpp src/preflight/LuaPreflight.h
    src/preflight/LuaJson.cpp src/preflight/LuaJson.h
    src/preflight/HttpClient.cpp src/preflight/HttpClient.h
    src/preflight/LuaHttp.cpp src/preflight/LuaHttp.h
    src/preflight/Jwt.cpp src/preflight/Jwt.h
    src/preflight/LuaJwt.cpp src/preflight/LuaJwt.h
    src/preflight/SharedCache.cpp src/preflight/SharedCache.h
    src/preflight/LuaCache.cpp src/preflight/LuaCache.h)
target_include_directories(preflight PUBLIC src)
target_link_libraries(preflight PUBLIC scriptconfig PRIVATE Threads::Threads lua::lua JsonCpp::JsonCpp CURL::libcurl jwt-cpp::jwt-cpp)
target_compile_features(preflight PUBLIC cxx_std_20)
set_target_properties(preflight PROPERTIES CXX_EXTENSIONS OFF)

# Pure IIIF syntax parsing; no dependency on HTTP or image libraries.
add_library(iiifparser src/iiifparser/IIIFParser.cpp src/iiifparser/IIIFParser.h
    src/iiifparser/IIIFGeometry.cpp src/iiifparser/IIIFGeometry.h)
target_include_directories(iiifparser PUBLIC src)
target_compile_features(iiifparser PUBLIC cxx_std_20)
set_target_properties(iiifparser PROPERTIES CXX_EXTENSIONS OFF)

# Grok types stay private; all I/O uses the existing version-bound ByteSource.
add_library(j2kreader src/imgformats/J2kReadPlan.cpp src/imgformats/J2kReadPlan.h src/imgformats/J2kReader.h src/imgformats/J2kReader.cpp)
target_include_directories(j2kreader PUBLIC src)
target_link_libraries(j2kreader PRIVATE grok::grok)
target_compile_features(j2kreader PUBLIC cxx_std_20)
set_target_properties(j2kreader PROPERTIES CXX_EXTENSIONS OFF)

add_library(tiffreader src/imgformats/TiffReadPlan.cpp src/imgformats/TiffReadPlan.h
    src/imgformats/TiffReader.cpp src/imgformats/TiffReader.h src/imgformats/TiffMetadata.h
    src/image/PixelRegion.h src/image/RasterImage.h)
target_include_directories(tiffreader PUBLIC src)
target_link_libraries(tiffreader PRIVATE TIFF::TIFF)
target_compile_features(tiffreader PUBLIC cxx_std_20)
set_target_properties(tiffreader PROPERTIES CXX_EXTENSIONS OFF)

add_library(imageprocessing src/image/WatermarkColor.h src/image/WatermarkColor.cpp src/image/Watermark.h src/image/Watermark.cpp src/image/Bitonal.h src/image/Bitonal.cpp src/image/GrayConversion.h src/image/GrayConversion.cpp src/image/ImageTransform.h src/image/ImageTransform.cpp src/image/OrthogonalTransform.h src/image/OrthogonalTransform.cpp src/image/AlphaComposite.h src/image/AlphaComposite.cpp src/image/PixelWindow.h src/image/AreaResize.h src/image/AreaResize.cpp)
target_include_directories(imageprocessing PUBLIC src)
target_link_libraries(imageprocessing PRIVATE lcms::lcms colorprofiles)
target_compile_features(imageprocessing PUBLIC cxx_std_20)
set_target_properties(imageprocessing PROPERTIES CXX_EXTENSIONS OFF)

add_library(jpegreader src/imgformats/JpegReader.h src/imgformats/JpegReader.cpp)
target_include_directories(jpegreader PUBLIC src)
target_link_libraries(jpegreader PRIVATE libjpeg-turbo::jpeg-static)
target_compile_features(jpegreader PUBLIC cxx_std_20)
set_target_properties(jpegreader PROPERTIES CXX_EXTENSIONS OFF)

add_library(colorprofiles src/image/SrgbProfiles.h src/image/SrgbProfiles.cpp)
target_include_directories(colorprofiles PUBLIC src)
target_compile_features(colorprofiles PUBLIC cxx_std_20)
set_target_properties(colorprofiles PROPERTIES CXX_EXTENSIONS OFF)

add_library(webpwriter src/imgformats/WebpWriter.h src/imgformats/WebpWriter.cpp)
target_include_directories(webpwriter PUBLIC src)
target_link_libraries(webpwriter PRIVATE WebP::webp WebP::libwebpmux lcms::lcms colorprofiles)
target_compile_features(webpwriter PUBLIC cxx_std_20)
set_target_properties(webpwriter PROPERTIES CXX_EXTENSIONS OFF)

add_library(jpegwriter src/imgformats/JpegWriter.cpp src/imgformats/JpegWriter.h src/imgformats/JpegOptions.h)
target_include_directories(jpegwriter PUBLIC src)
target_link_libraries(jpegwriter PRIVATE libjpeg-turbo::jpeg-static colorprofiles)
target_compile_features(jpegwriter PUBLIC cxx_std_20)
set_target_properties(jpegwriter PROPERTIES CXX_EXTENSIONS OFF)

add_library(pngreader src/imgformats/PngReader.h src/imgformats/PngReader.cpp)
target_include_directories(pngreader PUBLIC src)
target_link_libraries(pngreader PRIVATE PNG::PNG)
target_compile_features(pngreader PUBLIC cxx_std_20)
set_target_properties(pngreader PROPERTIES CXX_EXTENSIONS OFF)

add_library(pngwriter src/imgformats/PngWriter.cpp src/imgformats/PngWriter.h)
target_include_directories(pngwriter PUBLIC src)
target_link_libraries(pngwriter PRIVATE PNG::PNG)
target_compile_features(pngwriter PUBLIC cxx_std_20)
set_target_properties(pngwriter PROPERTIES CXX_EXTENSIONS OFF)

add_executable(sipi-2 src/main.cpp src/CommandLine.h
        src/CommandLine.cpp src/HttpServer.h src/HttpServer.cpp
        src/http/IiifWorker.h src/http/IiifWorker.cpp src/http/HttpProtocol.cpp src/http/DeliveryPolicy.cpp src/http/DeliveryPolicy.h src/http/IiifInfo.h src/http/IiifInfo.cpp src/http/ImageUri.cpp)
target_link_libraries(sipi-2 PRIVATE variantcache maskcache metadatacache resultcache spdlog::spdlog CLI11::CLI11 Drogon::Drogon iiifparser preflight imagesource imagestorage s3storage tiffreader j2kreader jpegreader pngreader jpegwriter pngwriter webpwriter imageprocessing)
target_compile_definitions(sipi-2 PRIVATE SIPI_VERSION="${sipi-2_VERSION}")

# Require standard C++20 for this target, without compiler-specific extensions.
target_compile_features(sipi-2 PRIVATE cxx_std_20)
set_target_properties(sipi-2 PROPERTIES CXX_EXTENSIONS OFF)

include(CTest)
if(BUILD_TESTING)
    add_executable(variant-cache-tests tests/VariantCacheTests.cpp)
    target_link_libraries(variant-cache-tests PRIVATE variantcache)
    add_test(NAME variant-cache COMMAND variant-cache-tests)
    add_executable(mask-cache-tests tests/MaskCacheTests.cpp)
    target_link_libraries(mask-cache-tests PRIVATE maskcache imageprocessing)
    add_test(NAME mask-cache COMMAND mask-cache-tests)
    add_executable(metadata-cache-tests tests/MetadataCacheTests.cpp)
    target_link_libraries(metadata-cache-tests PRIVATE metadatacache)
    set_target_properties(metadata-cache-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME metadata-cache COMMAND metadata-cache-tests)
    add_executable(result-cache-tests tests/ResultCacheTests.cpp)
    target_link_libraries(result-cache-tests PRIVATE resultcache SQLite::SQLite3 Threads::Threads)
    add_test(NAME result-cache COMMAND result-cache-tests)
    add_executable(cache-key-tests tests/CacheKeyTests.cpp)
    target_link_libraries(cache-key-tests PRIVATE cachekey iiifparser Threads::Threads)
    set_target_properties(cache-key-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME cache-key COMMAND cache-key-tests)
    add_executable(jpeg-metadata-tests tests/JpegMetadataTests.cpp)
    target_link_libraries(jpeg-metadata-tests PRIVATE jpegreader metadatacache libjpeg-turbo::jpeg-static Threads::Threads)
    set_target_properties(jpeg-metadata-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME jpeg-metadata COMMAND jpeg-metadata-tests "${CMAKE_CURRENT_BINARY_DIR}/jpeg-test-data")
    set_tests_properties(jpeg-metadata PROPERTIES FIXTURES_SETUP jpeg_images TIMEOUT 30)
    add_executable(j2k-region-tests tests/J2kRegionTests.cpp)
    target_link_libraries(j2k-region-tests PRIVATE j2kreader imagestorage imageprocessing)
    set_target_properties(j2k-region-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME j2k-region COMMAND j2k-region-tests "${CMAKE_CURRENT_BINARY_DIR}/http-test-data")
    set_tests_properties(j2k-region PROPERTIES FIXTURES_REQUIRED j2k_images TIMEOUT 30)
    add_executable(j2k-fixtures tests/CreateJ2kFixtures.cpp)
    target_link_libraries(j2k-fixtures PRIVATE grok::grok)
    target_compile_features(j2k-fixtures PRIVATE cxx_std_20)
    set_target_properties(j2k-fixtures PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME j2k-fixtures COMMAND j2k-fixtures "${CMAKE_CURRENT_BINARY_DIR}/http-test-data")
    set_tests_properties(j2k-fixtures PROPERTIES FIXTURES_SETUP j2k_images)
    add_executable(j2k-metadata-tests tests/J2kMetadataTests.cpp)
    target_link_libraries(j2k-metadata-tests PRIVATE j2kreader imagestorage)
    set_target_properties(j2k-metadata-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME j2k-metadata COMMAND j2k-metadata-tests "${CMAKE_CURRENT_BINARY_DIR}/http-test-data")
    set_tests_properties(j2k-metadata PROPERTIES FIXTURES_REQUIRED j2k_images TIMEOUT 15)
    add_executable(tiff-strip-gray-tests tests/TiffStripGrayTests.cpp)
    target_link_libraries(tiff-strip-gray-tests PRIVATE tiffreader imagestorage imageprocessing TIFF::TIFF)
    set_target_properties(tiff-strip-gray-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME tiff-strip-gray COMMAND tiff-strip-gray-tests "${CMAKE_CURRENT_BINARY_DIR}/strip-gray-test-data")
    add_executable(image-uri-tests tests/ImageUriTests.cpp src/http/ImageUri.cpp)
    target_link_libraries(image-uri-tests PRIVATE iiifparser)
    set_target_properties(image-uri-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME image-uri COMMAND image-uri-tests)

    add_executable(http-protocol-tests tests/HttpProtocolTests.cpp src/http/HttpProtocol.cpp)
    target_link_libraries(http-protocol-tests PRIVATE iiifparser)
    set_target_properties(http-protocol-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME http-protocol COMMAND http-protocol-tests)
    add_executable(delivery-policy-tests tests/DeliveryPolicyTests.cpp src/http/DeliveryPolicy.cpp)
    target_link_libraries(delivery-policy-tests PRIVATE tiffreader j2kreader jpegwriter imageprocessing iiifparser)
    set_target_properties(delivery-policy-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME delivery-policy COMMAND delivery-policy-tests)
    add_executable(bitonal-tests tests/BitonalTests.cpp)
    target_link_libraries(bitonal-tests PRIVATE imageprocessing)
    add_test(NAME bitonal COMMAND bitonal-tests)
    add_executable(gray-conversion-tests tests/GrayConversionTests.cpp)
    target_link_libraries(gray-conversion-tests PRIVATE imageprocessing lcms::lcms Threads::Threads)
    set_target_properties(gray-conversion-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME gray-conversion COMMAND gray-conversion-tests "${CMAKE_CURRENT_BINARY_DIR}/gray-test-data")
    set_tests_properties(gray-conversion PROPERTIES FIXTURES_SETUP gray_profiles)
    add_executable(watermark-color-tests tests/WatermarkColorTests.cpp)
    target_link_libraries(watermark-color-tests PRIVATE imageprocessing colorprofiles lcms::lcms Threads::Threads)
    add_test(NAME watermark-color COMMAND watermark-color-tests)
    add_executable(watermark-grid-tests tests/WatermarkGridTests.cpp)
    target_link_libraries(watermark-grid-tests PRIVATE imageprocessing tiffreader j2kreader)
    add_test(NAME watermark-grid COMMAND watermark-grid-tests)
    add_executable(watermark-tests tests/WatermarkTests.cpp)
    target_link_libraries(watermark-tests PRIVATE imageprocessing)
    add_test(NAME watermark COMMAND watermark-tests)
    add_executable(image-transform-tests tests/ImageTransformTests.cpp)
    target_link_libraries(image-transform-tests PRIVATE imageprocessing)
    set_target_properties(image-transform-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME image-transform COMMAND image-transform-tests)
    add_executable(orthogonal-transform-tests tests/OrthogonalTransformTests.cpp)
    target_link_libraries(orthogonal-transform-tests PRIVATE imageprocessing)
    set_target_properties(orthogonal-transform-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME orthogonal-transform COMMAND orthogonal-transform-tests)
    add_executable(alpha-tests tests/AlphaTests.cpp)
    target_link_libraries(alpha-tests PRIVATE imageprocessing tiffreader imagestorage jpegwriter TIFF::TIFF)
    set_target_properties(alpha-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME alpha COMMAND alpha-tests "${CMAKE_CURRENT_BINARY_DIR}/alpha-test-data")
    add_executable(upscale-tests tests/UpscaleTests.cpp)
    target_link_libraries(upscale-tests PRIVATE imageprocessing)
    add_test(NAME upscale COMMAND upscale-tests)

    add_executable(resize-weights-tests tests/ResizeWeightsTests.cpp)
    target_link_libraries(resize-weights-tests PRIVATE imageprocessing)
    add_test(NAME resize-weights COMMAND resize-weights-tests)

    add_executable(area-resize-tests tests/AreaResizeTests.cpp)
    target_link_libraries(area-resize-tests PRIVATE imageprocessing)
    add_test(NAME area-resize COMMAND area-resize-tests)
    add_executable(jpeg-writer-tests tests/JpegWriterTests.cpp)
    target_link_libraries(jpeg-writer-tests PRIVATE jpegwriter libjpeg-turbo::jpeg-static)
    set_target_properties(jpeg-writer-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME jpeg-writer COMMAND jpeg-writer-tests)
    add_executable(source-resolver-tests tests/SourceResolverTests.cpp)
    target_link_libraries(source-resolver-tests PRIVATE imagesource)
    set_target_properties(source-resolver-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME source-resolution COMMAND source-resolver-tests "${CMAKE_CURRENT_BINARY_DIR}/source-test-data")
    add_executable(byte-source-tests tests/ByteSourceTests.cpp)
    target_link_libraries(byte-source-tests PRIVATE imagestorage tiffreader TIFF::TIFF Threads::Threads)
    set_target_properties(byte-source-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME byte-source COMMAND byte-source-tests "${CMAKE_CURRENT_BINARY_DIR}/byte-source-test-data")
    set_tests_properties(byte-source PROPERTIES TIMEOUT 15)
    add_executable(iiif-worker-tests tests/http/IiifWorkerTests.cpp src/http/IiifWorker.cpp src/http/HttpProtocol.cpp src/http/DeliveryPolicy.cpp src/http/IiifInfo.cpp src/http/ImageUri.cpp)
    target_link_libraries(iiif-worker-tests PRIVATE variantcache maskcache metadatacache resultcache preflight imagesource imagestorage s3storage tiffreader j2kreader jpegreader pngreader jpegwriter pngwriter webpwriter imageprocessing iiifparser Drogon::Drogon spdlog::spdlog Threads::Threads)
    set_target_properties(iiif-worker-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiif-worker COMMAND iiif-worker-tests
             "${CMAKE_CURRENT_BINARY_DIR}/worker-test-data" "${CMAKE_CURRENT_BINARY_DIR}/http-test-data")
    set_tests_properties(iiif-worker PROPERTIES TIMEOUT 30 FIXTURES_REQUIRED "http_images;j2k_images")
    add_executable(shared-cache-tests tests/preflight/SharedCacheTests.cpp)
    target_link_libraries(shared-cache-tests PRIVATE preflight Threads::Threads)
    add_test(NAME lua-shared-cache COMMAND shared-cache-tests "${CMAKE_CURRENT_BINARY_DIR}/shared-cache-test-data")
    set_tests_properties(lua-shared-cache PROPERTIES TIMEOUT 20)
    add_executable(preflight-tests tests/preflight/LuaPreflightTests.cpp)
    target_link_libraries(preflight-tests PRIVATE preflight Threads::Threads)
    set_target_properties(preflight-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME lua-preflight COMMAND preflight-tests
             "${CMAKE_CURRENT_BINARY_DIR}/preflight-test-data")
    set_tests_properties(lua-preflight PROPERTIES TIMEOUT 15)
    add_executable(tiff-region-tests tests/TiffRegionTests.cpp)
    target_link_libraries(tiff-region-tests PRIVATE tiffreader imagestorage iiifparser TIFF::TIFF)
    set_target_properties(tiff-region-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME tiff-region COMMAND tiff-region-tests "${CMAKE_CURRENT_BINARY_DIR}/tiff-region-data")
    add_executable(tiff-pyramid-tests tests/TiffPyramidTests.cpp)
    target_link_libraries(tiff-pyramid-tests PRIVATE tiffreader imagestorage imageprocessing TIFF::TIFF)
    set_target_properties(tiff-pyramid-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME tiff-pyramid COMMAND tiff-pyramid-tests "${CMAKE_CURRENT_BINARY_DIR}/pyramid-test-data")

    add_executable(tiff-read-plan-tests tests/TiffReadPlanTests.cpp)
    target_link_libraries(tiff-read-plan-tests PRIVATE tiffreader)
    set_target_properties(tiff-read-plan-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME tiff-read-plan COMMAND tiff-read-plan-tests)

    add_executable(tiff-metadata-tests tests/TiffMetadataTests.cpp src/CommandLine.cpp)
    target_link_libraries(tiff-metadata-tests PRIVATE tiffreader imagestorage iiifparser TIFF::TIFF CLI11::CLI11 scriptconfig)
    set_target_properties(tiff-metadata-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME tiff-metadata COMMAND tiff-metadata-tests "${CMAKE_CURRENT_BINARY_DIR}/tiff-test-data")
    add_executable(iiif-geometry-tests tests/IIIFGeometryTests.cpp)
    target_link_libraries(iiif-geometry-tests PRIVATE iiifparser)
    set_target_properties(iiif-geometry-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiif-geometry COMMAND iiif-geometry-tests)
    add_executable(iiif-parser-tests tests/IIIFParserTests.cpp)
    target_link_libraries(iiif-parser-tests PRIVATE iiifparser)
    set_target_properties(iiif-parser-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiif-syntax COMMAND iiif-parser-tests)
    add_executable(s3-configuration-tests tests/S3ConfigurationTests.cpp src/CommandLine.cpp)
    target_include_directories(s3-configuration-tests PRIVATE src)
    target_link_libraries(s3-configuration-tests PRIVATE CLI11::CLI11 scriptconfig)
    target_compile_features(s3-configuration-tests PRIVATE cxx_std_20)
    set_target_properties(s3-configuration-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME s3-configuration COMMAND s3-configuration-tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_executable(script-config-tests tests/preflight/ScriptConfigTests.cpp src/CommandLine.cpp)
    target_link_libraries(script-config-tests PRIVATE CLI11::CLI11 preflight Threads::Threads)
    set_target_properties(script-config-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME script-configuration COMMAND script-config-tests "${CMAKE_CURRENT_BINARY_DIR}/script-config-test-data")
    set_tests_properties(script-configuration PROPERTIES TIMEOUT 15)
    add_executable(command-line-tests tests/CommandLineTests.cpp src/CommandLine.cpp)
    target_include_directories(command-line-tests PRIVATE src)
    target_link_libraries(command-line-tests PRIVATE CLI11::CLI11 scriptconfig)
    target_compile_features(command-line-tests PRIVATE cxx_std_20)
    set_target_properties(command-line-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME startup-configuration
             COMMAND command-line-tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_executable(http-tiff-fixtures tests/CreateHttpTiff.cpp)
    target_link_libraries(http-tiff-fixtures PRIVATE TIFF::TIFF)
    target_compile_features(http-tiff-fixtures PRIVATE cxx_std_20)
    set_target_properties(http-tiff-fixtures PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME http-tiff-fixtures COMMAND http-tiff-fixtures "${CMAKE_CURRENT_BINARY_DIR}/http-test-data")
    set_tests_properties(http-tiff-fixtures PROPERTIES FIXTURES_SETUP http_images)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME release-workflow COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ReleaseWorkflowTests.py")
    set_tests_properties(release-workflow PROPERTIES TIMEOUT 15)
    add_test(NAME license-collection COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/LicenseCollectionTests.py")
    set_tests_properties(license-collection PROPERTIES TIMEOUT 15)
    add_test(NAME source-package COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/SourcePackageTests.py")
    set_tests_properties(source-package PROPERTIES TIMEOUT 15)
    add_test(NAME script-config-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/preflight/script_config_http.py" $<TARGET_FILE:sipi-2>)
    set_tests_properties(script-config-http PROPERTIES TIMEOUT 30)
    add_executable(jwt-tests tests/preflight/JwtTests.cpp)
    target_link_libraries(jwt-tests PRIVATE preflight JsonCpp::JsonCpp Threads::Threads)
    set_target_properties(jwt-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME lua-jwt COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/preflight/jwt_check.py" $<TARGET_FILE:jwt-tests>)
    set_tests_properties(lua-jwt PROPERTIES TIMEOUT 60)
    add_executable(lua-http-tests tests/preflight/LuaHttpTests.cpp)
    target_link_libraries(lua-http-tests PRIVATE preflight Threads::Threads)
    set_target_properties(lua-http-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME lua-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/preflight/lua_http.py" $<TARGET_FILE:lua-http-tests>)
    set_tests_properties(lua-http PROPERTIES TIMEOUT 60)
    add_test(NAME cache-startup COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cache_startup.py" $<TARGET_FILE:sipi-2>)
    set_tests_properties(cache-startup PROPERTIES TIMEOUT 60)
    add_executable(webp-writer-tests tests/WebpWriterTests.cpp)
    target_link_libraries(webp-writer-tests PRIVATE webpwriter colorprofiles WebP::webp WebP::webpdemux)
    add_test(NAME webp-writer COMMAND webp-writer-tests)

    add_executable(png-writer-tests tests/PngWriterTests.cpp)
    target_link_libraries(png-writer-tests PRIVATE pngwriter)
    set_target_properties(png-writer-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME png-writer COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/png_check.py" $<TARGET_FILE:png-writer-tests>)
    add_executable(s3-protocol-tests tests/S3ProtocolTests.cpp)
    target_link_libraries(s3-protocol-tests PRIVATE s3storage spdlog::spdlog)
    set_target_properties(s3-protocol-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME s3-protocol COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/s3_protocol.py" $<TARGET_FILE:s3-protocol-tests>)
    set_tests_properties(s3-protocol PROPERTIES TIMEOUT 45)
    add_test(NAME s3-io-analysis COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/s3_io_analysis.py")
    add_executable(png-mask-tests tests/PngMaskTests.cpp)
    target_link_libraries(png-mask-tests PRIVATE pngreader imageprocessing imagestorage Threads::Threads)
    add_test(NAME png-mask COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/png_mask.py" $<TARGET_FILE:png-mask-tests>)
    set_tests_properties(png-mask PROPERTIES TIMEOUT 30)
    add_executable(png-metadata-tests tests/PngMetadataTests.cpp)
    target_link_libraries(png-metadata-tests PRIVATE pngreader metadatacache Threads::Threads)
    set_target_properties(png-metadata-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME png-metadata COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/png_metadata.py" $<TARGET_FILE:png-metadata-tests>
        "${CMAKE_CURRENT_BINARY_DIR}/png-input-test-data")
    set_tests_properties(png-metadata PROPERTIES TIMEOUT 30 FIXTURES_SETUP png_inputs)
    add_test(NAME iiif-http-protocol COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/iiif_http_protocol.py" $<TARGET_FILE:sipi-2>)
    set_tests_properties(iiif-http-protocol PROPERTIES TIMEOUT 60)
    add_test(NAME webp-http COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/webp_http.py" $<TARGET_FILE:sipi-2> $<TARGET_FILE:webp-writer-tests>)
    set_tests_properties(webp-http PROPERTIES TIMEOUT 60)
    add_test(NAME oldap-preflight-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/oldap_preflight_http.py" $<TARGET_FILE:sipi-2>)
    set_tests_properties(oldap-preflight-http PROPERTIES TIMEOUT 60)
    add_test(NAME restricted-resolution-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/restricted_resolution_http.py" $<TARGET_FILE:sipi-2>
        "${CMAKE_CURRENT_BINARY_DIR}/http-test-data" "${CMAKE_CURRENT_BINARY_DIR}/jpeg-test-data")
    set_tests_properties(restricted-resolution-http PROPERTIES TIMEOUT 90 FIXTURES_REQUIRED "http_images;j2k_images;jpeg_images")
    add_test(NAME watermark-preflight-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/watermark_preflight_http.py" $<TARGET_FILE:sipi-2>
        "${CMAKE_CURRENT_BINARY_DIR}/http-test-data")
    set_tests_properties(watermark-preflight-http PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED "http_images;j2k_images")
    add_test(NAME preflight-context-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/preflight_context_http.py" $<TARGET_FILE:sipi-2>)
    set_tests_properties(preflight-context-http PROPERTIES TIMEOUT 60)
    add_test(NAME upscale-http COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/upscale_http.py" $<TARGET_FILE:sipi-2>)
    set_tests_properties(upscale-http PROPERTIES TIMEOUT 60)
    add_test(NAME output-limits-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/output_limits_http.py" $<TARGET_FILE:sipi-2>)
    set_tests_properties(output-limits-http PROPERTIES TIMEOUT 60)
    add_test(NAME dither-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/dither_http.py" $<TARGET_FILE:sipi-2>)
    set_tests_properties(dither-http PROPERTIES TIMEOUT 60)
    add_test(NAME gray-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gray_http.py" $<TARGET_FILE:sipi-2>
        "${CMAKE_CURRENT_BINARY_DIR}/png-input-test-data" "${CMAKE_CURRENT_BINARY_DIR}/gray-test-data")
    set_tests_properties(gray-http PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED "png_inputs;gray_profiles")
    add_test(NAME png-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/png_http.py" $<TARGET_FILE:sipi-2>
        "${CMAKE_CURRENT_BINARY_DIR}/png-input-test-data")
    set_tests_properties(png-http PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED png_inputs)
    add_test(NAME jpeg-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/jpeg_http.py" $<TARGET_FILE:sipi-2>
        "${CMAKE_CURRENT_BINARY_DIR}/jpeg-test-data" "${CMAKE_CURRENT_BINARY_DIR}/http-test-data")
    set_tests_properties(jpeg-http PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED "http_images;jpeg_images")
    add_test(NAME parallel-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/parallel_http.py"
        "$<TARGET_FILE:sipi-2>" "${CMAKE_CURRENT_BINARY_DIR}/http-test-data")
    set_tests_properties(parallel-http PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED "http_images;j2k_images")
    add_test(NAME rotation-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/rotation_http.py" $<TARGET_FILE:sipi-2>
        "${CMAKE_CURRENT_BINARY_DIR}/http-test-data")
    set_tests_properties(rotation-http PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED "http_images;j2k_images")
    add_test(NAME metadata-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/metadata_http.py" $<TARGET_FILE:sipi-2>
        "${CMAKE_CURRENT_BINARY_DIR}/http-test-data")
    set_tests_properties(metadata-http PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED "http_images;j2k_images")
    add_test(NAME jpeg-options-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/jpeg_options_http.py" $<TARGET_FILE:sipi-2>
        "${CMAKE_CURRENT_BINARY_DIR}/http-test-data")
    set_tests_properties(jpeg-options-http PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED http_images)
    add_test(NAME cache-http COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cache_http.py" $<TARGET_FILE:sipi-2>
        "${CMAKE_CURRENT_BINARY_DIR}/http-test-data")
    set_tests_properties(cache-http PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED "http_images;j2k_images")
    add_test(NAME http-routing
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/http_smoke.py"
                     $<TARGET_FILE:sipi-2> "${CMAKE_CURRENT_BINARY_DIR}/http-test-data" $<TARGET_FILE:jpeg-writer-tests>)
    set_tests_properties(http-routing PROPERTIES TIMEOUT 30 FIXTURES_REQUIRED "http_images;j2k_images")
endif()

# Explicit opt-in: this test writes disposable buckets to our local MinIO only.
option(SIPI_TEST_S3 "Enable local MinIO integration tests (requires dev/minio)" OFF)
if(BUILD_TESTING AND SIPI_TEST_S3)
    add_executable(s3-byte-source-tests tests/S3ByteSourceTests.cpp)
    target_link_libraries(s3-byte-source-tests PRIVATE s3storage imagestorage tiffreader AWS::aws-sdk-cpp-s3 Threads::Threads)
    set_target_properties(s3-byte-source-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME s3-byte-source COMMAND s3-byte-source-tests "${CMAKE_CURRENT_BINARY_DIR}/http-test-data/tiled.tif")
    add_test(NAME s3-http COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/s3_http.py"
             $<TARGET_FILE:sipi-2> "${CMAKE_CURRENT_BINARY_DIR}/http-test-data" "${CMAKE_CURRENT_BINARY_DIR}/jpeg-test-data" "${CMAKE_CURRENT_BINARY_DIR}/png-input-test-data")
    set_tests_properties(s3-http PROPERTIES TIMEOUT 120 FIXTURES_REQUIRED "http_images;j2k_images;jpeg_images;png_inputs" LABELS integration)
    set_tests_properties(s3-byte-source PROPERTIES TIMEOUT 120 FIXTURES_REQUIRED "http_images;j2k_images" LABELS integration)
endif()

# Explicit opt-in benchmark setup; never generated during ordinary builds/CTest.
add_executable(benchmark-images EXCLUDE_FROM_ALL benchmarks/CreateImages.cpp)
target_link_libraries(benchmark-images PRIVATE TIFF::TIFF grok::grok)
target_compile_features(benchmark-images PRIVATE cxx_std_20)
set_target_properties(benchmark-images PROPERTIES CXX_EXTENSIONS OFF)

# Offline real-image benchmark preparation, never part of the runtime image.
add_executable(prepare-archive-image EXCLUDE_FROM_ALL benchmarks/PrepareArchiveImage.cpp)
target_link_libraries(prepare-archive-image PRIVATE TIFF::TIFF grok::grok lcms::lcms j2kreader imagestorage)
target_compile_features(prepare-archive-image PRIVATE cxx_std_20)
set_target_properties(prepare-archive-image PROPERTIES CXX_EXTENSIONS OFF)

# Offline resampling baseline; excluded from normal builds and runtime images.
add_executable(resize-benchmark EXCLUDE_FROM_ALL benchmarks/ResizeBenchmark.cpp)
target_link_libraries(resize-benchmark PRIVATE imageprocessing tiffreader j2kreader
    imagestorage jpegwriter JsonCpp::JsonCpp)
target_compile_features(resize-benchmark PRIVATE cxx_std_20)
set_target_properties(resize-benchmark PROPERTIES CXX_EXTENSIONS OFF)
