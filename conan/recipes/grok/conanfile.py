"""Pinned CPU-only Grok core; callbacks use SIPI's own storage implementation."""
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get
import os


class GrokRecipe(ConanFile):
    name = "grok"
    version = "20.4.15"
    package_type = "shared-library"
    license = "AGPL-3.0-only"
    homepage = "https://github.com/GrokImageCompression/grok"
    settings = "os", "arch", "compiler", "build_type"

    def layout(self):
        cmake_layout(self)

    def source(self):
        # Archive exports omit submodules. Fetch only the three used by the core,
        # at the exact revisions recorded by the Grok release, with SHA256 checks.
        for item in self.conan_data["sources"][self.version].values():
            get(self, item["url"], sha256=item["sha256"],
                destination=item["destination"], strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["BUILD_SHARED_LIBS"] = True
        tc.variables["CMAKE_POSITION_INDEPENDENT_CODE"] = True
        for option in ("GRK_BUILD_CODEC", "GRK_BUILD_CORE_EXAMPLES",
                       "GRK_BUILD_CODEC_EXAMPLES", "GRK_BUILD_CORE_SWIG_BINDINGS",
                       "GRK_BUILD_PLUGIN_LOADER", "GRK_BUILD_LIBPNG",
                       "GRK_BUILD_LIBTIFF", "GRK_BUILD_JPEG", "GRK_ENABLE_LIBCURL",
                       "BUILD_TESTING"):
            tc.variables[option] = False
        # CPU C++ path only: no optional Cargo build or network/cloud clients.
        tc.cache_variables["CARGO_EXECUTABLE"] = "OFF"
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build(target="grokj2k")

    def package(self):
        CMake(self).install(component="Libraries")
        CMake(self).install(component="Headers")
        copy(self, "LICENSE*", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "grok")
        self.cpp_info.set_property("cmake_target_name", "grok::grok")
        self.cpp_info.libs = ["grokj2k"]
        self.cpp_info.includedirs = ["include/grok-20.4"]
