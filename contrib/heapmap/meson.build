# contrib/heapmap: learned block-range index

heapmap_sources = files(
  'hm_am.c',
  'hm_build.c',
  'hm_exc.c',
  'hm_insert.c',
  'hm_model.c',
  'hm_scan.c',
)

if host_system == 'windows'
  heapmap_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'heapmap',
    '--FILEDESC', 'heapmap access method - learned block-range index',])
endif

heapmap = shared_module('heapmap',
  heapmap_sources,
  c_args: ['-ffp-contract=off'],
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)
contrib_targets += heapmap

install_data(
  'heapmap.control',
  'heapmap--1.0.sql',
  kwargs: contrib_data_args,
)
