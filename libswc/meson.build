install_headers('swc.h')

libswc_src = files(
  'bindings.c',
  'compositor.c',
  'cursor_shape.c',
  'data.c',
  'data_device.c',
  'data_device_manager.c',
  'decor.c',
  'drag.c',
  'foreign_toplevel.c',
  'idle_inhibit.c',
  'idle_notify.c',
  'titlebar.c',
  'input.c',
  'input_mode.c',
  'kde_decoration.c',
  'keyboard.c',
  'layer_shell.c',
  'launch.c',
  'mode.c',
  'output.c',
  'panel.c',
  'panel_manager.c',
  'pointer.c',
  'pointer_constraints.c',
  'primary_selection.c',
  'relative_pointer.c',
  'primary_plane.c',
  'region.c',
  'screen.c',
  'screencopy.c',
  'session_lock.c',
  'select.c',
  'snap.c',
  'shell.c',
  'shell_surface.c',
  'shm.c',
  'subcompositor.c',
  'subsurface.c',
  'surface.c',
  'text_input.c',
  'swc.c',
  'util.c',
  'view.c',
  'wallpaper.c',
  'wayland_buffer.c',
  'window.c',
  'workspace.c',
  'xdg_activation.c',
  'xdg_decoration.c',
  'xdg_output.c',
  'xdg_shell.c',

  input_backend == 'libinput' ? 'seat.c' : [],
  input_backend == 'wscons' ? 'seat-ws.c' : [],

  xwayland.found() ? ['xserver.c', 'xwm.c', 'xselection.c'] : [],  

  video_backend == 'drm' ? ['dmabuf.c', 'drm.c', 'drm_syncobj.c', 'plane.c'] : [],
  framebuffer_backend == 'fbdev' ? ['fb.c', 'fb_fbdev.c'] : [],
  framebuffer_backend == 'wsdisplay' ? ['fb.c', 'fb_wsdisplay.c'] : [],
)
libswc_src += [cursor, launch_protocol]

foreach proto, proto_src : protos
	libswc_src += proto_src
endforeach

libswc_lib = library(
	'swc',
	libswc_src,
	dependencies: requires + requires_private,
	install: true,
	include_directories: include_directories('..'),
)

# Allows other meson projects to use libswc
libswc = declare_dependency(
	link_with: libswc_lib,
	dependencies: requires + requires_private,
)

pkg.generate(
  libswc_lib,
  name: 'swc',
  description: 'A library for making a simple Wayland compositor',
  requires: requires,
  requires_private: requires_private,
)
