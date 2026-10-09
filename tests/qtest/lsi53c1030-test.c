/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest cases for the LSI 53C1030 Fusion-MPT controller, ported from
 * syunnPC's tests/qtest/lsi53c1030-test.c (upstream 226527d).  Upstream runs
 * them under the libqos graph; here every case starts a zx1 machine with the
 * adapter as a -device card at 00:10.0, maps its BARs into PCI memory the
 * board leaves free, and keeps its frames and page buffers in guest RAM,
 * which the card reaches 1:1 while no OS has enabled the SBA's IOMMU.  The
 * MSI case is left out: ia64 offers no MSI yet.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_ids.h"
#include "hw/pci/pci_regs.h"
#include "hw/scsi/mpi.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "libqos/pci.h"
#include "libqos/generic-pcihost.h"
#include "libqos/libqos-malloc.h"

#define MPT_DEVFN          QPCI_DEVFN(0x10, 0)
#define MPT_MMIO_BASE      (IA64_PCI_MMIO_BASE + 0x00200000ULL)
#define MPT_MMIO_LIMIT     (IA64_PCI_MMIO_BASE + 0x01000000ULL)
#define MPT_PIO_BASE       0x6000U
#define MPT_PIO_LIMIT      0x7000U
#define MPT_DMA_BASE       0x00400000ULL
#define MPT_DMA_END        0x00800000ULL

typedef struct QMptSpi {
    QTestState *qts;
    QGenericPCIBus gbus;
    QPCIDevice dev;
    QPCIBar bar;
} QMptSpi;

typedef struct MptSpiSnapshotData {
    char *tmpdir;
    char *disk_path;
} MptSpiSnapshotData;

typedef void MptSpiTest(void *obj, void *data, QGuestAllocator *alloc);

typedef struct MptSpiCase {
    const char *name;
    MptSpiTest *fn;
    bool snapshot;
} MptSpiCase;

static uint32_t mptspi_probe_rom_size(QPCIDevice *dev)
{
    uint32_t saved_rom = qpci_config_readl(dev, PCI_ROM_ADDRESS);
    uint32_t rom_mask;

    qpci_config_writel(dev, PCI_ROM_ADDRESS, UINT32_MAX);
    rom_mask = qpci_config_readl(dev, PCI_ROM_ADDRESS);
    qpci_config_writel(dev, PCI_ROM_ADDRESS, saved_rom);

    rom_mask &= PCI_ROM_ADDRESS_MASK;
    return rom_mask ? ~rom_mask + 1 : 0;
}

static void mptspi_handshake(QMptSpi *mpt, const void *request,
                             size_t request_size, void *reply,
                             size_t reply_size)
{
    const uint8_t *request_bytes = request;
    uint8_t *reply_bytes = reply;
    size_t i;

    g_assert_cmpuint(request_size % sizeof(uint32_t), ==, 0);
    g_assert_cmpuint(reply_size % sizeof(uint16_t), ==, 0);

    qpci_io_writel(&mpt->dev, mpt->bar, MPI_DOORBELL_OFFSET,
                   (MPI_FUNCTION_HANDSHAKE << MPI_DOORBELL_FUNCTION_SHIFT) |
                   ((request_size / sizeof(uint32_t)) <<
                    MPI_DOORBELL_ADD_DWORDS_SHIFT));
    g_assert_cmphex(qpci_io_readl(&mpt->dev, mpt->bar,
                                  MPI_HOST_INTERRUPT_STATUS_OFFSET) &
                    MPI_HIS_DOORBELL_INTERRUPT, ==,
                    MPI_HIS_DOORBELL_INTERRUPT);
    qpci_io_writel(&mpt->dev, mpt->bar,
                   MPI_HOST_INTERRUPT_STATUS_OFFSET, 0);

    for (i = 0; i < request_size; i += sizeof(uint32_t)) {
        qpci_io_writel(&mpt->dev, mpt->bar, MPI_DOORBELL_OFFSET,
                       ldl_le_p(request_bytes + i));
    }

    g_assert_cmphex(qpci_io_readl(&mpt->dev, mpt->bar,
                                  MPI_HOST_INTERRUPT_STATUS_OFFSET) &
                    MPI_HIS_DOORBELL_INTERRUPT, ==,
                    MPI_HIS_DOORBELL_INTERRUPT);
    for (i = 0; i < reply_size; i += sizeof(uint16_t)) {
        stw_le_p(reply_bytes + i,
                 qpci_io_readl(&mpt->dev, mpt->bar,
                               MPI_DOORBELL_OFFSET));
        qpci_io_writel(&mpt->dev, mpt->bar,
                       MPI_HOST_INTERRUPT_STATUS_OFFSET, 0);
    }
}

static void mptspi_ioc_init(QMptSpi *mpt)
{
    MPIMsgIOCInit request = { 0 };
    MPIMsgIOCInitReply reply;

    request.WhoInit = MPI_WHOINIT_HOST_DRIVER;
    request.Function = MPI_FUNCTION_IOC_INIT;
    request.MaxDevices = 16;
    request.MaxBuses = 1;
    request.ReplyFrameSize = cpu_to_le16(80);
    request.MsgVersion = cpu_to_le16(0x0105);
    mptspi_handshake(mpt, &request, sizeof(request), &reply, sizeof(reply));
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    g_assert_cmphex(qpci_io_readl(&mpt->dev, mpt->bar,
                                  MPI_DOORBELL_OFFSET) &
                    MPI_IOC_STATE_OPERATIONAL, ==,
                    MPI_IOC_STATE_OPERATIONAL);
}

static MPIMsgConfigReply mptspi_config(QMptSpi *mpt, uint8_t action,
                                       uint8_t page_type, uint8_t page_number,
                                       uint32_t page_address,
                                       uint64_t buffer_address,
                                       uint32_t buffer_length, bool write)
{
    MPIMsgConfig request = { 0 };
    MPIMsgConfigReply reply;
    uint32_t flags = MPI_SGE_FLAGS_SIMPLE_ELEMENT |
                     MPI_SGE_FLAGS_LAST_ELEMENT |
                     MPI_SGE_FLAGS_END_OF_BUFFER |
                     MPI_SGE_FLAGS_END_OF_LIST;

    g_assert_cmpuint(buffer_address, <=, UINT32_MAX);
    request.Action = action;
    request.Function = MPI_FUNCTION_CONFIG;
    request.PageNumber = page_number;
    request.PageType = page_type;
    request.PageAddress = cpu_to_le32(page_address);
    if (write) {
        flags |= MPI_SGE_FLAGS_HOST_TO_IOC;
    }
    request.PageBufferSGE.FlagsLength =
        cpu_to_le32(flags | buffer_length);
    request.PageBufferSGE.u.Address32 = cpu_to_le32(buffer_address);

    mptspi_handshake(mpt, &request, sizeof(request), &reply, sizeof(reply));
    return reply;
}

