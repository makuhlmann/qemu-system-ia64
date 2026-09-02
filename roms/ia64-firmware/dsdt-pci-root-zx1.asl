// SPDX-License-Identifier: GPL-2.0-or-later
//
// zx1-profile DSDT: the PCI root bridge PCI0 is nested inside the HP zx1 SBA
// IOC device SBA0 (HWP0001), because Linux sba_iommu's sba_connect_bus() finds
// the IOC by walking *up* the ACPI parent chain from each PCI root ("the IOC
// scope encloses PCI root bridges in the ACPI namespace").  A sibling SBA0
// would make the device appear but never associate, so DMA would fall back to
// swiotlb.  This variant is published only when the machine selects
// chipset=zx1; the flat dsdt-pci-root.asl still serves the 460gx/E8870 guests
// (and XP 2002 / build 2600, which does not support zx1).
//
// Recompile with:  iasl -on -oi dsdt-pci-root-zx1.asl
//
// The -oi flag and the 0x00 constants inside _S5 and _PRT are load-bearing:
// they keep every *package element* a typed literal rather than a
// ZeroOp/OneOp constant opcode, which older Linux ACPI-CA rejects when walking
// package elements raw (see dsdt-pci-root.asl for the full rationale).  The
// PCI0 body below is copied verbatim from that file so the Windows-driven _CRS
// producer windows and _PRT behave identically; only the enclosing SBA0 device
// is new.
//
// PCI0 keeps _HID "PNP0A03" (not HWP0002): Windows XP 3790 / Server 2003 -- the
// Windows releases that support zx1 -- validate the install disk's ancestor
// bridges against a fixed list that only accepts a PNP0A03 _HID.  sba_iommu
// matches the HWP0001 ancestor, not the root's own _HID, so PNP0A03 is safe.

