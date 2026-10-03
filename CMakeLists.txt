cmake_minimum_required(VERSION 3.24)

project(sipi-2 VERSION 0.1.0 LANGUAGES CXX)

find_package(spdlog CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(Drogon CONFIG REQUIRED)
find_package(TIFF CONFIG REQUIRED)
find_package(JPEG CONFIG REQUIRED)
find_package(lua CONFIG REQUIRED)

add_library(imagesource src/image/SourceResolver.cpp src/image/SourceResolver.h)
target_include_directories(imagesource PUBLIC src)
target_compile_features(imagesource PUBLIC cxx_std_20)
set_target_properties(imagesource PROPERTIES CXX_EXTENSIONS OFF)

# Standalone Lua policy evaluation; no HTTP or codec dependency.
add_library(preflight src/preflight/LuaPreflight.cpp src/preflight/LuaPreflight.h)
target_include_directories(preflight PUBLIC src)
target_link_libraries(preflight PRIVATE lua::lua)
target_compile_features(preflight PUBLIC cxx_std_20)
set_target_properties(preflight PROPERTIES CXX_EXTENSIONS OFF)

# Pure IIIF syntax parsing; no dependency on HTTP or image libraries.
add_library(iiifparser src/iiifparser/IIIFParser.cpp src/iiifparser/IIIFParser.h
    src/iiifparser/IIIFGeometry.cpp src/iiifparser/IIIFGeometry.h)
target_include_directories(iiifparser PUBLIC src)
target_compile_features(iiifparser PUBLIC cxx_std_20)
set_target_properties(iiifparser PROPERTIES CXX_EXTENSIONS OFF)

add_library(tiffreader src/imgformats/TiffReader.cpp src/imgformats/TiffReader.h src/imgformats/TiffMetadata.h
    src/image/PixelRegion.h src/image/RgbImage.h)
target_include_directories(tiffreader PUBLIC src)
target_link_libraries(tiffreader PRIVATE TIFF::TIFF)
target_compile_features(tiffreader PUBLIC cxx_std_20)
set_target_properties(tiffreader PROPERTIES CXX_EXTENSIONS OFF)

add_library(jpegwriter src/imgformats/JpegWriter.cpp src/imgformats/JpegWriter.h)
target_include_directories(jpegwriter PUBLIC src)
target_link_libraries(jpegwriter PRIVATE JPEG::JPEG)
target_compile_features(jpegwriter PUBLIC cxx_std_20)
set_target_properties(jpegwriter PROPERTIES CXX_EXTENSIONS OFF)

add_executable(sipi-2 src/main.cpp src/CommandLine.h
        src/CommandLine.cpp src/HttpServer.h src/HttpServer.cpp
        src/http/IiifWorker.h src/http/IiifWorker.cpp src/http/IiifInfo.h src/http/IiifInfo.cpp)
target_link_libraries(sipi-2 PRIVATE spdlog::spdlog CLI11::CLI11 Drogon::Drogon iiifparser preflight imagesource tiffreader jpegwriter)
target_compile_definitions(sipi-2 PRIVATE SIPI_VERSION="${PROJECT_VERSION}")

# Require standard C++20 for this target, without compiler-specific extensions.
target_compile_features(sipi-2 PRIVATE cxx_std_20)
set_target_properties(sipi-2 PROPERTIES CXX_EXTENSIONS OFF)

include(CTest)
if(BUILD_TESTING)
    add_executable(jpeg-writer-tests tests/JpegWriterTests.cpp)
    target_link_libraries(jpeg-writer-tests PRIVATE jpegwriter JPEG::JPEG)
    set_target_properties(jpeg-writer-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME jpeg-writer COMMAND jpeg-writer-tests)
    add_executable(source-resolver-tests tests/SourceResolverTests.cpp)
    target_link_libraries(source-resolver-tests PRIVATE imagesource)
    set_target_properties(source-resolver-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME source-resolution COMMAND source-resolver-tests "${CMAKE_CURRENT_BINARY_DIR}/source-test-data")
    find_package(Threads REQUIRED)
    add_executable(iiif-worker-tests tests/http/IiifWorkerTests.cpp src/http/IiifWorker.cpp src/http/IiifInfo.cpp)
    target_link_libraries(iiif-worker-tests PRIVATE preflight imagesource tiffreader jpegwriter iiifparser Drogon::Drogon spdlog::spdlog Threads::Threads)
    set_target_properties(iiif-worker-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiif-worker COMMAND iiif-worker-tests
             "${CMAKE_CURRENT_BINARY_DIR}/worker-test-data")
    set_tests_properties(iiif-worker PROPERTIES TIMEOUT 15)
    add_executable(preflight-tests tests/preflight/LuaPreflightTests.cpp)
    target_link_libraries(preflight-tests PRIVATE preflight Threads::Threads)
    set_target_properties(preflight-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME lua-preflight COMMAND preflight-tests
             "${CMAKE_CURRENT_BINARY_DIR}/preflight-test-data")
    set_tests_properties(lua-preflight PROPERTIES TIMEOUT 15)
    add_executable(tiff-region-tests tests/TiffRegionTests.cpp)
    target_link_libraries(tiff-region-tests PRIVATE tiffreader iiifparser TIFF::TIFF)
    set_target_properties(tiff-region-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME tiff-region COMMAND tiff-region-tests "${CMAKE_CURRENT_BINARY_DIR}/tiff-region-data")
    add_executable(tiff-metadata-tests tests/TiffMetadataTests.cpp src/CommandLine.cpp)
    target_link_libraries(tiff-metadata-tests PRIVATE tiffreader iiifparser TIFF::TIFF CLI11::CLI11)
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
    add_executable(command-line-tests tests/CommandLineTests.cpp src/CommandLine.cpp)
    target_include_directories(command-line-tests PRIVATE src)
    target_link_libraries(command-line-tests PRIVATE CLI11::CLI11)
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
    add_test(NAME http-routing
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/http_smoke.py"
                     $<TARGET_FILE:sipi-2> "${CMAKE_CURRENT_BINARY_DIR}/http-test-data" $<TARGET_FILE:jpeg-writer-tests>)
    set_tests_properties(http-routing PROPERTIES TIMEOUT 30 FIXTURES_REQUIRED http_images)
endif()