static void mptspi_test_io_unit_policy(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    QMptSpi *mpt = obj;
    uint8_t page[8];
    uint64_t pa = guest_alloc(alloc, sizeof(page));
    MPIMsgConfigReply reply;
    uint32_t flags;
    const uint8_t actions[][2] = {
        { MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
          MPI_CONFIG_ACTION_PAGE_READ_CURRENT },
        { MPI_CONFIG_ACTION_PAGE_WRITE_NVRAM,
          MPI_CONFIG_ACTION_PAGE_READ_NVRAM },
    };
    unsigned i;

    mptspi_ioc_init(mpt);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_IO_UNIT, 1, 0,
                          pa, sizeof(page), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, pa, page, sizeof(page));

    /* Static volume ID policy is valid without RAID volumes. */
    flags = ldl_le_p(page + 4) | MPI_IOUNITPAGE1_IR_USE_STATIC_VOLUME_ID;
    for (i = 0; i < G_N_ELEMENTS(actions); i++) {
        stl_le_p(page + 4, flags);
        qtest_memwrite(mpt->dev.bus->qts, pa, page, sizeof(page));
        reply = mptspi_config(mpt, actions[i][0],
                              MPI_CONFIG_PAGETYPE_IO_UNIT, 1, 0,
                              pa, sizeof(page), true);
        g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                        MPI_IOCSTATUS_SUCCESS);
        qtest_memset(mpt->dev.bus->qts, pa, 0, sizeof(page));
        reply = mptspi_config(mpt, actions[i][1],
                              MPI_CONFIG_PAGETYPE_IO_UNIT, 1, 0,
                              pa, sizeof(page), false);
        g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                        MPI_IOCSTATUS_SUCCESS);
        qtest_memread(mpt->dev.bus->qts, pa, page, sizeof(page));
        g_assert_cmphex(ldl_le_p(page + 4), ==, flags);
    }

    /* The policy must not make the controller's function layout writable. */
    stl_le_p(page + 4, flags ^ MPI_IOUNITPAGE1_SINGLE_FUNCTION);
    qtest_memwrite(mpt->dev.bus->qts, pa, page, sizeof(page));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_IO_UNIT, 1, 0,
                          pa, sizeof(page), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_CONFIG_INVALID_DATA);
    guest_free(alloc, pa);
}

