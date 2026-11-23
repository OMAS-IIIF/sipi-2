#include "image/SourceResolver.h"
#include "storage/LocalFileSource.h"
#include <array>
#include <cstring>
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
    // Race between resolution and open(): a resolved directory is swapped for a
    // symlink to outside. The path check already passed; the descriptor check
    // must reject the file that was actually opened.
    fs::create_directories(root / "swap");
    { std::ofstream(root / "swap" / "image.tif") << "inside"; }
    { std::ofstream(base / "images-other" / "image.tif") << "outside"; }
    const auto before_swap = std::get<sipi::LocalSource>(sources.resolve_source("swap/image.tif"));
    check(before_swap.root == fs::canonical(root), "relative source carries its repository");
    {
        sipi::LocalFileSource unchanged(before_swap.path);
        require_opened_inside(before_swap, unchanged.opened_path());
        check(unchanged.opened_path() == fs::canonical(root / "swap" / "image.tif"), "descriptor path is canonical");
    }
    fs::remove_all(root / "swapped-away");
    fs::rename(root / "swap", root / "swapped-away");
    fs::create_directory_symlink(base / "images-other", root / "swap");
    {
        sipi::LocalFileSource redirected(before_swap.path); // Follows the new symlink.
        std::array<std::byte, 7> bytes{};
        check(redirected.read_at(0, bytes) == 7 &&
              std::memcmp(bytes.data(), "outside", 7) == 0, "stale path really reaches outside");
        rejects([&] { require_opened_inside(before_swap, redirected.opened_path()); },
                SourceErrorCode::invalid_path, "symlink swap after resolution is rejected");
        // Trusted absolute overrides are not confined to the repository.
        const sipi::LocalSource trusted{before_swap.path};
        require_opened_inside(trusted, redirected.opened_path());
        check(!trusted.root, "absolute override has no repository bound");
    }
    fs::remove(root / "swap");
    fs::rename(root / "swapped-away", root / "swap");
    check(!std::get<sipi::LocalSource>(sources.resolve_source("ignored",
          sipi::SourceOverride{fs::canonical(root / "sample.tif")})).root, "absolute override unbounded");
    {
        // An unlinked file has no path: report a change, never a stale name.
        { std::ofstream(root / "removed.tif") << "gone"; }
        sipi::LocalFileSource removed(root / "removed.tif");
        fs::remove(root / "removed.tif");
        try { (void)removed.opened_path(); check(false, "unlinked file reports change"); }
        catch (const sipi::ByteSourceError& error) {
            check(error.code() == sipi::ByteSourceErrorCode::changed, "unlinked file reports change");
        }
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
