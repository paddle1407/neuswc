neuswc
------

This repository is the [neuswc fork maintained for charaWC](https://github.com/paddle1407/neuswc).
It provides a C interface for building a Wayland compositor and is based on
Michael Forney's [swc](https://github.com/michaelforney/swc), through the
[wayland.fyi](https://wayland.fyi) neuswc project. The desktop using this
version is [charaWC](https://github.com/paddle1407/charawc).

Linux DRM/libinput and framebuffer backends are supported by the build.
The tree also retains BSD wsdisplay/wscons paths; hardware and platform
validation depends on the environment in which it is built.


neu features
------------

- z axis ordering
- more cursor functions
- zooming
- experimental subsurface support
- fullscreen
- double window borders
- screenshots
- layer shell support
- window decorations
- framebuffer support

neuswc is in active development, we plan to add many more features, and increase compatibility with new wayland protocols.

build
-----

you will need: 
- A C11-compatible compiler
- Meson 1.8 or newer and Ninja
- pkg-config
- wayland-scanner, wayland-server, wayland-client and wayland-protocols
- libdrm (if  building with DRM support) 
- pixman, xkbcommon
- [neuwld](https://github.com/paddle1407/neuwld)
- libinput on Linux and wscons on BSD
- xcb, xcb-composite, xcb-ewmh and xcb-icccm if you want Xwayland support.

The protocol package must provide the XML files used in `protocol/meson.build`,
including stable tablet v2 and staging ext-workspace. The charaWC CI uses
wayland-protocols 1.49.

```
meson setup build
ninja -C build
meson install -C build
```

Configure a Linux framebuffer build with `-Dvideo=fb -Dxwayland=disabled`.
On Linux, tests are registered with Meson; compiling does not execute them. Run
`meson test -C build --print-errorlogs` explicitly for rendering, input-mode,
clipboard authorization and keyboard-focus checks. The default checks do
not require a desktop session or GPU. The overview rendering executable also
offers an explicit `--gpu /dev/dri/renderD...` mode in DRM builds.

credits
-------
an extremely large thank you to [michael forney](https://mforney.org) for creating the original [swc](https://github.com/michaelforney/swc) and [wld](https://github.com/michaelforney/wld), without his amazing work none of this would be possible! once they are more stable, we hope some of the changes from our neu* forks will eventually be upstreamed into the original projects.

repositories
------------

- [Current charaWC fork](https://github.com/paddle1407/neuswc)
- [Historical neuswc sourcehut repository](https://git.sr.ht/~shrub900/neuswc)
