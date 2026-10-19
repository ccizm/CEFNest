src_inc = include_directories('.')

libnested = static_library(
  'nested',
  [ 'nested.c', 'nested-dmabuf.c', 'nested-seat.c', 'nested-data-device.c',
    'nested-xdg.c', 'nested-text-input.c',
    xdg_shell_server_header, xdg_shell_code,
    dmabuf_server_header, dmabuf_code,
    viewporter_server_header, viewporter_code,
    fractional_scale_server_header, fractional_scale_code,
    text_input_server_header, text_input_code ],
  include_directories: gen_inc,
  dependencies: [ gtk_dep, wl_server_dep, xkb_dep, gbm_dep, drm_dep, threads_dep ],
)

libnested_dep = declare_dependency(
  link_with: libnested,
  include_directories: src_inc,
  dependencies: [ gtk_dep, wl_server_dep, xkb_dep, gbm_dep, drm_dep ],
)

chromium_demo = executable(
  'chromium-demo',
  [ 'main.c', 'chromium-view.c', 'chromium-session.c' ],
  c_args: [
    '-DCLIENT_PATH="' + meson.current_build_dir() + '/../test/shm-client"',
    '-DHELPER_PATH="' + meson.current_source_dir() + '/../helper/build/Release/chromium-helper"',
  ],
  dependencies: [ libnested_dep, gtk_dep, json_dep ],
  install: false,
)