DefinitionBlock ("", "DSDT", 2, "QEMU  ", "IA64DSDT", 0x00000001)
{
    Name (_S5, Package (0x04)
    {
        0x00,
        0x00,
        0x00,
        0x00
    })

    Scope (\_SB)
    {
        Device (SBA0)
        {
            // HP zx1 System Bus Adapter / IOC (the IOMMU).  PNP0A05 makes the
            // OS enumerate the nested PCI root(s).
            Name (_HID, EisaId ("HWP0001"))
            Name (_CID, EisaId ("PNP0A05"))
            Name (_UID, Zero)
            Name (_CCA, One)
            // The IOC CSR block.  Linux sba_iommu locates the IOC base NOT from
            // a memory descriptor but from an HP-specific "CCSR" vendor-defined
            // resource: hp_acpi_csr_space() -> acpi_find_vendor_resource() looks
            // for a Vendor-Defined Large (0x84) resource whose data is
            //   guid_id=0x02, GUID 69e9adf9-924f-ab5f-f64a-24d201370ead,
            //   then u64 csr_base + u64 csr_length (16-byte payload).
            // It then reads the IOC registers at csr_base + ZX1_IOC_OFFSET
            // (0x1000) + IBASE(0x300); the model maps the IOMMU register window
            // at CSR offset 0x1300.  Emitting only a memory descriptor (as a
            // first attempt did) leaves the IOC uninitialised.  The GUID bytes
            // below are the EFI_GUID(0x69e9adf9,0x924f,0xab5f,f6,4a,24,d2,01,37,
            // 0e,ad) little-endian encoding.  Keep the base/length in lockstep
            // with IA64_SBA_CSR_BASE / IA64_SBA_CSR_SIZE.
            Name (_CRS, ResourceTemplate ()
            {
                VendorLong ()
                {
                    0x02,                                           // guid_id
                    0xF9, 0xAD, 0xE9, 0x69, 0x4F, 0x92, 0x5F, 0xAB, // GUID[0:7]
                    0xF6, 0x4A, 0x24, 0xD2, 0x01, 0x37, 0x0E, 0xAD, // GUID[8:15]
                    0x00, 0x00, 0xD0, 0xFE, 0x00, 0x00, 0x00, 0x00, // base 0xFED00000
                    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00  // length 0x10000
                }
                Memory32Fixed (ReadWrite, 0xFED00000, 0x00010000)
            })

            Device (PCI0)
            {
                Name (_HID, "PNP0A03")
                Name (_CID, "PNP0A03")
                Name (_SEG, Zero)
                Name (_BBN, Zero)
                Name (_UID, Zero)
                Name (_CCA, One)
                Name (_CRS, ResourceTemplate ()
                {
                    // PCI0 owns buses 0x00..0x0F; the Mercury root (LBA0) owns
                    // bus 0x10, so PCI0 no longer claims the whole 0..0xFF range.
                    WordBusNumber (ResourceProducer, MinFixed, MaxFixed,
                        PosDecode, 0, 0, 0x000F, 0, 0x0010)
                    // I/O with two holes, both behind the Mercury root: the
                    // legacy VGA ports 0x3B0..0x3DF (the graphics adapter is the
                    // VGA owner, so its root must decode the legacy VGA I/O as
                    // well as the 0xA0000 aperture -- splitting them across roots
                    // makes the VGA arbiter fail the device with Code 10) and the
                    // graphics I/O BAR at 0xC300..0xC3FF.  PCI0 must not claim
                    // either.
                    QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                        EntireRange, 0, 0, 0x000003AF, 0xFFFFC000000,
                        0x000003B0, , , , TypeTranslation, SparseTranslation)
                    QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                        EntireRange, 0, 0x000003E0, 0x0000C2FF, 0xFFFFC000000,
                        0x0000BF20, , , , TypeTranslation, SparseTranslation)
                    QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                        EntireRange, 0, 0x0000C400, 0x0000FFFF, 0xFFFFC000000,
                        0x00003C00, , , , TypeTranslation, SparseTranslation)
                    // Low MMIO: PCI0 device BARs (LSI/AHCI/USB/NIC) at 0xEE0xxxxx.
                    // The VGA legacy (0xA0000) / option-ROM (0xC0000) apertures
                    // and the high MMIO (0xF0000000+) belong to the Mercury root
                    // now that the graphics adapter moved there.
                    QWordMemory (ResourceProducer, PosDecode, MinFixed,
                        MaxFixed, NonCacheable, ReadWrite,
                        0, 0xEE000000, 0xEFFFFFFF, 0, 0x02000000)
                })
                Name (_PRT, Package ()
                {
                    Package () { 0x0000FFFF, 0, 0x00, 16 },
                    Package () { 0x0000FFFF, 1, 0x00, 17 },
                    Package () { 0x0000FFFF, 2, 0x00, 18 },
                    Package () { 0x0000FFFF, 3, 0x00, 19 },
                    Package () { 0x0001FFFF, 0, 0x00, 17 },
                    Package () { 0x0001FFFF, 1, 0x00, 18 },
                    Package () { 0x0001FFFF, 2, 0x00, 19 },
                    Package () { 0x0001FFFF, 3, 0x00, 16 },
                    Package () { 0x0002FFFF, 0, 0x00, 18 },
                    Package () { 0x0002FFFF, 1, 0x00, 19 },
                    Package () { 0x0002FFFF, 2, 0x00, 16 },
                    Package () { 0x0002FFFF, 3, 0x00, 17 },
                    Package () { 0x0003FFFF, 0, 0x00, 19 },
                    Package () { 0x0003FFFF, 1, 0x00, 16 },
                    Package () { 0x0003FFFF, 2, 0x00, 17 },
                    Package () { 0x0003FFFF, 3, 0x00, 18 },
                    Package () { 0x0004FFFF, 0, 0x00, 16 },
                    Package () { 0x0004FFFF, 1, 0x00, 17 },
                    Package () { 0x0004FFFF, 2, 0x00, 18 },
                    Package () { 0x0004FFFF, 3, 0x00, 19 },
                    Package () { 0x0005FFFF, 0, 0x00, 17 },
                    Package () { 0x0005FFFF, 1, 0x00, 18 },
                    Package () { 0x0005FFFF, 2, 0x00, 19 },
                    Package () { 0x0005FFFF, 3, 0x00, 16 },
                    Package () { 0x0006FFFF, 0, 0x00, 18 },
                    Package () { 0x0006FFFF, 1, 0x00, 19 },
                    Package () { 0x0006FFFF, 2, 0x00, 16 },
                    Package () { 0x0006FFFF, 3, 0x00, 17 }
                })
            }

            // HP zx1 LBA (Local Bus Adapter), the AGP-capable host bridge.
            // Linux hp-agp (drivers/char/agp/hp-agp.c) finds it via HWP0003,
            // reads its CSR block as PCI config space for an AGP capability, and
            // walks *up* the ACPI parent chain for the enclosing HWP0001 IOC
            // (SBA0) whose IOPDIR it reuses as the GART -- so LBA0 must nest
            // inside SBA0.  Its CSR base comes from the same HP CCSR
            // vendor-defined resource as the IOC (guid_id=0x02, GUID
            // 69e9adf9-...); keep base/length in lockstep with IA64_LBA_CSR_BASE
            // / IA64_LBA_CSR_SIZE and the ia64-zx1-lba device.
            // The Mercury (LBA/ioa) is a real PCI root bridge: it presents its
            // own PCI bus (0x10) with the AGP graphics adapter on it, exactly as
            // real zx1 puts the AGP master behind Mercury.  It carries _HID
            // HWP0003 (so the HP AgpMercury "hpagp" driver binds it) AND _CID
            // PNP0A03 (so Windows pci.sys owns it as a PCI root bridge and
            // enumerates the graphics on its child bus -- HpAgp.inf installs
            // pci.sys as the associated service on *HWP0003 and hpagp as a
            // filter).  It stays nested in SBA0 so Linux sba_iommu/hp-agp find
            // the enclosing HWP0001 IOC by walking up the ACPI parent chain.
            Device (LBA0)
            {
                Name (_HID, EisaId ("HWP0003"))
                Name (_CID, "PNP0A03")
                Name (_SEG, Zero)
                Name (_BBN, 0x10)
                Name (_UID, One)
                Name (_CCA, One)
                Name (_CRS, ResourceTemplate ()
                {
                    // Mercury owns exactly bus 0x10 (IA64_MERCURY_BUS).
                    WordBusNumber (ResourceProducer, MinFixed, MaxFixed,
                        PosDecode, 0, 0x0010, 0x0010, 0, 0x0001)
                    // Legacy VGA I/O ports (0x3B0..0x3DF): the graphics adapter
                    // is the VGA owner, so its root decodes the legacy VGA I/O
                    // (the VGA arbiter designates the root that decodes both the
                    // VGA legacy I/O and the 0xA0000 aperture as the VGA owner).
                    QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                        EntireRange, 0, 0x000003B0, 0x000003DF, 0xFFFFC000000,
                        0x00000030, , , , TypeTranslation, SparseTranslation)
                    // The graphics I/O BAR (the 0xC300..0xC3FF hole punched out
                    // of PCI0's I/O above).
                    QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                        EntireRange, 0, 0x0000C300, 0x0000C3FF, 0xFFFFC000000,
                        0x00000100, , , , TypeTranslation, SparseTranslation)
                    // Legacy VGA aperture + option-ROM/VBIOS window: real zx1
                    // forwards VGA (and its VBE extension) cycles to the AGP rope.
                    DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                        Cacheable, ReadWrite,
                        0, 0x000A0000, 0x000BFFFF, 0, 0x00020000)
                    DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                        Cacheable, ReadWrite,
                        0, 0x000C0000, 0x000DFFFF, 0, 0x00020000)
                    // The graphics framebuffer/MMIO/ROM live in the high MMIO
                    // aperture (FB 0xF0000000, MMIO 0xF5000000, ROM 0xF6000000).
                    QWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                        NonCacheable, ReadWrite,
                        0, 0xF0000000, 0xFDFFFFFF, 0, 0x0E000000)
                    // The Mercury CSR register block -- the "Mercury MMIO base"
                    // the HP AgpMercury miniport scans _CRS for (it aborts with
                    // "Mercury MMIO base not found in _CRS!" otherwise).  It is
                    // consumed by this bridge, not forwarded to children, so it
                    // is a ResourceConsumer descriptor.  Keep in lockstep with
                    // IA64_LBA_CSR_BASE / IA64_LBA_CSR_SIZE.
                    DWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                        NonCacheable, ReadWrite,
                        0, 0xFED10000, 0xFED10FFF, 0, 0x00001000,
                        , , , AddressRangeMemory, TypeStatic)
                    // HP CCSR vendor descriptor (guid_id 0x02, GUID 69e9adf9-...)
                    // giving the same base -- Linux hp-agp reads the LBA base
                    // from here.  Keep in lockstep with IA64_LBA_CSR_BASE.
                    VendorLong ()
                    {
                        0x02,                                           // guid_id
                        0xF9, 0xAD, 0xE9, 0x69, 0x4F, 0x92, 0x5F, 0xAB, // GUID[0:7]
                        0xF6, 0x4A, 0x24, 0xD2, 0x01, 0x37, 0x0E, 0xAD, // GUID[8:15]
                        0x00, 0x00, 0xD1, 0xFE, 0x00, 0x00, 0x00, 0x00, // base 0xFED10000
                        0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  // length 0x1000
                    }
                })
                // Graphics is device 0 on the Mercury bus; INTx uses the same
                // (slot+pin)%4 swizzle and GSIs 16..19 as PCI0.
                Name (_PRT, Package ()
                {
                    Package () { 0x0000FFFF, 0, 0x00, 16 },
                    Package () { 0x0000FFFF, 1, 0x00, 17 },
                    Package () { 0x0000FFFF, 2, 0x00, 18 },
                    Package () { 0x0000FFFF, 3, 0x00, 19 }
                })
            }
        }
    }
}
