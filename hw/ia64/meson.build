ia64_ss = ss.source_set()
ia64_ss.add(when: 'CONFIG_IA64_VPC', if_true: files(
  'ia64_vpc.c',
  'ia64_loader.c',
  'ia64_pci.c',
  'ia64_iosapic.c',
  'ia64_agp.c',
  'ia64_sba.c',
  'ia64_lba.c',
  'ia64_mercury.c',
  'ia64_expander.c',
))
hw_arch += {'ia64': ia64_ss}
