foreach name : ['overview_render', 'input_mode', 'repaint', 'selection', 'keyboard_focus']
  sources = [name + '_test.c']
  if name == 'selection'
    sources += protos['primary-selection-unstable-v1']
  endif
  program = executable(name + '_test', sources,
    dependencies: requires + requires_private,
    include_directories: include_directories('../libswc', '../protocol'),
    c_args: ['-ffunction-sections', '-fdata-sections'],
    link_args: ['-Wl,--gc-sections', '-lm'],
    override_options: ['b_ndebug=false'],
  )
  test(name, program, timeout: 10)
endforeach
