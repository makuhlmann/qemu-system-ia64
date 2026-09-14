// SPDX-License-Identifier: GPL-2.0-or-later

DefinitionBlock ("", "SSDT", 2, "QEMU  ", "IA64SSDT", 0x00000001)
{
    External (\_SB.PCI0, DeviceObj)
    // The south bridge's LPC/ISA function, declared in the DSDT.
    External (\_SB.PCI0.ISA, DeviceObj)

    Scope (\_SB)
    {
        Name (C0EN, 0x0F)
        // Keep these as AML BytePrefix objects; firmware patches the payload.
        Name (C1EN, 0x0F)
        Name (C2EN, 0x0F)
        Name (C3EN, 0x0F)
        Name (C4EN, 0x0F)
        Name (C5EN, 0x0F)
        Name (C6EN, 0x0F)
        Name (C7EN, 0x0F)

        Processor (CPU0, 0, 0, 0)
        {
            Method (_STA, 0, NotSerialized)
            {
                Return (C0EN)
            }
        }

        Processor (CPU1, 1, 0, 0)
        {
            Method (_STA, 0, NotSerialized)
            {
                Return (C1EN)
            }
        }

        Processor (CPU2, 2, 0, 0)
        {
            Method (_STA, 0, NotSerialized)
            {
                Return (C2EN)
            }
        }

        Processor (CPU3, 3, 0, 0)
        {
            Method (_STA, 0, NotSerialized)
            {
                Return (C3EN)
            }
        }

        Processor (CPU4, 4, 0, 0)
        {
            Method (_STA, 0, NotSerialized)
            {
                Return (C4EN)
            }
        }

        Processor (CPU5, 5, 0, 0)
        {
            Method (_STA, 0, NotSerialized)
            {
                Return (C5EN)
            }
        }

        Processor (CPU6, 6, 0, 0)
        {
            Method (_STA, 0, NotSerialized)
            {
                Return (C6EN)
            }
        }

        Processor (CPU7, 7, 0, 0)
        {
            Method (_STA, 0, NotSerialized)
            {
                Return (C7EN)
            }
        }

    }

    Scope (\_SB.PCI0.ISA)
    {
        Name (P2EN, 0x0F)
        Name (U2EN, 0x0F)

        // The Super I/O's UART1: COM1 at 3F8h on ISA IRQ 4, the console.
        Device (UAR0)
        {
            Name (_HID, "PNP0501")
            /*
             * Scalar names may stay ZeroOp (resolved via
             * acpi_evaluate_object); only *package elements* must be typed
             * byte literals under -oi -- see status.md 2.3.
             */
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x03F8, 0x03F8, 8, 8)
                IRQNoFlags () {4}
            })
        }

        // UART2: COM2 at 2F8h on IRQ 3, fitted when the board has a debug
        // port (U2EN is patched by the firmware).
        Device (UAR1)
        {
            Name (_HID, "PNP0501")
            Name (_UID, One)
            Method (_STA, 0, NotSerialized)
            {
                Return (U2EN)
            }
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x02F8, 0x02F8, 8, 8)
                IRQNoFlags () {3}
            })
        }

        Device (PS2K)
        {
            Name (_HID, EisaId ("PNP0303"))
            Method (_STA, 0, NotSerialized)
            {
                Return (P2EN)
            }
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x0060, 0x0060, 1, 1)
                IO (Decode16, 0x0064, 0x0064, 1, 1)
                IRQNoFlags () {1}
            })
        }

        Device (PS2M)
        {
            Name (_HID, EisaId ("PNP0F13"))
            Method (_STA, 0, NotSerialized)
            {
                Return (P2EN)
            }
            Name (_CRS, ResourceTemplate ()
            {
                IRQNoFlags () {12}
            })
        }
    }
}
