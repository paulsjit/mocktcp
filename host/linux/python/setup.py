from setuptools import setup
from pybind11.setup_helpers import Pybind11Extension, build_ext, ParallelCompile

build_type = os.environ.get('BUILD') or 'release'
sync_type = False if os.environ.get('NOSYNC') else True

try:
    debug_file = os.environ['DEBUG']
    define_macros = [('MTCP_DEBUG', debug_file[0] + debug_file[1:])]
except IndexError:
    define_macros = [('MTCP_DEBUG', 1)]
except KeyError:
    define_macros = []

if sync_type:
    define_macros.append(('CONFIG_MTCP_ENABLE_SYNC', 1))

__version__ = "0.0.1"

if os.environ.get('NOPAR') is None:
    ParallelCompile(default=0).install()

ext_modules = [
    Pybind11Extension("mocktcp_lib",
        ["mocktcp_lib_api.cpp", "../src/mocktcp_lib.c"],
        define_macros = define_macros,
	include_dirs = ['../src', '../../../common'],
        extra_compile_args=(['-O0', '-g3'] if build_type == 'debug' else ['-O3', '-g3']),
	extra_link_args=['-lpthread', '-lrt'],
    ),
]
setup(
    name="mocktcp",
    version=__version__,
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
)
