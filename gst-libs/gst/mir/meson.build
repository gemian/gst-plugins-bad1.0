miralloc_sources = [
  'mirallocator.c',
  'gstmircontext.c',
]

miralloc_headers = [
  'mirallocator.h',
  'gstmircontext.h',
]
install_headers(miralloc_headers, subdir : 'gstreamer-1.0/gst/mir')


gstmiralloc = library('gstmiralloc-' + api_version,
  miralloc_sources,
  c_args : gst_plugins_bad_args + ['-DGST_USE_UNSTABLE_API'],
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gstbase_dep],
)

gstmiralloc_dep = declare_dependency(link_with : gstmiralloc,
  include_directories : [libsinc],
  dependencies : [gstbase_dep])
