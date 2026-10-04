#include "image/SourceResolver.h"
#include <fstream>
#include <iostream>

namespace {
int checks = 0, failures = 0;
void check(bool value, const char* name) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
template<class Function>
void rejects(Function function, sipi::SourceErrorCode expected, const char* name) {
    try { function(); check(false, name); }
    catch (const sipi::SourceError& error) { check(error.code() == expected, name); }
}
}

/** Filesystem-backed tests cover actual symlinks and component containment. */
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    using namespace sipi;
    namespace fs = std::filesystem;
    const auto base = fs::absolute(argv[1]);
    const auto root = base / "images";
    fs::create_directories(root / "nested");
    fs::create_directories(base / "images-other");
    { std::ofstream(root / "sample.tif") << "fixture"; }
    { std::ofstream(root / "nested" / "sample.tif") << "fixture"; }
    { std::ofstream(root / "a%2Fb") << "fixture"; }
    { std::ofstream(base / "images-other" / "outside.tif") << "fixture"; }
    for (const auto* link : {"inside", "outside", "sibling"}) fs::remove(root / link);
    fs::create_symlink(root / "sample.tif", root / "inside");
    fs::create_directory_symlink(base, root / "outside");
    fs::create_directory_symlink(base / "images-other", root / "sibling");
    const SourceResolver sources(root);
    check(sources.resolve("sample.tif") == fs::canonical(root / "sample.tif"), "default identifier");
    check(sources.resolve("nested/sample.tif") == fs::canonical(root / "nested/sample.tif"), "nested identifier stays inside root");
    check(sources.resolve("ignored", "sample.tif") == fs::canonical(root / "sample.tif"), "relative override");
    check(sources.resolve("inside") == fs::canonical(root / "sample.tif"), "internal symlink");
    check(sources.resolve("a%2Fb") == fs::canonical(root / "a%2Fb"), "no second URL decode");
    check(SourceResolver{}.resolve("ignored", base / "images-other/outside.tif") ==
          fs::canonical(base / "images-other/outside.tif"), "absolute override without root");
    check(sources.resolve("ignored", base / "images-other/outside.tif") ==
          fs::canonical(base / "images-other/outside.tif"), "absolute script override outside root");
    for (const auto* id : {"../images-other/outside.tif", "/etc/passwd", "nested/../../file", "a\\b"})
        rejects([&] { (void)sources.resolve(id); }, SourceErrorCode::invalid_path, "unsafe identifier");
    rejects([&] { (void)sources.resolve(std::string("a\0b", 3)); }, SourceErrorCode::invalid_path, "NUL identifier");
    rejects([&] { (void)sources.resolve("outside/images-other/outside.tif"); }, SourceErrorCode::invalid_path, "outside symlink");
    rejects([&] { (void)sources.resolve("sibling/outside.tif"); }, SourceErrorCode::invalid_path, "not a string-prefix boundary");
    rejects([&] { (void)sources.resolve("ignored", "../images-other/outside.tif"); }, SourceErrorCode::invalid_path, "relative override traversal");
    rejects([&] { (void)sources.resolve("missing"); }, SourceErrorCode::not_found, "missing image");
    rejects([&] { (void)sources.resolve("nested"); }, SourceErrorCode::not_found, "directory is not an image");
    rejects([&] { (void)SourceResolver{}.resolve("sample.tif"); }, SourceErrorCode::configuration, "no implicit root");
    rejects([&] { (void)SourceResolver{}.resolve("ignored", "sample.tif"); }, SourceErrorCode::configuration, "relative override needs root");
    rejects([&] { SourceResolver invalid(base / "absent"); }, SourceErrorCode::configuration, "missing root");
    rejects([&] { SourceResolver invalid(root / "sample.tif"); }, SourceErrorCode::configuration, "root must be directory");
    const SourceResolver remote(root, sipi::StorageKind::s3, "test-bucket");
    const auto selected = std::get<sipi::S3Source>(remote.resolve_source("folder/a%2Fb"));
    check(selected.bucket == "test-bucket" && selected.key == "folder/a%2Fb", "literal S3 key");
    check(std::get<sipi::LocalSource>(remote.resolve_source("ignored", sipi::SourceOverride{fs::path("sample.tif")})).path ==
          fs::canonical(root / "sample.tif"), "string override stays local with S3 default");
    const SourceResolver mixed(root, sipi::StorageKind::local, "test-bucket");
    check(std::get<sipi::S3Source>(mixed.resolve_source("ignored", sipi::SourceOverride{sipi::S3Key{"project/key"}})).key ==
          "project/key", "explicit S3 override with local default");
    rejects([&] { (void)sources.resolve_source("ignored", sipi::SourceOverride{sipi::S3Key{"key"}}); },
            SourceErrorCode::configuration, "S3 override requires bucket");
    rejects([&] { SourceResolver invalid({}, sipi::StorageKind::s3); },
            SourceErrorCode::configuration, "S3 default requires bucket");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
