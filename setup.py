import platform

from setuptools import setup, Extension

# Linux-only: hardcode Vulkan backend
machine = platform.machine()

# DXC shared libraries for Linux
additional_files = []
if machine == "armv7l":
    additional_files = ["backends/libdxcompiler_armhf.so"]
elif machine == "aarch64":
    additional_files = [
        "backends/libdxcompiler_aarch64.so",
        "backends/libdxcompiler_armhf.so",  # for 32-bit compatibility
    ]
else:
    additional_files = ["backends/libdxcompiler_x86_64.so"]

# Vulkan extension – all split C files
vulkan_sources = [
    "compushady/backends/vulkan_module.cpp",
    "compushady/backends/vulkan_device.cpp",
    "compushady/backends/vulkan_resource.cpp",
    "compushady/backends/vulkan_swapchain.cpp",
    "compushady/backends/vulkan_compute.cpp",
    "compushady/backends/vulkan_utils.cpp",
    "compushady/backends/vulkan_sampler.cpp",
    "compushady/backends/common.cpp",
]

# DXC extension
dxc_sources = [
    "compushady/backends/dxc.cpp",
    "compushady/backends/common.cpp",
]

backends = [
    Extension(
        "compushady.backends.vulkan",
        sources=vulkan_sources,
        libraries=["vulkan"],
        extra_compile_args=["-std=c++14", "-O3", "-mtune=generic"],
        depends=[
            "compushady/backends/vulkan_common.h",
            "compushady/backends/compushady.h",
        ],
        language="c++",
    ),
    Extension(
        "compushady.backends.dxc",
        sources=dxc_sources,
        extra_compile_args=["-std=c++14", "-O3"],
        depends=["compushady/backends/compushady.h", "compushady/backends/dxcapi.h"],
        language="c++",
    ),
]

setup(
    packages=["compushady", "compushady.shaders"],
    package_data={"compushady": additional_files},
    ext_modules=backends,
)
