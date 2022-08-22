libvips_sources = []
libvips_components = []
subdir('include/vips')
subdir('foreign')
if get_option('deprecated')
    subdir('deprecated')
endif
subdir('arithmetic')
subdir('resample')
subdir('colour')
subdir('conversion')
subdir('convolution')
subdir('freqfilt')
subdir('histogram')
subdir('draw')
subdir('iofuncs')
subdir('morphology')
subdir('mosaicing')
subdir('create')

libvips_lib = library('vips',
    enumtypes,
    link_whole: libvips_components,
    dependencies: libvips_deps,
    version: library_version,
    darwin_versions: darwin_versions,
    gnu_symbol_visibility: 'hidden',
    install: true,
    link_args: nodelete_link_args,
)

libvips_dep = declare_dependency(
    link_with: libvips_lib,
    dependencies: libvips_deps,
)

pkg.generate(
    libvips_lib,
    requires: [ glib_dep, gio_dep, gobject_dep ],
    name: 'vips',
    description: 'Image processing library',
)

if get_option('introspection')
    vips_gir = gnome.generate_gir(
        libvips_lib,
        namespace: 'Vips',
        nsversion: '8.0',
        identifier_prefix: 'Vips',
        symbol_prefix: 'vips',
        header: 'vips/vips.h',
        sources: libvips_sources,
        dependencies: libvips_deps,
        includes: 'GObject-2.0',
        install: true
    )

    if get_option('vapi')
        gnome.generate_vapi(
            'vips',
            sources: vips_gir[0],
            packages: [ 'glib-2.0', 'gio-2.0', 'gobject-2.0' ],
            install: true
        )
    endif
endif

#
# The following configuration is only valid when the modules are enabled
#
if not modules_enabled
    subdir_done()
endif

# Keep the autotools convention for shared module suffix because GModule
# depends on it: https://gitlab.gnome.org/GNOME/glib/issues/1413
module_suffix = []
if ['darwin', 'ios'].contains(host_os)
  module_suffix = 'so'
endif

# For shared modules on Emscripten:
# - Use .wasm as shared module suffix
# - Compile and link with -sSIDE_MODULE=2
# - Ensure g_module_check_init is not DCE'd
module_c_args = ''
module_link_args = ''
if host_os == 'emscripten'
    module_suffix = 'wasm'
    module_c_args = '-sSIDE_MODULE=2'
    module_link_args = [module_c_args, '-sEXPORTED_FUNCTIONS=_g_module_check_init']
endif

if magick_module
    shared_module('vips-magick',
        'module/magick.c',
        magick_module_sources,
        magick_module_headers,
        c_args: module_c_args,
        link_args: module_link_args,
        name_prefix: '',
        name_suffix: module_suffix,
        dependencies: [libvips_dep, magick_dep],
        install: true,
        install_dir: module_dir
    )
endif

if libjxl_module
    shared_module('vips-jxl',
        'module/jxl.c',
        jpeg_xl_module_sources,
        c_args: module_c_args,
        link_args: module_link_args,
        name_prefix: '',
        name_suffix: module_suffix,
        dependencies: [libvips_dep, libjxl_dep, libjxl_threads_dep],
        install: true,
        install_dir: module_dir
    )
endif

if libheif_module
    shared_module('vips-heif',
        'module/heif.c',
        heif_module_sources,
        c_args: module_c_args,
        link_args: module_link_args,
        name_prefix: '',
        name_suffix: module_suffix,
        dependencies: [libvips_dep, libheif_dep],
        install: true,
        install_dir: module_dir
    )
endif

if libpoppler_module
    shared_module('vips-poppler',
        'module/poppler.c',
        poppler_module_sources,
        c_args: module_c_args,
        link_args: module_link_args,
        name_prefix: '',
        name_suffix: module_suffix,
        dependencies: [libvips_dep, libpoppler_dep, cairo_dep],
        install: true,
        install_dir: module_dir
    )
endif

if openslide_module
    shared_module('vips-openslide',
        'module/openslide.c',
        openslide_module_sources,
        c_args: module_c_args,
        link_args: module_link_args,
        name_prefix: '',
        name_suffix: module_suffix,
        dependencies: [libvips_dep, openslide_dep],
        install: true,
        install_dir: module_dir
    )
endif
