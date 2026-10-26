# The native plugin the plugin gate installs. The gate copies the manifest and
# the built module into a plugin directory of its own (the id is the directory
# name, and the gate wants one it chose), so nothing here is installed — but it
# is built, because a gate that loads a plugin nobody compiled is a gate that
# measures the failure to build.
shared_module(
  'plugin-demo',
  'plugin-demo.c',
  include_directories: src_inc,
  dependencies: [ libcefnest_dep ],
  install: false,
)
