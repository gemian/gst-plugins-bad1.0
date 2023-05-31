hybris_sources = [
  'gsthybrissink.c',
  'mirpool.c',
]

gsthybris = library('gsthybris',
  hybris_sources,
  c_args : gst_plugins_bad_args + ['-DGST_USE_UNSTABLE_API'],
  link_args : noseh_link_args,
  include_directories : [configinc, libsinc, '/usr/include/ubuntu'],
  dependencies : [gstvideo_dep, gstmiralloc_dep],
  install : true,
  install_dir : plugins_install_dir,
)
plugins += [gsthybris]
hybris_dep = declare_dependency(include_directories : include_directories('.'))
