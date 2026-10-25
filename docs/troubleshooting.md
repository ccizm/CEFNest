# Troubleshooting

English | [简体中文](troubleshooting.zh-CN.md)

| Symptom | Fix |
| --- | --- |
| `No usable sandbox!` at helper start | the host restricts unprivileged user namespaces. Install an AppArmor profile allowing userns for `cefnest-helper` (recommended), or set `CHROMIUM_HELPER_NO_SANDBOX=1` |
| `start()` says `no helper binary at …` | that exact path is what was tried: install `cefnest-helper`, or point `CEFNEST_HELPER` at an absolute path; in a dev tree, run cmake first |
| Window shows a blank page area | confirm the helper is running and the session is a native Wayland session (`GDK_BACKEND=wayland`); X11 does not render |
| `G_MESSAGES_DEBUG` prints nothing | the domain list is **space**-separated; a comma list matches nothing |
| Closing the last tab closes the page | one session is one helper; open a new browser first, or handle `::closed` |
| `Lost connection to Wayland compositor` before `::helper-exited` | normal teardown order — destroy your display only after `::helper-exited` |
| The helper is quiet on stderr | deliberate: Chromium's log severity is WARNING, so per-event INFO chatter is gone; warnings and errors still print |