static void mptspi_test_facts(void *obj, void *data,
                              QGuestAllocator *alloc)
{
    QMptSpi *mpt = obj;
    QPCIBar diag_bar;
    MPIMsgIOCFacts facts_request = { 0 };
    MPIMsgIOCFactsReply facts_reply;
    MPIMsgPortFacts port_request = { 0 };
    MPIMsgPortFactsReply port_reply;
    uint16_t product_id = MPI_FW_HEADER_PID_TYPE_SCSI |
                          MPI_FW_HEADER_PID_PROD_INITIATOR_SCSI |
                          MPI_FW_HEADER_PID_FAMILY_1030C0_SCSI;
    uint64_t diag_size;

    g_assert_cmphex(qpci_config_readw(&mpt->dev, PCI_VENDOR_ID), ==,
                    PCI_VENDOR_ID_LSI_LOGIC);
    g_assert_cmphex(qpci_config_readw(&mpt->dev, PCI_DEVICE_ID), ==,
                    PCI_DEVICE_ID_LSI_53C1030);
    g_assert_cmphex(qpci_config_readb(&mpt->dev, PCI_REVISION_ID), ==, 0x07);
    g_assert_cmphex(qpci_config_readw(&mpt->dev,
                                      PCI_SUBSYSTEM_VENDOR_ID), ==,
                    PCI_VENDOR_ID_LSI_LOGIC);
    g_assert_cmphex(qpci_config_readw(&mpt->dev, PCI_SUBSYSTEM_ID), ==,
                    0x1000);
    g_assert_cmphex(qpci_config_readl(&mpt->dev, PCI_BASE_ADDRESS_1) &
                    PCI_BASE_ADDRESS_MEM_TYPE_MASK, ==,
                    PCI_BASE_ADDRESS_MEM_TYPE_64);
    g_assert_cmphex(qpci_config_readl(&mpt->dev, PCI_BASE_ADDRESS_3) &
                    PCI_BASE_ADDRESS_MEM_TYPE_MASK, ==,
                    PCI_BASE_ADDRESS_MEM_TYPE_64);
    diag_bar = qpci_iomap(&mpt->dev, 3, &diag_size);
    g_assert_cmpuint(diag_size, ==, 0x10000);
    qpci_iounmap(&mpt->dev, diag_bar);
    g_assert_cmpuint(mptspi_probe_rom_size(&mpt->dev), ==, 0);
    g_assert_cmphex(qpci_io_readl(&mpt->dev, mpt->bar,
                                  MPI_DOORBELL_OFFSET) &
                    MPI_IOC_STATE_READY, ==, MPI_IOC_STATE_READY);

    facts_request.Function = MPI_FUNCTION_IOC_FACTS;
    mptspi_handshake(mpt, &facts_request, sizeof(facts_request),
                     &facts_reply, sizeof(facts_reply));
    g_assert_cmphex(le16_to_cpu(facts_reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    g_assert_cmphex(le16_to_cpu(facts_reply.ProductID), ==, product_id);
    g_assert_cmpuint(facts_reply.NumberOfPorts, ==, 1);
    g_assert_cmpuint(facts_reply.MaxDevices, ==, 16);
    g_assert_cmpuint(facts_reply.MaxBuses, ==, 1);
    /* The rx2600's firmware: FwRev=01032300h, MaxQ=255 under mptbase. */
    g_assert_cmpuint(facts_reply.FWVersionMajor, ==, 0x01);
    g_assert_cmpuint(facts_reply.FWVersionMinor, ==, 0x03);
    g_assert_cmpuint(facts_reply.FWVersionUnit, ==, 0x23);
    g_assert_cmpuint(facts_reply.FWVersionDev, ==, 0x00);
    g_assert_cmpuint(le16_to_cpu(facts_reply.GlobalCredits), ==, 255);

    port_request.Function = MPI_FUNCTION_PORT_FACTS;
    port_request.PortNumber = 0;
    mptspi_handshake(mpt, &port_request, sizeof(port_request),
                     &port_reply, sizeof(port_reply));
    g_assert_cmphex(le16_to_cpu(port_reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    g_assert_cmphex(port_reply.PortType, ==, MPI_PORTFACTS_PORTTYPE_SCSI);
    g_assert_cmpuint(le16_to_cpu(port_reply.MaxDevices), ==, 16);
    g_assert_cmpuint(le16_to_cpu(port_reply.PortSCSIID), ==, 7);
    g_assert_cmphex(le16_to_cpu(port_reply.ProtocolFlags), ==,
                    MPI_PORTFACTS_PROTOCOL_INITIATOR);

    /* IOC page 0 repeats the function's header: a 24-bit ClassCode. */
    {
        uint64_t pa = guest_alloc(alloc, 28);
        MPIMsgConfigReply reply;
        uint8_t page[28];

        mptspi_ioc_init(mpt);
        reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                              MPI_CONFIG_PAGETYPE_IOC, 0, 0, pa,
                              sizeof(page), false);
        g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                        MPI_IOCSTATUS_SUCCESS);
        qtest_memread(mpt->dev.bus->qts, pa, page, sizeof(page));
        g_assert_cmphex(lduw_le_p(page + 12), ==, PCI_VENDOR_ID_LSI_LOGIC);
        g_assert_cmphex(lduw_le_p(page + 14), ==, PCI_DEVICE_ID_LSI_53C1030);
        g_assert_cmphex(page[16], ==, 0x07);
        g_assert_cmphex(ldl_le_p(page + 20), ==, 0x010000);
        g_assert_cmphex(ldl_le_p(page + 24), ==, 0x10001000);
        guest_free(alloc, pa);
    }
}

static void mptspi_test_config_and_reset(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    QMptSpi *mpt = obj;
    MPIMsgConfigReply reply;
    uint8_t port_page[12];
    uint8_t port_page_1[16] = { 0 };
    uint8_t port_page_2[76];
    uint8_t io_unit_page_1[8];
    uint8_t ioc_page_1[16] = { 0 };
    uint8_t device_page_0[12];
    uint8_t device_page_1[16] = { 0 };
    uint64_t page_address;
    uint32_t capabilities;
    const uint32_t requested_params = 0x20ff0807;
    const uint8_t diag_keys[] = { 0x04, 0x0b, 0x02, 0x07, 0x0d };
    int i;

    mptspi_ioc_init(mpt);
    page_address = guest_alloc(alloc, 76);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_HEADER,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 0, 0, 0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    g_assert_cmpuint(reply.PageVersion, ==, 0x02);
    g_assert_cmpuint(reply.PageLength, ==, sizeof(port_page) / 4);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 0, 0,
                          page_address, sizeof(port_page), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page,
                  sizeof(port_page));
    capabilities = ldl_le_p(port_page + 4);
    g_assert_cmphex(capabilities & MPI_SCSIPORTPAGE0_CAP_WIDE, ==,
                    MPI_SCSIPORTPAGE0_CAP_WIDE);
    g_assert_cmphex(ldl_le_p(port_page + 8), ==,
                    MPI_SCSIPORTPAGE0_PHY_SIGNAL_LVD);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_HEADER,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    g_assert_cmpuint(reply.PageVersion, ==, 0x03);
    g_assert_cmpuint(reply.PageLength, ==, sizeof(port_page_1) / 4);
    g_assert_cmphex(reply.PageType, ==,
                    MPI_CONFIG_PAGEATTR_PERSISTENT |
                    MPI_CONFIG_PAGETYPE_SCSI_PORT);

    port_page_1[0] = 0x03;
    port_page_1[1] = sizeof(port_page_1) / 4;
    port_page_1[2] = 1;
    port_page_1[3] = MPI_CONFIG_PAGEATTR_CHANGEABLE |
                     MPI_CONFIG_PAGETYPE_SCSI_PORT;
    stl_le_p(port_page_1 + 4,
             6 |
             (1U << (6 +
                     MPI_SCSIPORTPAGE1_CFG_SHIFT_PORT_RESPONSE_ID)));
    qtest_memwrite(mpt->dev.bus->qts, page_address, port_page_1,
                   sizeof(port_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);

    memset(port_page_1, 0, sizeof(port_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_1,
                  sizeof(port_page_1));
    g_assert_cmphex(ldl_le_p(port_page_1 + 4), ==, 0x00400006);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_DEFAULT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_1,
                  sizeof(port_page_1));
    g_assert_cmphex(ldl_le_p(port_page_1 + 4), ==, 0x00800007);

    port_page_1[12] = 1;
    qtest_memwrite(mpt->dev.bus->qts, page_address, port_page_1,
                   sizeof(port_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_CONFIG_INVALID_DATA);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_1,
                  sizeof(port_page_1));
    g_assert_cmphex(ldl_le_p(port_page_1 + 4), ==, 0x00400006);

    port_page_1[13] = 1;
    qtest_memwrite(mpt->dev.bus->qts, page_address, port_page_1,
                   sizeof(port_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_CONFIG_INVALID_DATA);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_DEFAULT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_1,
                  sizeof(port_page_1));
    g_assert_cmphex(ldl_le_p(port_page_1 + 4), ==, 0x00800007);
    g_assert_cmpuint(ldl_le_p(port_page_1 + 8), ==, 0);

    stl_le_p(port_page_1 + 4, 0x00400006);
    stl_le_p(port_page_1 + 8, 0x12345678);
    qtest_memwrite(mpt->dev.bus->qts, page_address, port_page_1,
                   sizeof(port_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_HEADER,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    g_assert_cmpuint(reply.PageVersion, ==, 0x02);
    g_assert_cmpuint(reply.PageLength, ==, sizeof(port_page_2) / 4);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_NVRAM,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_2,
                  sizeof(port_page_2));
    g_assert_cmphex(ldl_le_p(port_page_2 + 8), ==,
                    MPI_SCSIPORTPAGE2_PORT_BIOS_OS_INIT_HBA | 7);
    g_assert_cmpuint(port_page_2[13], ==, 0x08);
    g_assert_cmphex(lduw_le_p(port_page_2 + 14), ==, 0x000f);
    g_assert_cmpuint(port_page_2[73], ==, 0x08);
    g_assert_cmphex(lduw_le_p(port_page_2 + 74), ==, 0x000f);

    stw_le_p(port_page_2 + 14, 0x0007);
    qtest_memwrite(mpt->dev.bus->qts, page_address, port_page_2,
                   sizeof(port_page_2));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_2,
                  sizeof(port_page_2));
    g_assert_cmphex(lduw_le_p(port_page_2 + 14), ==, 0x0007);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_NVRAM,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2) - 1, true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_CONFIG_INVALID_DATA);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_NVRAM,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_2,
                  sizeof(port_page_2));
    g_assert_cmphex(lduw_le_p(port_page_2 + 14), ==, 0x0007);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_DEFAULT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0, 0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_2,
                  sizeof(port_page_2));
    g_assert_cmphex(lduw_le_p(port_page_2 + 14), ==, 0x000f);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_IO_UNIT, 1, 0,
                          page_address, sizeof(io_unit_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, io_unit_page_1,
                  sizeof(io_unit_page_1));
    g_assert_cmphex(ldl_le_p(io_unit_page_1 + 4), ==,
                    MPI_IOUNITPAGE1_MULTI_FUNCTION |
                    MPI_IOUNITPAGE1_DISABLE_IR);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_HEADER,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, 0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    g_assert_cmpuint(reply.PageVersion, ==, 0x03);
    g_assert_cmpuint(reply.PageLength, ==, sizeof(ioc_page_1) / 4);
    g_assert_cmphex(reply.PageType, ==,
                    MPI_CONFIG_PAGEATTR_CHANGEABLE |
                    MPI_CONFIG_PAGETYPE_IOC);

    ioc_page_1[0] = 0x03;
    ioc_page_1[1] = sizeof(ioc_page_1) / 4;
    ioc_page_1[2] = 1;
    ioc_page_1[3] = MPI_CONFIG_PAGEATTR_CHANGEABLE |
                    MPI_CONFIG_PAGETYPE_IOC;
    stl_le_p(ioc_page_1 + 4, MPI_IOCPAGE1_REPLY_COALESCING);
    stl_le_p(ioc_page_1 + 8, 10);
    ioc_page_1[12] = 4;
    qtest_memwrite(mpt->dev.bus->qts, page_address, ioc_page_1,
                   sizeof(ioc_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, page_address,
                          sizeof(ioc_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);

    memset(ioc_page_1, 0, sizeof(ioc_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, page_address,
                          sizeof(ioc_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, ioc_page_1,
                  sizeof(ioc_page_1));
    g_assert_cmphex(ldl_le_p(ioc_page_1 + 4), ==,
                    MPI_IOCPAGE1_REPLY_COALESCING);
    g_assert_cmpuint(ldl_le_p(ioc_page_1 + 8), ==, 10);
    g_assert_cmpuint(ioc_page_1[12], ==, 4);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_DEFAULT,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, page_address,
                          sizeof(ioc_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, ioc_page_1,
                  sizeof(ioc_page_1));
    g_assert_cmpuint(ldl_le_p(ioc_page_1 + 4), ==, 0);
    g_assert_cmpuint(ldl_le_p(ioc_page_1 + 8), ==, 0);
    g_assert_cmpuint(ioc_page_1[12], ==, 0);

    stl_le_p(ioc_page_1 + 4,
             MPI_IOCPAGE1_INITIATOR_CONTEXT_REPLY_DISABLE);
    qtest_memwrite(mpt->dev.bus->qts, page_address, ioc_page_1,
                   sizeof(ioc_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, page_address,
                          sizeof(ioc_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_CONFIG_INVALID_DATA);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, page_address,
                          sizeof(ioc_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, ioc_page_1,
                  sizeof(ioc_page_1));
    g_assert_cmphex(ldl_le_p(ioc_page_1 + 4), ==,
                    MPI_IOCPAGE1_REPLY_COALESCING);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_DEFAULT,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, 0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, page_address,
                          sizeof(ioc_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, ioc_page_1,
                  sizeof(ioc_page_1));
    g_assert_cmpuint(ldl_le_p(ioc_page_1 + 4), ==, 0);
    g_assert_cmpuint(ldl_le_p(ioc_page_1 + 8), ==, 0);
    g_assert_cmpuint(ioc_page_1[12], ==, 0);

    stl_le_p(ioc_page_1 + 4, MPI_IOCPAGE1_REPLY_COALESCING);
    stl_le_p(ioc_page_1 + 8, 10);
    ioc_page_1[12] = 4;
    qtest_memwrite(mpt->dev.bus->qts, page_address, ioc_page_1,
                   sizeof(ioc_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, page_address,
                          sizeof(ioc_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_HEADER,
                          MPI_CONFIG_PAGETYPE_IOC, 4, 0, 0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    g_assert_cmpuint(reply.PageLength, ==, 2);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_DEFAULT,
                          MPI_CONFIG_PAGETYPE_IOC, 4, 0, 0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_CONFIG_CANT_COMMIT);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_HEADER,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 1, 0,
                          0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    g_assert_cmpuint(reply.PageVersion, ==, 0x05);
    g_assert_cmpuint(reply.PageLength, ==, sizeof(device_page_1) / 4);

    device_page_1[0] = 0x05;
    device_page_1[1] = sizeof(device_page_1) / 4;
    device_page_1[2] = 1;
    device_page_1[3] = MPI_CONFIG_PAGEATTR_CHANGEABLE |
                       MPI_CONFIG_PAGETYPE_SCSI_DEVICE;
    stl_le_p(device_page_1 + 4, requested_params);
    stl_le_p(device_page_1 + 12, 0x08);
    qtest_memwrite(mpt->dev.bus->qts, page_address, device_page_1,
                   sizeof(device_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 1, 2,
                          page_address, sizeof(device_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 0, 2,
                          page_address, sizeof(device_page_0), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, device_page_0,
                  sizeof(device_page_0));
    g_assert_cmphex(ldl_le_p(device_page_0 + 4), ==, requested_params);

    stl_le_p(device_page_1 + 8, 1);
    qtest_memwrite(mpt->dev.bus->qts, page_address, device_page_1,
                   sizeof(device_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 1, 2,
                          page_address, sizeof(device_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_CONFIG_INVALID_DATA);
    stl_le_p(device_page_1 + 8, 0);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 1, 0x00010002,
                          page_address, sizeof(device_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_CONFIG_INVALID_PAGE);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_DEFAULT,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 1, 2,
                          page_address, sizeof(device_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, device_page_1,
                  sizeof(device_page_1));
    g_assert_cmpuint(ldl_le_p(device_page_1 + 4), ==, 0);
    g_assert_cmpuint(ldl_le_p(device_page_1 + 12), ==, 0);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_DEFAULT,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 1, 2,
                          0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 0, 2,
                          page_address, sizeof(device_page_0), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, device_page_0,
                  sizeof(device_page_0));
    g_assert_cmpuint(ldl_le_p(device_page_0 + 4), ==, 0);

    memset(device_page_1, 0, sizeof(device_page_1));
    device_page_1[0] = 0x05;
    device_page_1[1] = sizeof(device_page_1) / 4;
    device_page_1[2] = 1;
    device_page_1[3] = MPI_CONFIG_PAGEATTR_CHANGEABLE |
                       MPI_CONFIG_PAGETYPE_SCSI_DEVICE;
    stl_le_p(device_page_1 + 4, requested_params);
    stl_le_p(device_page_1 + 12, 0x08);
    qtest_memwrite(mpt->dev.bus->qts, page_address, device_page_1,
                   sizeof(device_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 1, 2,
                          page_address, sizeof(device_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);

    for (i = 0; i < ARRAY_SIZE(diag_keys); i++) {
        qpci_io_writel(&mpt->dev, mpt->bar, MPI_WRITE_SEQUENCE_OFFSET,
                       diag_keys[i]);
    }
    g_assert_cmphex(qpci_io_readl(&mpt->dev, mpt->bar,
                                  MPI_DIAGNOSTIC_OFFSET) & MPI_DIAG_DRWE,
                    ==, MPI_DIAG_DRWE);
    qpci_io_writel(&mpt->dev, mpt->bar, MPI_DIAGNOSTIC_OFFSET,
                   MPI_DIAG_RESET_ADAPTER);
    g_assert_cmphex(qpci_io_readl(&mpt->dev, mpt->bar,
                                  MPI_DOORBELL_OFFSET) &
                    MPI_IOC_STATE_READY, ==, MPI_IOC_STATE_READY);
    g_assert_cmphex(qpci_io_readl(&mpt->dev, mpt->bar,
                                  MPI_HOST_INTERRUPT_MASK_OFFSET), ==,
                    MPI_HIM_DIM | MPI_HIM_RIM);
    g_assert_cmphex(qpci_io_readl(&mpt->dev, mpt->bar,
                                  MPI_DIAGNOSTIC_OFFSET), ==, 0);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 0, 2,
                          page_address, sizeof(device_page_0), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, device_page_0,
                  sizeof(device_page_0));
    g_assert_cmphex(ldl_le_p(device_page_0 + 4), ==, 0);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, page_address,
                          sizeof(ioc_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, ioc_page_1,
                  sizeof(ioc_page_1));
    g_assert_cmpuint(ldl_le_p(ioc_page_1 + 4), ==, 0);
    g_assert_cmpuint(ldl_le_p(ioc_page_1 + 8), ==, 0);
    g_assert_cmpuint(ioc_page_1[12], ==, 0);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_1,
                  sizeof(port_page_1));
    g_assert_cmphex(ldl_le_p(port_page_1 + 4), ==, 0x00800007);

    guest_free(alloc, page_address);
}

static void mptspi_snapshot_data_free(void *opaque)
{
    MptSpiSnapshotData *snapshot = opaque;

    g_assert_cmpint(g_unlink(snapshot->disk_path), ==, 0);
    g_assert_cmpint(g_rmdir(snapshot->tmpdir), ==, 0);
    g_free(snapshot->disk_path);
    g_free(snapshot->tmpdir);
    g_free(snapshot);
}

static void *mptspi_snapshot_setup(GString *cmd_line, void *arg)
{
    g_autofree char *quoted_disk_path = NULL;
    g_autoptr(GError) error = NULL;
    MptSpiSnapshotData *snapshot;

    if (!have_qemu_img()) {
        return NULL;
    }

    snapshot = g_new0(MptSpiSnapshotData, 1);
    snapshot->tmpdir = g_dir_make_tmp("lsi53c1030-savevm-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(snapshot->tmpdir);
    snapshot->disk_path = g_build_filename(snapshot->tmpdir,
                                            "snapshot.qcow2", NULL);
    g_assert_true(mkimg(snapshot->disk_path, "qcow2", 16));
    quoted_disk_path = g_shell_quote(snapshot->disk_path);
    g_string_append_printf(cmd_line,
                           " -drive file=%s,format=qcow2,if=none,id=snapshot",
                           quoted_disk_path);
    g_test_queue_destroy(mptspi_snapshot_data_free, snapshot);
    return snapshot;
}

static void mptspi_test_config_savevm(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    const uint8_t diag_keys[] = { 0x04, 0x0b, 0x02, 0x07, 0x0d };
    QMptSpi *mpt = obj;
    MPIMsgConfigReply reply;
    uint8_t device_page_1[16] = { 0 };
    uint8_t ioc_page_1[16] = { 0 };
    uint8_t port_page_1[16] = { 0 };
    uint8_t port_page_2[76];
    uint64_t page_address;
    g_autofree char *response = NULL;

    if (!data) {
        g_test_skip("qemu-img is required for config-page savevm testing");
        return;
    }

    mptspi_ioc_init(mpt);
    page_address = guest_alloc(alloc, sizeof(port_page_2));

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_2,
                  sizeof(port_page_2));
    stw_le_p(port_page_2 + 14, 0x0007);
    qtest_memwrite(mpt->dev.bus->qts, page_address, port_page_2,
                   sizeof(port_page_2));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);

    stw_le_p(port_page_2 + 14, 0x0005);
    qtest_memwrite(mpt->dev.bus->qts, page_address, port_page_2,
                  sizeof(port_page_2));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_NVRAM,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);

    ioc_page_1[0] = 0x03;
    ioc_page_1[1] = sizeof(ioc_page_1) / 4;
    ioc_page_1[2] = 1;
    ioc_page_1[3] = MPI_CONFIG_PAGEATTR_CHANGEABLE |
                    MPI_CONFIG_PAGETYPE_IOC;
    stl_le_p(ioc_page_1 + 4, MPI_IOCPAGE1_REPLY_COALESCING);
    stl_le_p(ioc_page_1 + 8, 0x10203040);
    ioc_page_1[12] = 0x20;
    qtest_memwrite(mpt->dev.bus->qts, page_address, ioc_page_1,
                   sizeof(ioc_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, page_address,
                          sizeof(ioc_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);

    port_page_1[0] = 0x03;
    port_page_1[1] = sizeof(port_page_1) / 4;
    port_page_1[2] = 1;
    port_page_1[3] = MPI_CONFIG_PAGEATTR_PERSISTENT |
                     MPI_CONFIG_PAGETYPE_SCSI_PORT;
    stl_le_p(port_page_1 + 4, 0x00400006);
    stl_le_p(port_page_1 + 8, 0x50607080);
    qtest_memwrite(mpt->dev.bus->qts, page_address, port_page_1,
                   sizeof(port_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);

    stl_le_p(port_page_1 + 4, 0x00200005);
    stl_le_p(port_page_1 + 8, 0x70abcdef);
    qtest_memwrite(mpt->dev.bus->qts, page_address, port_page_1,
                  sizeof(port_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_NVRAM,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);

    device_page_1[0] = 0x05;
    device_page_1[1] = sizeof(device_page_1) / 4;
    device_page_1[2] = 1;
    device_page_1[3] = MPI_CONFIG_PAGEATTR_CHANGEABLE |
                       MPI_CONFIG_PAGETYPE_SCSI_DEVICE;
    stl_le_p(device_page_1 + 4, 0x20ff0807);
    stl_le_p(device_page_1 + 12, 0x08);
    qtest_memwrite(mpt->dev.bus->qts, page_address, device_page_1,
                   sizeof(device_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 1, 2,
                          page_address, sizeof(device_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);

    response = qtest_hmp(mpt->dev.bus->qts, "savevm config-pages");
    g_assert_cmpstr(response, ==, "");
    g_clear_pointer(&response, g_free);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_DEFAULT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_DEFAULT,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, 0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_DEFAULT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_DEFAULT,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 1, 2,
                          0, 0, false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);

    stw_le_p(port_page_2 + 14, 0x0003);
    qtest_memwrite(mpt->dev.bus->qts, page_address, port_page_2,
                  sizeof(port_page_2));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_NVRAM,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    stl_le_p(port_page_1 + 4, 0x00100004);
    qtest_memwrite(mpt->dev.bus->qts, page_address, port_page_1,
                  sizeof(port_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_NVRAM,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), true);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);

    response = qtest_hmp(mpt->dev.bus->qts, "loadvm config-pages");
    g_assert_cmpstr(response, ==, "");

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_2,
                  sizeof(port_page_2));
    g_assert_cmphex(lduw_le_p(port_page_2 + 14), ==, 0x0007);
    memset(ioc_page_1, 0, sizeof(ioc_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_IOC, 1, 0, page_address,
                          sizeof(ioc_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, ioc_page_1,
                  sizeof(ioc_page_1));
    g_assert_cmphex(ldl_le_p(ioc_page_1 + 4), ==,
                    MPI_IOCPAGE1_REPLY_COALESCING);
    g_assert_cmphex(ldl_le_p(ioc_page_1 + 8), ==, 0x10203040);
    g_assert_cmphex(ioc_page_1[12], ==, 0x20);

    memset(port_page_1, 0, sizeof(port_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_1,
                  sizeof(port_page_1));
    g_assert_cmphex(ldl_le_p(port_page_1 + 4), ==, 0x00400006);
    g_assert_cmphex(ldl_le_p(port_page_1 + 8), ==, 0x50607080);

    memset(device_page_1, 0, sizeof(device_page_1));
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_DEVICE, 1, 2,
                          page_address, sizeof(device_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==,
                    MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, device_page_1,
                  sizeof(device_page_1));
    g_assert_cmphex(ldl_le_p(device_page_1 + 4), ==, 0x20ff0807);
    g_assert_cmphex(ldl_le_p(device_page_1 + 12), ==, 0x08);

    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_NVRAM,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_1,
                 sizeof(port_page_1));
    g_assert_cmphex(ldl_le_p(port_page_1 + 4), ==, 0x00200005);
    g_assert_cmphex(ldl_le_p(port_page_1 + 8), ==, 0x70abcdef);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_NVRAM,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_2,
                 sizeof(port_page_2));
    g_assert_cmphex(lduw_le_p(port_page_2 + 14), ==, 0x0005);

    for (unsigned int i = 0; i < ARRAY_SIZE(diag_keys); i++) {
        qpci_io_writel(&mpt->dev, mpt->bar, MPI_WRITE_SEQUENCE_OFFSET,
                      diag_keys[i]);
    }
    qpci_io_writel(&mpt->dev, mpt->bar, MPI_DIAGNOSTIC_OFFSET,
                  MPI_DIAG_RESET_ADAPTER);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 1, 0,
                          page_address, sizeof(port_page_1), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_1,
                 sizeof(port_page_1));
    g_assert_cmphex(ldl_le_p(port_page_1 + 4), ==, 0x00200005);
    g_assert_cmphex(ldl_le_p(port_page_1 + 8), ==, 0x70abcdef);
    reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                          MPI_CONFIG_PAGETYPE_SCSI_PORT, 2, 0,
                          page_address, sizeof(port_page_2), false);
    g_assert_cmphex(le16_to_cpu(reply.IOCStatus), ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(mpt->dev.bus->qts, page_address, port_page_2,
                 sizeof(port_page_2));
    g_assert_cmphex(lduw_le_p(port_page_2 + 14), ==, 0x0005);

    guest_free(alloc, page_address);
}

static void mptspi_reply_coalescing(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    QMptSpi *mpt = obj;
    QPCIDevice *dev = &mpt->dev;
    QTestState *qts = dev->bus->qts;
    uint64_t msi_address = guest_alloc(alloc, 16);
    uint8_t msi_cap = qpci_find_capability(dev, PCI_CAP_ID_MSI, 0);
    uint16_t msi_control = qpci_config_readw(dev, msi_cap + PCI_MSI_FLAGS);
    uint64_t page = guest_alloc(alloc, 16);
    uint64_t request = guest_alloc(alloc, 128);
    uint64_t reply_address = guest_alloc(alloc, 256);
    uint8_t ioc_page[16] = { 0 };
    MPIMsgIOCFacts facts = { .Function = MPI_FUNCTION_IOC_FACTS };
    MPIMsgConfigReply config_reply;

    mptspi_ioc_init(mpt);
    config_reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_READ_CURRENT,
                                 MPI_CONFIG_PAGETYPE_IOC, 1, 0,
                                 page, 16, false);
    g_assert_cmphex(le16_to_cpu(config_reply.IOCStatus),
                    ==, MPI_IOCSTATUS_SUCCESS);
    qtest_memread(qts, page, ioc_page, 16);
    stl_le_p(ioc_page + 4, MPI_IOCPAGE1_REPLY_COALESCING);
    stl_le_p(ioc_page + 8, 100);
    ioc_page[12] = 2;
    qtest_memwrite(qts, page, ioc_page, 16);
    config_reply = mptspi_config(mpt, MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT,
                                 MPI_CONFIG_PAGETYPE_IOC, 1, 0, page, 16, true);
    g_assert_cmphex(le16_to_cpu(config_reply.IOCStatus),
                    ==, MPI_IOCSTATUS_SUCCESS);
    qpci_io_writel(dev, mpt->bar, MPI_HOST_INTERRUPT_MASK_OFFSET, MPI_HIM_DIM);
    qtest_memwrite(qts, request, &facts, sizeof(facts));

    if (data) {
        /* The first reply after restoring an empty FIFO must still wait. */
        g_autofree char *saved = qtest_hmp(qts, "savevm idle-reply");
        g_autofree char *loaded = NULL;

        g_assert_cmpstr(saved, ==, "");
        loaded = qtest_hmp(qts, "loadvm idle-reply");
        g_assert_cmpstr(loaded, ==, "");
    }

    for (unsigned int cycle = 0; cycle < 3; cycle++) {
        bool use_msi = cycle == 1 && msi_cap && !qpci_has_buggy_msi(dev);

        qtest_writel(qts, msi_address, 0);
        if (use_msi) {
            g_assert_cmpuint(msi_cap, >, 0);
            qpci_config_writel(dev, msi_cap + PCI_MSI_ADDRESS_LO, msi_address);
            if (msi_control & PCI_MSI_FLAGS_64BIT) {
                qpci_config_writel(dev, msi_cap + PCI_MSI_ADDRESS_HI, 0);
            }
            qpci_config_writew(dev, msi_cap +
                               (msi_control & PCI_MSI_FLAGS_64BIT ?
                                PCI_MSI_DATA_64 : PCI_MSI_DATA_32), 0x51);
            qpci_config_writew(dev, msi_cap + PCI_MSI_FLAGS,
                               msi_control | PCI_MSI_FLAGS_ENABLE);
        }
        qtest_memset(qts, reply_address, 0, 256);
        qpci_io_writel(dev, mpt->bar, MPI_REPLY_QUEUE_OFFSET, reply_address);
        qpci_io_writel(dev, mpt->bar, MPI_REQUEST_QUEUE_OFFSET, request);
        for (unsigned int tries = 0;
             !qtest_readb(qts, reply_address + 2) && tries < 1000; tries++) {
            qtest_qmp_assert_success(qts, "{'execute':'query-status'}");
        }
        g_assert_cmpuint(qtest_readb(qts, reply_address + 2), >, 0);
        g_assert_cmphex(qpci_config_readw(dev, PCI_STATUS) &
                        PCI_STATUS_INTERRUPT, ==, 0);
        if (cycle == 0) {
            qtest_clock_step(qts, 99000);
            g_assert_cmphex(qpci_config_readw(dev, PCI_STATUS) &
                            PCI_STATUS_INTERRUPT, ==, 0);
            if (data) {
                g_autofree char *saved = qtest_hmp(qts, "savevm pending-reply");
                g_autofree char *loaded = NULL;

                g_assert_cmpstr(saved, ==, "");
                qpci_io_writel(dev, mpt->bar, MPI_DOORBELL_OFFSET,
                               MPI_FUNCTION_IOC_MESSAGE_UNIT_RESET <<
                               MPI_DOORBELL_FUNCTION_SHIFT);
                loaded = qtest_hmp(qts, "loadvm pending-reply");
                g_assert_cmpstr(loaded, ==, "");
                g_assert_cmphex(qpci_config_readw(dev, PCI_STATUS) &
                                PCI_STATUS_INTERRUPT, ==, 0);
            }
            qtest_clock_step(qts, 1000);
        } else if (cycle == 1) {
            qpci_io_writel(dev, mpt->bar, MPI_REPLY_QUEUE_OFFSET,
                           reply_address + 128);
            qpci_io_writel(dev, mpt->bar, MPI_REQUEST_QUEUE_OFFSET, request);
            for (unsigned int tries = 0;
                 !qtest_readb(qts, reply_address + 130) && tries < 1000;
                 tries++) {
                qtest_qmp_assert_success(qts, "{'execute':'query-status'}");
            }
            g_assert_cmpuint(qtest_readb(qts, reply_address + 130), >, 0);
        } else {
            qpci_io_writel(dev, mpt->bar, MPI_DOORBELL_OFFSET,
                           MPI_FUNCTION_IOC_MESSAGE_UNIT_RESET <<
                           MPI_DOORBELL_FUNCTION_SHIFT);
            qtest_clock_step(qts, 1000000);
            g_assert_cmphex(qpci_config_readw(dev, PCI_STATUS) &
                            PCI_STATUS_INTERRUPT, ==, 0);
            break;
        }
        g_assert_cmphex(qpci_config_readw(dev, PCI_STATUS) &
                        PCI_STATUS_INTERRUPT, ==,
                        use_msi ? 0 : PCI_STATUS_INTERRUPT);
        g_assert_cmphex(qtest_readl(qts, msi_address), ==, use_msi ? 0x51 : 0);
        for (unsigned int i = 0; i <= cycle; i++) {
            g_assert_cmphex(qpci_io_readl(dev, mpt->bar,
                                          MPI_REPLY_QUEUE_OFFSET),
                            !=, UINT32_MAX);
        }
        g_assert_cmphex(qpci_io_readl(dev, mpt->bar, MPI_REPLY_QUEUE_OFFSET),
                        ==, UINT32_MAX);
        g_assert_cmphex(qpci_config_readw(dev, PCI_STATUS) &
                        PCI_STATUS_INTERRUPT, ==, 0);
        if (use_msi) {
            qpci_config_writew(dev, msi_cap + PCI_MSI_FLAGS, msi_control);
        }
    }
    guest_free(alloc, msi_address);
    guest_free(alloc, page);
    guest_free(alloc, request);
    guest_free(alloc, reply_address);
}

static void mptspi_coalescing_savevm(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    if (!data) {
        g_test_skip("qemu-img is required for coalescing savevm testing");
        return;
    }
    mptspi_reply_coalescing(obj, data, alloc);
}


/*
 * The header and capabilities of both functions as the rx2600's 53C1030
 * reads them (zx1probe run 2, 20:01.0 and 20:01.1), without the fields
 * firmware writes (command, cache line size, latency, BAR bases, interrupt
 * line).  Until ia64 delivers MSI the list ends at the PM capability; the
 * chip then has MSI at 58h.
 */
static uint32_t mptspi_cfg_readl(QTestState *qts, uint8_t fn, uint8_t reg)
{
    return qtest_readl(qts, IA64_PCI_CONFIG_BASE +
                       ((uint64_t)(MPT_DEVFN | fn) << 12) + reg);
}

static void mptspi_cfg_writel(QTestState *qts, uint8_t fn, uint8_t reg,
                              uint32_t value)
{
    qtest_writel(qts, IA64_PCI_CONFIG_BASE +
                 ((uint64_t)(MPT_DEVFN | fn) << 12) + reg, value);
}

static void mptspi_test_config_image(void)
{
    static const struct {
        uint8_t reg;
        uint32_t sized;
    } bars[] = {
        { PCI_BASE_ADDRESS_0, 0xffffff01 },     /* I/O, 256 bytes */
        { PCI_BASE_ADDRESS_1, 0xffff0004 },     /* Memory [0], 64-bit 64 KiB */
        { PCI_BASE_ADDRESS_2, 0xffffffff },
        { PCI_BASE_ADDRESS_3, 0xffff0004 },     /* Memory [1], 64-bit 64 KiB */
        { PCI_BASE_ADDRESS_4, 0xffffffff },
        { PCI_BASE_ADDRESS_5, 0x00000000 },
    };
    QTestState *qts = qtest_init(
        "-machine zx1 -m 256M -S "
        "-device lsi53c1030,addr=10.0,multifunction=on "
        "-device lsi53c1030,addr=10.1,multifunction=on");
    uint8_t fn;
    size_t i;

    for (fn = 0; fn < 2; fn++) {
        g_assert_cmphex(mptspi_cfg_readl(qts, fn, PCI_VENDOR_ID), ==,
                        0x00301000);
        g_assert_cmphex(mptspi_cfg_readl(qts, fn, PCI_COMMAND) >> 16, ==,
                        0x0230);
        g_assert_cmphex(mptspi_cfg_readl(qts, fn, PCI_REVISION_ID), ==,
                        0x01000007);
        g_assert_cmphex(mptspi_cfg_readl(qts, fn, PCI_CACHE_LINE_SIZE) &
                        0xffff0000, ==, 0x00800000);
        g_assert_cmphex(mptspi_cfg_readl(qts, fn, PCI_SUBSYSTEM_VENDOR_ID),
                        ==, 0x10001000);
        g_assert_cmphex(mptspi_cfg_readl(qts, fn, PCI_CAPABILITY_LIST) &
                        0xff, ==, 0x50);
        g_assert_cmphex(mptspi_cfg_readl(qts, fn, PCI_INTERRUPT_LINE) >> 8,
                        ==, 0x121100 | (fn + 1));
        g_assert_cmphex(mptspi_cfg_readl(qts, fn, 0x50), ==, 0x06020001);
        g_assert_cmphex(mptspi_cfg_readl(qts, fn, 0x54), ==, 0);
        g_assert_cmphex(mptspi_cfg_readl(qts, fn, 0x58), ==, 0);
        g_assert_cmphex(mptspi_cfg_readl(qts, fn, 0x5c), ==, 0);

        for (i = 0; i < G_N_ELEMENTS(bars); i++) {
            mptspi_cfg_writel(qts, fn, bars[i].reg, 0xffffffff);
            g_assert_cmphex(mptspi_cfg_readl(qts, fn, bars[i].reg), ==,
                            bars[i].sized);
        }
    }

    /* Only the power state field of PMCSR is writable. */
    mptspi_cfg_writel(qts, 0, 0x54, 0xffffffff);
    g_assert_cmphex(mptspi_cfg_readl(qts, 0, 0x54) & 0xffff, ==,
                    PCI_PM_CTRL_STATE_MASK);
    qtest_quit(qts);
}

static void mptspi_run(const void *opaque)
{
    const MptSpiCase *c = opaque;
    /*
     * No -S: the reply-coalescing timer runs on QEMU_CLOCK_VIRTUAL, which
     * only advances while the machine runs.
     */
    g_autoptr(GString) args = g_string_new(
        "-machine zx1 -m 256M -device lsi53c1030,addr=10.0,id=mptspi");
    QPCIAddress addr = { .devfn = MPT_DEVFN,
                         .vendor_id = PCI_VENDOR_ID_LSI_LOGIC,
                         .device_id = PCI_DEVICE_ID_LSI_53C1030 };
    MptSpiSnapshotData *snapshot = NULL;
    QGuestAllocator alloc;
    QPCIBar io_bar;
    QMptSpi mpt = { 0 };

    if (c->snapshot) {
        snapshot = mptspi_snapshot_setup(args, NULL);
    }
    mpt.qts = qtest_init(args->str);
    qpci_init_generic(&mpt.gbus, mpt.qts, NULL, false);
    mpt.gbus.ecam_alloc_ptr = IA64_PCI_CONFIG_BASE;
    mpt.gbus.bus.mmio_alloc_ptr = MPT_MMIO_BASE;
    mpt.gbus.bus.mmio_limit = MPT_MMIO_LIMIT;
    mpt.gbus.bus.pio_alloc_ptr = MPT_PIO_BASE;
    mpt.gbus.bus.pio_limit = MPT_PIO_LIMIT;
    qpci_device_init(&mpt.dev, &mpt.gbus.bus, &addr);
    /* Place the I/O BAR too, so that enabling decode does not claim port 0. */
    io_bar = qpci_iomap(&mpt.dev, 0, NULL);
    mpt.bar = qpci_iomap(&mpt.dev, 1, NULL);
    qpci_device_enable(&mpt.dev);
    alloc_init(&alloc, 0, MPT_DMA_BASE, MPT_DMA_END, 0x1000);

    c->fn(&mpt, snapshot, &alloc);

    alloc_destroy(&alloc);
    qpci_iounmap(&mpt.dev, mpt.bar);
    qpci_iounmap(&mpt.dev, io_bar);
    qtest_quit(mpt.qts);
}

static const MptSpiCase mptspi_cases[] = {
    { "facts", mptspi_test_facts, false },
    { "io-unit-policy", mptspi_test_io_unit_policy, false },
    { "config-reset", mptspi_test_config_and_reset, false },
    { "config-savevm", mptspi_test_config_savevm, true },
    { "reply-coalescing", mptspi_reply_coalescing, false },
    { "coalescing-savevm", mptspi_coalescing_savevm, true },
};

int main(int argc, char **argv)
{
    size_t i;

    g_test_init(&argc, &argv, NULL);
    for (i = 0; i < G_N_ELEMENTS(mptspi_cases); i++) {
        g_autofree char *path = g_strdup_printf("/lsi53c1030/%s",
                                                mptspi_cases[i].name);

        qtest_add_data_func(path, &mptspi_cases[i], mptspi_run);
    }
    qtest_add_func("/lsi53c1030/config-image", mptspi_test_config_image);
    return g_test_run();
}
