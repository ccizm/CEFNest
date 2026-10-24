executable(
  'cefnest-minimal',
  'minimal.c',
  include_directories: src_inc,
  dependencies: [libcefnest_dep],
  install: false,
)

executable(
  'cefnest-host',
  'host.c',
  include_directories: src_inc,
  dependencies: [libcefnest_dep],
  install: false,
)

# One focused, runnable demo per task:
examples_files = [
  'browser.c',     # single page: toolbar, entry, zoom, mute
  'tabs.c',        # multi-tab in the shared tab window
  'windows.c',     # windowed browsers, one GTK window each
  'script.c',      # javascript in, script-result/page-message out
  'findbar.c',     # in-page search with a find bar
  'downloads.c',   # accept into ~/Downloads with progress rows
  'menus.c',       # a self-drawn context menu popover
  'permissions.c', # permission asks as rows with allow/deny
  'errors.c',      # load-error / renderer-exited / closed banners
  'observe.c',     # frame counter and the accessibility switch
  'useragent.c',   # switch the User-Agent by restarting the helper
  'focus-chain.c', # gtk widgets and page elements in one tab order
]

foreach file : examples_files
  name = file.split('.')[0]
  executable(
    'cefnest-' + name,
    file,
    include_directories: src_inc,
    # browser.c turns a Blink zoom level into a percentage with pow().
    dependencies: [libcefnest_dep, m_dep],
    install: false,
  )
endforeach
