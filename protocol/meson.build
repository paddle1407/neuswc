fs = import('fs')

# Allow easy lookup for protocol for utility applications
protos = {}
foreach proto : [
  'server-decoration.xml',
  'swc.xml',
  'swc_snap.xml',
  'swc_select.xml',
  'wlr-layer-shell-unstable-v1.xml',
  'wlr-foreign-toplevel-management-unstable-v1.xml',
  'wlr-screencopy-unstable-v1.xml',
  'wayland-drm.xml',
  wl.find_protocol('xdg-shell', state: 'stable'),
  wl.find_protocol('linux-dmabuf', state: 'unstable', version: 1),
  wl.find_protocol('xdg-decoration', state: 'unstable', version: 1),
  wl.find_protocol('xdg-output', state: 'unstable', version: 1),
  wl.find_protocol('pointer-constraints', state: 'unstable', version: 1),
  wl.find_protocol('relative-pointer', state: 'unstable', version: 1),
  wl.find_protocol('idle-inhibit', state: 'unstable', version: 1),
  wl.find_protocol('ext-workspace', state: 'staging', version: 1),
  wl.find_protocol('linux-drm-syncobj', state: 'staging', version: 1),
  wl.find_protocol('primary-selection', state: 'unstable', version: 1),
  wl.find_protocol('ext-idle-notify', state: 'staging', version: 1),
  wl.find_protocol('ext-session-lock', state: 'staging', version: 1),
  wl.find_protocol('cursor-shape', state: 'staging', version: 1),
  wl.find_protocol('xdg-activation', state: 'staging', version: 1),
  wl.find_protocol('fractional-scale', state: 'staging', version: 1),
  wl.find_protocol('xdg-system-bell', state: 'staging', version: 1),
  wl.find_protocol('tablet', state: 'stable', version: 2),
  wl.find_protocol('viewporter'),
  wl.find_protocol('text-input', state: 'unstable', version: 3),
  'input-method-unstable-v2.xml',
]
  protocols = wl.scan_xml(proto,
    public: true,
    client: true,
    server: true)
  protos += {fs.stem(fs.name(proto)): protocols}
endforeach
install_data('swc.xml', install_dir: get_option('datadir') / 'swc')
