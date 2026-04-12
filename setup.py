import platform

from setuptools import setup, Extension

machine = platform.machine()
additional_files = []
if machine == "armv7l":
    additional_files = ["backends/libdxcompiler_armhf.so"]
elif machine == "aarch64":
    additional_files = [
        "backends/libdxcompiler_aarch64.so",
        "backends/libdxcompiler_armhf.so",
    ]
else:
    additional_files = ["backends/libdxcompiler_x86_64.so"]

backends = [
    Extension(
        "compushady.backends.vulkan",
        libraries=["vulkan"],
        sources=["compushady/backends/vulkan.cpp", "compushady/backends/common.cpp"],
        extra_compile_args=["-std=c++14", "-O3", "-march=native"],
    ),
    Extension(
        "compushady.backends.dxc",
        sources=["compushady/backends/dxc.cpp", "compushady/backends/common.cpp"],
        extra_compile_args=["-std=c++14", "-O3"],
    ),
]

setup(
    packages=["compushady", "compushady.shaders"],
    package_data={"compushady": additional_files},
    ext_modules=backends,
)
