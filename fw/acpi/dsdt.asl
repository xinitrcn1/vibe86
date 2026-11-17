/*
 * vibe86 platform: the Differentiated System Description Table.
 *
 * The board as docs/platform.md describes it, in ACPI Source Language
 * (ACPI Specification 6.5, ch. 19 "ACPI Source Language Reference"):
 * the PCI root complex (bus 0 is i440FX/PIIX compatible for PC firmware,
 * the root ports behind it), the legacy ISA devices of the platform
 * controller, QEMU's fw_cfg interface (QEMU0002), the board's own MMIO
 * devices as motherboard resources, and the S5 (soft off) sleep type the
 * PM1a control register takes. Compiled with iasl (ACPICA) into dsdt.aml;
 * fw/acpi/mktables.py builds the other tables around it.
 */
DefinitionBlock ("", "DSDT", 2, "VIBE86", "VIBE86V1", 0x00000001)
{
    Scope (\_SB)
    {
        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A08"))     // PCI Express root bridge
            Name (_CID, EisaId ("PNP0A03"))     // PCI root bridge
            Name (_UID, Zero)
            Name (_SEG, Zero)
            Name (_BBN, Zero)

            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0x0000, 0x0000, 0x00FF, 0x0000, 0x0100)
                IO (Decode16, 0x0CF8, 0x0CF8, 0x01, 0x08)
                WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange,
                    0x0000, 0x0000, 0x0CF7, 0x0000, 0x0CF8)
                WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange,
                    0x0000, 0x0D00, 0xFFFF, 0x0000, 0xF300)
                // the legacy VGA window
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, Cacheable, ReadWrite,
                    0x00000000, 0x000A0000, 0x000BFFFF, 0x00000000, 0x00020000)
                // option ROM space
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, Cacheable, ReadWrite,
                    0x00000000, 0x000C0000, 0x000DFFFF, 0x00000000, 0x00020000)
                // the 32-bit BAR window
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, NonCacheable, ReadWrite,
                    0x00000000, 0xC0000000, 0xDFFFFFFF, 0x00000000, 0x20000000)
                // the 64-bit (prefetchable) BAR window, 64 GiB at 2 TiB
                QWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, Prefetchable, ReadWrite,
                    0x0000000000000000, 0x0000020000000000, 0x0000020FFFFFFFFF,
                    0x0000000000000000, 0x0000001000000000)
            })

            // PCIe functions on this board signal with MSI; INTx isn't routed
            // (no _PRT)

            // 00:01.0: the ISA bridge and the legacy devices behind it
            Device (ISA)
            {
                Name (_ADR, 0x00010000)

                Device (PIC)
                {
                    Name (_HID, EisaId ("PNP0000"))
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0020, 0x0020, 0x01, 0x02)
                        IO (Decode16, 0x00A0, 0x00A0, 0x01, 0x02)
                        IO (Decode16, 0x04D0, 0x04D0, 0x01, 0x02)
                        IRQNoFlags () {2}
                    })
                }

                Device (TMR)
                {
                    Name (_HID, EisaId ("PNP0100"))
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0040, 0x0040, 0x01, 0x04)
                        IRQNoFlags () {0}
                    })
                }

                Device (RTC)
                {
                    Name (_HID, EisaId ("PNP0B00"))
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0070, 0x0070, 0x01, 0x02)
                        IRQNoFlags () {8}
                    })
                }

                Device (SPKR)
                {
                    Name (_HID, EisaId ("PNP0800"))
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0061, 0x0061, 0x01, 0x01)
                    })
                }

                Device (COM1)
                {
                    Name (_HID, EisaId ("PNP0501"))
                    Name (_UID, One)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x03F8, 0x03F8, 0x00, 0x08)
                        IRQNoFlags () {4}
                    })
                }

                // QEMU's firmware configuration interface (docs/specs/fw_cfg.rst)
                Device (FWCF)
                {
                    Name (_HID, "QEMU0002")
                    Name (_STA, 0x0B)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0510, 0x0510, 0x01, 0x0C)
                    })
                }

                // reserved I/O: fast A20 (92h), POST (80h), the debug console
                // (402h), the reset control register (CF9h), the PM registers
                Device (RES)
                {
                    Name (_HID, EisaId ("PNP0C02"))
                    Name (_UID, One)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0080, 0x0080, 0x01, 0x01)
                        IO (Decode16, 0x0092, 0x0092, 0x01, 0x01)
                        IO (Decode16, 0x0402, 0x0402, 0x01, 0x01)
                        IO (Decode16, 0x0600, 0x0600, 0x01, 0x0C)
                        IO (Decode16, 0x0CF9, 0x0CF9, 0x01, 0x01)
                    })
                }
            }
        }

        // the board's MMIO devices (docs/platform.md "Physical map"): UART
        // alias, SYSCON, straps, SPD SMBus, DDR5 controller, QPORT, IOMMU,
        // I2C, PMC, NVRAM and the VGA registers; the ECAM; the IOAPIC, the
        // local APICs and the flash
        Device (MBRS)
        {
            Name (_HID, EisaId ("PNP0C02"))
            Name (_UID, 0x02)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0xFE000000, 0x00020000)
                Memory32Fixed (ReadWrite, 0xE0000000, 0x10000000)
                Memory32Fixed (ReadWrite, 0xFEC00000, 0x00001000)
                Memory32Fixed (ReadWrite, 0xFEE00000, 0x00100000)
                Memory32Fixed (ReadOnly, 0xFF000000, 0x01000000)
                Memory32Fixed (ReadWrite, 0xF0000000, 0x01000000)
            })
        }
    }

    // S5 (soft off): SLP_TYP 0 in PM1a_CNT (ACPI 6.5, 7.4.2 "\_Sx")
    Name (_S5, Package (0x04) { Zero, Zero, Zero, Zero })
}
