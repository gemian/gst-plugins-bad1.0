hybris_sources = [
  'gsthybrissink.c',
  'mirpool.c',
]

wpe_feat = get_option('wpe').require(gstgl_dep.found(),
  error_message : 'wpe plugin enabled but GL support was not detected')
egl_dep = dependency('egl', required : wpe_feat)

gsthybris = library('gsthybris',
  hybris_sources,
  c_args : gst_plugins_bad_args,
  include_directories : [configinc, '/usr/include/ubuntu'],
  dependencies : [gstvideo_dep, gstmiralloc_dep, egl_dep, gstglegl_dep],
  install : true,
  install_dir : plugins_install_dir,
)
plugins += [gsthybris]
