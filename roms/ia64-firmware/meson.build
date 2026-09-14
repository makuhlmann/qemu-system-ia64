# SPDX-License-Identifier: GPL-2.0-or-later
# IA-64 firmware build rules

sh_prog = find_program('sh')
build_fw_script = files('build_firmware.sh')
firmware_manifest = 'firmware.sources'
firmware_inputs = fs.read(firmware_manifest).strip().split('\n')

ia64_firmware_bin = custom_target(
  'ia64-firmware.bin',
  output: [
    'ia64-firmware.bin',
    'ia64-firmware.raw',
    'ia64-firmware.elf',
    'ia64-firmware.map',
    'ia64-firmware.sections',
  ],
  input: files(firmware_inputs + [firmware_manifest]),
  command: [
    sh_prog,
    build_fw_script,
    '@OUTPUT0@',
    '@OUTPUT1@',
    '@OUTPUT2@',
    '@OUTPUT3@',
    '@OUTPUT4@',
    meson.current_source_dir(),
    '@DEPFILE@',
  ],
  depfile: 'ia64-firmware.d',
  build_by_default: true,
  install: false,
)
