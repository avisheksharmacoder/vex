import platform
from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

IS_X86_64 = platform.machine().lower() in ("x86_64", "amd64")

C_SOURCES = [
    "vex_py.c",
    "../src/vx_arena.c",
    "../src/vx_spsc.c",
    "../src/vx_log.c",
    "../src/vx_wmesh.c",
    "../src/vx_str.c",
    "../src/vx_chunk.c",
    "../src/vx_simd.c",
    "../src/vx_simd_avx2.c",
    "../src/vx_ebr.c",
    "../src/vx_wal.c",
    "../src/vx_snap.c",
    "../src/vx_rhmap.c",
    "../src/vx_delmap.c",
    "../src/vx_lexer.c",
    "../src/vx_parser.c",
    "../src/vx_db.c",
    "../src/vx_exec.c",
]


class PortableSIMDBuildExt(build_ext):
    def build_extensions(self):
        orig_compile = self.compiler._compile

        def _compile_with_simd(obj, src, ext, cc_args, extra_postargs, pp_opts):
            post = list(extra_postargs or [])
            if IS_X86_64 and src.replace("\\", "/").endswith("vx_simd_avx2.c"):
                post.append("-mavx2")
            return orig_compile(obj, src, ext, cc_args, post, pp_opts)

        self.compiler._compile = _compile_with_simd
        try:
            super().build_extensions()
        finally:
            self.compiler._compile = orig_compile


ext = Extension(
    name="vex",
    sources=C_SOURCES,
    include_dirs=["../include"],
    extra_compile_args=[
        "-std=c11",
        "-D_POSIX_C_SOURCE=200809L",
        "-O3",
        "-Wall",
        "-Wextra",
        "-pthread",
    ],
    extra_link_args=["-pthread", "-lm"],
)

setup(
    name="vexdb",
    version="1.0.0",
    ext_modules=[ext],
    cmdclass={"build_ext": PortableSIMDBuildExt},
)
