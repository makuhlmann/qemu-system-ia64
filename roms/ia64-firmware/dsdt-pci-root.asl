// SPDX-License-Identifier: GPL-2.0-or-later
//
// Source for the DSDT AML byte array in firmware.c (mDsdt).  Recompile with
//     iasl -on -oi dsdt-pci-root.asl
// (-on suppresses the \_SB -> _SB name optimisation so the encoding stays
// byte-identical to what is shipping) and splice the AML body back in; see
// plans/runbook.md.
//
// -oi, and the 0x00 constants inside _S5 and _PRT below, are load-bearing:
// together they keep every *package element* encoded as a typed literal
// (BytePrefix/WordPrefix/DWordPrefix) rather than the ZeroOp/OneOp constant
// opcodes iasl prefers.  The ACPI CA in Linux 2.4 (20011018, what Debian 3.0's
// 2.4.17-mckinley kernel carries) types a constant opcode as
// INTERNAL_TYPE_REFERENCE, not ACPI_TYPE_INTEGER
// (acpi_ds_map_opcode_to_data_type(), dispatcher/dsutils.c), and converts it
// only while *resolving a value*.  Code that walks package elements raw never
// resolves them and so rejects the whole object:
// acpi_rs_create_pci_routing_table() fails the _PRT with AE_BAD_DATA on the
// first entry whose Pin or Source is ZeroOp, and
// acpi_hw_obtain_sleep_type_register_data() fails _S5 the same way.  A failed
// _PRT leaves Linux with *no* PCI interrupt routes: no IOSAPIC RTE is
// programmed for any PCI device, so the SCSI HBA's interrupt is never
// delivered and every command dies on a timeout.  Scalar names (_SEG, _BBN,
// _UID, _CCA) are read through acpi_evaluate_object(), which does resolve
// constants, so they are unaffected and stay as Zero/One.
//
// The root bridge _HID must be PNP0A03 (conventional PCI), not PNP0A08: some
// guest OS installers validate every ancestor device of the install disk
// against a fixed hardware-compatibility list that predates PCI Express and
// only recognizes *PNP0A03 root bridges, and a string _CID is not matched in
// its wildcard form there.  The emulated root bus is conventional PCI, so
// PNP0A03 is also the accurate identifier.

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
                // PCI0 is the PXB compatibility bus and owns bus 0 only:
                // the two WXB roots own buses 1 and 2 and the GXB root owns
                // the AGP bus 3, each declared below.
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed,
                    PosDecode, 0, 0, 0, 0, 0x0001)
                // PCI I/O space is 16 bits wide, so the producer window must
                // stop at 0xFFFF.  Declaring 16 MB here let Windows' PnP I/O
                // arbiter believe it owned 0x000000-0xFFFFFF and rebalance the
                // display adapter's I/O BAR to 0x00FFFF00 - a port number no
                // PCI device can decode and that does not exist in the IA-64
                // 64 KB I/O port space either.  The miniport then fails to map
                // its access ranges and the device stops with Code 10.
                //
                // The window is declared the way every real IA-64 root bridge
                // declares it: ports are reached through a sparse memory-
                // mapped window (4 KB page per 4 ports, SDM vol 2 10.7), so
                // the translation offset carries the window's physical base
                // (= the EfiMemoryMappedIoPortSpace descriptor, see
                // LEGACY_IO_BASE) and the type is Translation + Sparse.
                // Windows' acpi.sys builds its bridge translator windows and
                // the HAL port-range handles ((RangeId << 16) | port) from
                // exactly these fields; Linux fills io_space[] from them.
                // Two holes, both behind the GXB root: the legacy VGA ports
                // 0x3B0..0x3DF and the graphics I/O BAR at 0xC300..0xC3FF.
                // The VGA arbiter designates the root that decodes both the
                // legacy VGA I/O and the 0xA0000 aperture as the VGA owner,
                // so those must not be split across roots -- splitting them
                // fails the adapter with Code 10.
                QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    EntireRange, 0, 0, 0x000003AF, 0xFFFFC000000,
                    0x000003B0, , , , TypeTranslation, SparseTranslation)
                // The chipset routes I/O in 4 KB segments, one or more per
                // logical PCI bus, so each root owns whole segments: B is
                // the first WXB root's, D the AGP root's and E the second
                // WXB root's.  The compatibility bus keeps everything else,
                // including the legacy ports below 0x1000, the IDE
                // bus-master and USB and network ports in segment C, and the
                // SMBus controller in segment F.  A device's ports must be
                // inside its own root's producer window or the guest
                // arbiter cannot assign them.
                QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    EntireRange, 0, 0x000003E0, 0x0000AFFF, 0xFFFFC000000,
                    0x0000AC20, , , , TypeTranslation, SparseTranslation)
                QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    EntireRange, 0, 0x0000C000, 0x0000CFFF, 0xFFFFC000000,
                    0x00001000, , , , TypeTranslation, SparseTranslation)
                QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    EntireRange, 0, 0x0000F000, 0x0000FFFF, 0xFFFFC000000,
                    0x00001000, , , , TypeTranslation, SparseTranslation)
                // The legacy VGA and option-ROM apertures moved to the GXB
                // root with the graphics adapter (see GXB0 below).
                //
                // The chipset decodes one n x 32 MB aperture per logical PCI
                // bus out of the gap below 4 GB - 32 MB, so each root here
                // owns whole units of that size and none is carved out of
                // another: the compatibility bus takes the unit at the
                // bottom of the gap, which holds every BAR on it (the USB
                // controller at 0xEE010000, the AHCI at 0xEE020000, the
                // network adapter slices from 0xEE040000 and the audio
                // windows at 0xEF800000).
                QWordMemory (ResourceProducer, PosDecode, MinFixed,
                    MaxFixed, NonCacheable, ReadWrite,
                    0, 0xEE000000, 0xEFFFFFFF, 0, 0x02000000)
            })
            /*
             * The 82468GX I/O and Firmware Bridge's function 0.  The
             * platform's legacy devices are declared under it in the
             * platform SSDT, which is where firmware for a board with an
             * LPC/ISA bridge puts them: Windows' ACPI driver arbitrates a
             * device's interrupt in the pass that enumerates its parent, so
             * an IRQ-consuming device must sit inside the PCI root's subtree
             * (see the acpi-sb-irq-arbitration note -- moving them out to
             * \_SB bugchecks ACPI.sys), and under the bridge that carries
             * them is the faithful place inside it.
             */
            Device (ISA)
            {
                Name (_ADR, 0x00030000)
            }

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

        // WXB0 expander root.  It owns exactly bus 1 and its own block of four
        // Programmable Interrupt Device inputs at 20..23, so it shares no
        // interrupt line with the compatibility bus.  Its producer windows are
        // the 32 MB memory unit at 0xFA000000 and I/O segment B; the SCSI
        // adapter's BARs sit there.
        Device (WXB0)
        {
            Name (_HID, "PNP0A03")
            Name (_CID, "PNP0A03")
            Name (_SEG, Zero)
            Name (_BBN, 0x01)
            Name (_UID, 0x01)
            Name (_CCA, One)
            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed,
                    PosDecode, 0, 0x0001, 0x0001, 0, 0x0001)
                QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    EntireRange, 0, 0x0000B000, 0x0000BFFF, 0xFFFFC000000,
                    0x00001000, , , , TypeTranslation, SparseTranslation)
                QWordMemory (ResourceProducer, PosDecode, MinFixed,
                    MaxFixed, NonCacheable, ReadWrite,
                    0, 0xFA000000, 0xFBFFFFFF, 0, 0x02000000)
            })
            Name (_PRT, Package ()
            {
                    Package () { 0x0000FFFF, 0, 0x00, 20 },
                    Package () { 0x0000FFFF, 1, 0x00, 21 },
                    Package () { 0x0000FFFF, 2, 0x00, 22 },
                    Package () { 0x0000FFFF, 3, 0x00, 23 },
                    Package () { 0x0001FFFF, 0, 0x00, 21 },
                    Package () { 0x0001FFFF, 1, 0x00, 22 },
                    Package () { 0x0001FFFF, 2, 0x00, 23 },
                    Package () { 0x0001FFFF, 3, 0x00, 20 },
                    Package () { 0x0002FFFF, 0, 0x00, 22 },
                    Package () { 0x0002FFFF, 1, 0x00, 23 },
                    Package () { 0x0002FFFF, 2, 0x00, 20 },
                    Package () { 0x0002FFFF, 3, 0x00, 21 },
                    Package () { 0x0003FFFF, 0, 0x00, 23 },
                    Package () { 0x0003FFFF, 1, 0x00, 20 },
                    Package () { 0x0003FFFF, 2, 0x00, 21 },
                    Package () { 0x0003FFFF, 3, 0x00, 22 },
                    Package () { 0x0004FFFF, 0, 0x00, 20 },
                    Package () { 0x0004FFFF, 1, 0x00, 21 },
                    Package () { 0x0004FFFF, 2, 0x00, 22 },
                    Package () { 0x0004FFFF, 3, 0x00, 23 },
                    Package () { 0x0005FFFF, 0, 0x00, 21 },
                    Package () { 0x0005FFFF, 1, 0x00, 22 },
                    Package () { 0x0005FFFF, 2, 0x00, 23 },
                    Package () { 0x0005FFFF, 3, 0x00, 20 },
                    Package () { 0x0006FFFF, 0, 0x00, 22 },
                    Package () { 0x0006FFFF, 1, 0x00, 23 },
                    Package () { 0x0006FFFF, 2, 0x00, 20 },
                    Package () { 0x0006FFFF, 3, 0x00, 21 }
            })
        }

        // WXB1 expander root.  It owns exactly bus 2 and its own block of four
        // Programmable Interrupt Device inputs at 24..27, so it shares no
        // interrupt line with the compatibility bus.  Its producer windows are
        // the 32 MB memory unit at 0xFC000000 and I/O segment E; the
        // QLogic adapter's BARs sit there.
        Device (WXB1)
        {
            Name (_HID, "PNP0A03")
            Name (_CID, "PNP0A03")
            Name (_SEG, Zero)
            Name (_BBN, 0x02)
            Name (_UID, 0x02)
            Name (_CCA, One)
            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed,
                    PosDecode, 0, 0x0002, 0x0002, 0, 0x0001)
                QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    EntireRange, 0, 0x0000E000, 0x0000EFFF, 0xFFFFC000000,
                    0x00001000, , , , TypeTranslation, SparseTranslation)
                QWordMemory (ResourceProducer, PosDecode, MinFixed,
                    MaxFixed, NonCacheable, ReadWrite,
                    0, 0xFC000000, 0xFDFFFFFF, 0, 0x02000000)
            })
            Name (_PRT, Package ()
            {
                    Package () { 0x0000FFFF, 0, 0x00, 24 },
                    Package () { 0x0000FFFF, 1, 0x00, 25 },
                    Package () { 0x0000FFFF, 2, 0x00, 26 },
                    Package () { 0x0000FFFF, 3, 0x00, 27 },
                    Package () { 0x0001FFFF, 0, 0x00, 25 },
                    Package () { 0x0001FFFF, 1, 0x00, 26 },
                    Package () { 0x0001FFFF, 2, 0x00, 27 },
                    Package () { 0x0001FFFF, 3, 0x00, 24 },
                    Package () { 0x0002FFFF, 0, 0x00, 26 },
                    Package () { 0x0002FFFF, 1, 0x00, 27 },
                    Package () { 0x0002FFFF, 2, 0x00, 24 },
                    Package () { 0x0002FFFF, 3, 0x00, 25 },
                    Package () { 0x0003FFFF, 0, 0x00, 27 },
                    Package () { 0x0003FFFF, 1, 0x00, 24 },
                    Package () { 0x0003FFFF, 2, 0x00, 25 },
                    Package () { 0x0003FFFF, 3, 0x00, 26 },
                    Package () { 0x0004FFFF, 0, 0x00, 24 },
                    Package () { 0x0004FFFF, 1, 0x00, 25 },
                    Package () { 0x0004FFFF, 2, 0x00, 26 },
                    Package () { 0x0004FFFF, 3, 0x00, 27 },
                    Package () { 0x0005FFFF, 0, 0x00, 25 },
                    Package () { 0x0005FFFF, 1, 0x00, 26 },
                    Package () { 0x0005FFFF, 2, 0x00, 27 },
                    Package () { 0x0005FFFF, 3, 0x00, 24 },
                    Package () { 0x0006FFFF, 0, 0x00, 26 },
                    Package () { 0x0006FFFF, 1, 0x00, 27 },
                    Package () { 0x0006FFFF, 2, 0x00, 24 },
                    Package () { 0x0006FFFF, 3, 0x00, 25 }
            })
        }

        // GXB0 expander root.  It owns exactly bus 3 and its own block of four
        // Programmable Interrupt Device inputs at 28..31, so it shares no
        // interrupt line with the compatibility bus.  Its producer windows are
        // added when devices move onto it.
        Device (GXB0)
        {
            Name (_HID, "PNP0A03")
            Name (_CID, "PNP0A03")
            Name (_SEG, Zero)
            Name (_BBN, 0x03)
            Name (_UID, 0x03)
            Name (_CCA, One)
            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed,
                    PosDecode, 0, 0x0003, 0x0003, 0, 0x0001)
                // The graphics adapter is the VGA owner, so the GXB root
                // decodes the legacy VGA I/O and the 0xA0000 aperture
                // together.  Those ports are a sub-range of segment 0 rather
                // than a segment of their own because the chipset routes
                // them by its own VGA-enable, not by segment decode; the
                // adapter's own I/O BAR lives in segment D.
                QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    EntireRange, 0, 0x000003B0, 0x000003DF, 0xFFFFC000000,
                    0x00000030, , , , TypeTranslation, SparseTranslation)
                QWordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    EntireRange, 0, 0x0000D000, 0x0000DFFF, 0xFFFFC000000,
                    0x00001000, , , , TypeTranslation, SparseTranslation)
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    Cacheable, ReadWrite,
                    0, 0x000A0000, 0x000BFFFF, 0, 0x00020000)
                // The option-ROM segment must be a producer window too: the
                // inbox ATI miniport maps its video BIOS at 0xC0000 through
                // HalTranslateBusAddress, which pci.sys validates against the
                // root bridge windows.
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    Cacheable, ReadWrite,
                    0, 0x000C0000, 0x000DFFFF, 0, 0x00020000)
                // Five 32 MB units, enough for the largest framebuffer
                // (128 MB), register aperture and option ROM any of the
                // supported adapters presents.
                QWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0, 0xF0000000, 0xF9FFFFFF, 0, 0x0A000000)
            })
            Name (_PRT, Package ()
            {
                    Package () { 0x0000FFFF, 0, 0x00, 28 },
                    Package () { 0x0000FFFF, 1, 0x00, 29 },
                    Package () { 0x0000FFFF, 2, 0x00, 30 },
                    Package () { 0x0000FFFF, 3, 0x00, 31 },
                    Package () { 0x0001FFFF, 0, 0x00, 29 },
                    Package () { 0x0001FFFF, 1, 0x00, 30 },
                    Package () { 0x0001FFFF, 2, 0x00, 31 },
                    Package () { 0x0001FFFF, 3, 0x00, 28 },
                    Package () { 0x0002FFFF, 0, 0x00, 30 },
                    Package () { 0x0002FFFF, 1, 0x00, 31 },
                    Package () { 0x0002FFFF, 2, 0x00, 28 },
                    Package () { 0x0002FFFF, 3, 0x00, 29 },
                    Package () { 0x0003FFFF, 0, 0x00, 31 },
                    Package () { 0x0003FFFF, 1, 0x00, 28 },
                    Package () { 0x0003FFFF, 2, 0x00, 29 },
                    Package () { 0x0003FFFF, 3, 0x00, 30 },
                    Package () { 0x0004FFFF, 0, 0x00, 28 },
                    Package () { 0x0004FFFF, 1, 0x00, 29 },
                    Package () { 0x0004FFFF, 2, 0x00, 30 },
                    Package () { 0x0004FFFF, 3, 0x00, 31 },
                    Package () { 0x0005FFFF, 0, 0x00, 29 },
                    Package () { 0x0005FFFF, 1, 0x00, 30 },
                    Package () { 0x0005FFFF, 2, 0x00, 31 },
                    Package () { 0x0005FFFF, 3, 0x00, 28 },
                    Package () { 0x0006FFFF, 0, 0x00, 30 },
                    Package () { 0x0006FFFF, 1, 0x00, 31 },
                    Package () { 0x0006FFFF, 2, 0x00, 28 },
                    Package () { 0x0006FFFF, 3, 0x00, 29 }
            })
        }
    }
}
