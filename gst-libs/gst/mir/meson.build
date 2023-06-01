mir_sources = ['gstmircontext.c','mirallocator.c']
mir_headers = ['gstmircontext.h', 'mirallocator.h', 'mir-prelude.h']
install_headers(mir_headers, subdir : 'gstreamer-1.0/gst/mir')

libmedia = cc.find_library('media', required : false)

gstmiralloc = library('gstmiralloc-' + api_version,
  mir_sources,
  c_args : gst_plugins_bad_args + ['-DGST_USE_UNSTABLE_API', '-DBUILDING_GST_MIR', '-DG_LOG_DOMAIN="GStreamer-MirAllocator"'],
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  darwin_versions : osxversion,
  install : true,
  dependencies : [gstbase_dep, gstvideo_dep, gst_dep, libmedia],
)

pkg_name = 'gstreamer-miralloc-1.0'
pkgconfig.generate(gstmiralloc,
  libraries : [gstbase_dep, gstvideo_dep, gst_dep, libmedia],
  variables : pkgconfig_variables,
  subdirs : pkgconfig_subdirs,
  name : pkg_name,
  description : 'Mir Allocator',
)

gstmiralloc_dep = declare_dependency(link_with : gstmiralloc,
  include_directories : [libsinc],
  dependencies : [gstbase_dep, gstvideo_dep, gst_dep, libmedia])

gst_libraries += [[pkg_name, {'lib': gstmiralloc}]]
meson.override_dependency(pkg_name, gstmiralloc_dep)
