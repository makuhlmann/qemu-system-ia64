/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HP zx1 LBA (Local Bus Adapter / Mercury I/O adapter) CSR block for the ia64
 * zx1 machine.
 *
 * On real zx1 the Mercury ioa is a PCI/PCI-X/AGP host bridge whose registers
 * live in an MMIO CSR block that the mio locates for it (not standard PCI config
 * space).  Linux hp-agp (drivers/char/agp/hp-agp.c) and the Windows HP AGP
 * "AgpMercury" miniport reach it through the ACPI HWP0003 device and read it as
 * ordinary configuration space to find the AGP capability (id 0x02 at 0x60, AGP
 * status at 0x64, writable AGP command at 0x68; PCI_STATUS.CAP_LIST set,
 * capability-list pointer at 0x34).
 *
 * This models that CSR block faithfully: the identity registers a driver reads
 * to recognise the bridge (FUNCTION_ID 0x00, FUNCTION_CLASS 0x08,
 * CAPABILITIES_POINTER 0x30, BUS_NUMBER 0x58), the AGP capability/command, the
 * CONFIG_ADDRESS/DATA pair (0x40/0x48) which generates configuration cycles on
 * the Mercury root bus, and the control/decode registers real Mercury exposes
 * (ARBITRATION_MASK 0x80, STATUS_CONTROL/SIC 0x108, the LMMIO/GMMIO/WLMMIO/
 * WGMMIO/ELMMIO window decoders 0x200-0x258, SLAVE_CONTROL 0x278, MSI 0x280/
 * 0x288, BUS_MODE 0x620) with their real reset values and writable masks.
 *
 * Register values and masks mirror upstream hw/pci-host/hp-zx1-ioa-regs.c; the
 * "straps" property says whether the ioa sits on an AGP or a PCI rope, as the
 * board ties its power-up pins (ERS 3.2.1).  Deliberate simplifications for this machine: the LMMIO/GMMIO
 * decoders are modelled for driver-visible fidelity but do not themselves route
 * decode -- this machine decodes through the shared PCI window and the ACPI
 * _CRS, and DMA/GART is the SBA's IOPDIR (hp-agp's "shared" path).  The MSI
 * window does route: inbound writes into it become interrupt transactions
 * (ia64_lba_attach_dma).  The block
 * carries no PCI config space of its own and does no DMA translation; the
 * graphics adapter is a real PCI device on the Mercury root bus
 * (hw/ia64/ia64_mercury.c).  It is described to guests only through the ACPI
 * HWP0003 _CRS (a CCSR VendorLong and a memory descriptor), never the EFI memory
 * map; keep base/length in lockstep with LBA0 in
 * roms/ia64-firmware/dsdt-pci-root-zx1.asl.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/bitmap.h"
#include "qemu/log.h"
#include "hw/ia64/ia64_lba.h"
#include "hw/ia64/ia64_iosapic.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "target/ia64/cpu.h"

/* Mercury CSR register offsets (HP zx1 ioa ERS / upstream hp-zx1-ioa-regs.h). */
#define LBA_FUNCTION_ID          0x000
#define LBA_FUNCTION_CLASS       0x008
#define LBA_CAP_POINTER          0x030
/* ERS 8.4: the pointer names the AGP capability in AGP mode, else PCI-X's. */
#define LBA_CAP_POINTER_PCIX     0x0a0
#define LBA_CONFIG_ADDRESS       0x040
#define LBA_CONFIG_DATA          0x048
#define LBA_BUS_NUMBER           0x058
/* ERS 8.10: secondary in byte 0x58, subordinate in 0x59, R/W, reset 0. */
#define LBA_BUS_NUMBER_WRITE     0xffffU
#define LBA_AGP_CAPABILITY       0x060
#define LBA_AGP_COMMAND          0x068
#define LBA_ARBITRATION_MASK     0x080
#define LBA_ARBITRATION_MODE     0x090
#define LBA_MT_LATENCY           0x098
#define LBA_STATUS_CONTROL       0x108
#define LBA_LMMIO_BASE           0x200
#define LBA_LMMIO_MASK           0x208
#define LBA_GMMIO_BASE           0x210
#define LBA_GMMIO_MASK           0x218
#define LBA_WLMMIO_BASE          0x220
#define LBA_WLMMIO_MASK          0x228
#define LBA_WGMMIO_BASE          0x230
#define LBA_WGMMIO_MASK          0x238
#define LBA_ELMMIO_BASE          0x250
#define LBA_ELMMIO_MASK          0x258
#define LBA_SLAVE_CONTROL        0x278
#define LBA_MSI_BASE             0x280
#define LBA_MSI_MASK             0x288
/*
 * Not in the ioa ERS, which has 0x0610 to 0x0618 reserved: the firmware reads
 * bit 3 of 0x0618 for the I/O bus clock's DLL and reports POST 0x82 "PCI clock
 * DLL error" without it (FFEBA480), then deconfigures the bridge.
 */
/*
 * ERS sec 11.2: the ioa's I/O SAPIC has a register-select/window pair here,
 * a version hardwired to 0x20 with 0x0A as its highest redirection-table
 * entry, and ten interrupt pins plus a software interrupt.
 */
#define LBA_IOSAPIC              0x800
#define LBA_IOSAPIC_PINS         11
#define LBA_IOSAPIC_VERSION      0x20

#define LBA_ROPE_CONFIG          0x610
#define LBA_ROPE_SINGLE_WIDE     (UINT64_C(1) << 8)
#define LBA_CLOCK_STATUS         0x618
#define LBA_CLOCK_DLL_LOCKED     (UINT64_C(1) << 3)

#define LBA_BUS_MODE             0x620
/*
 * The ioa's error-log block (ERS sec 6).  POST programs ERROR_CONFIG and
 * reads it back; a read that does not answer with what it wrote makes the
 * vendor SAL log a PCI bus error record for this rope, which Windows then
 * reports as a Machine Check Error.  The register at 0x6a0 is in the ERS's
 * reserved range and the firmware writes and reads it the same way.
 */
#define LBA_ERROR_CONFIG         0x680
#define LBA_ERROR_STATUS         0x688
#define LBA_ERROR_MASTER_ID      0x690
#define LBA_ERROR_CONTROL        0x6a0
#define LBA_PCIX_CAP             0x0a0
#define LBA_ROPE_ERROR           0x600
#define LBA_INBOUND_ERR_ADDR     0x290
#define LBA_INBOUND_ERR_ATTR     0x298
#define LBA_COMPLETION_MSG       0x2a0
#define LBA_OUTBOUND_ERR_ADDR    0x070

/* Reset values and writable masks (upstream hp-zx1-ioa-regs.h, AGP mode). */
#define LBA_CONFIG_ADDRESS_MASK  UINT32_C(0x00fffffc)
#define LBA_ARBITRATION_RESET    UINT32_C(0x00000001)
#define LBA_ARBITRATION_WRITE    UINT32_C(0x0000007f)
#define LBA_ARBITRATION_F        UINT32_C(0x00000040)
#define LBA_SIC_FORWARD_VGA      (UINT64_C(1) << 3)
#define LBA_SIC_CLEAR_LOG        (UINT64_C(1) << 4)
#define LBA_SIC_CLEAR_ENABLE     (UINT64_C(1) << 5)
#define LBA_SIC_HARD_FAIL        (UINT64_C(1) << 6)
#define LBA_SIC_BUS_IN_RESET     (UINT64_C(1) << 32)
#define LBA_SIC_LATCH_MASK       (LBA_SIC_FORWARD_VGA | LBA_SIC_CLEAR_LOG | \
                                  LBA_SIC_CLEAR_ENABLE | LBA_SIC_HARD_FAIL)
#define LBA_LMMIO_BASE_RESET     UINT64_C(0x80000000)
#define LBA_LMMIO_BASE_WRITE     UINT64_C(0x7fff0001)
#define LBA_LMMIO_MASK_RESET     UINT64_C(0x80000000)
#define LBA_LMMIO_MASK_WRITE     UINT64_C(0x7fff0000)
#define LBA_GMMIO_BASE_WRITE     UINT64_C(0x00000ffffc000001)
#define LBA_GMMIO_MASK_WRITE     UINT64_C(0x00000ffffc000000)
#define LBA_WLMMIO_BASE_RESET    UINT64_C(0x80000000)
#define LBA_WLMMIO_BASE_WRITE    UINT64_C(0x7ff00001)
#define LBA_WLMMIO_MASK_RESET    UINT64_C(0x80000000)
#define LBA_WLMMIO_MASK_WRITE    UINT64_C(0x7ff00000)
#define LBA_WGMMIO_BASE_WRITE    UINT64_C(0x00000fff00000001)
#define LBA_WGMMIO_MASK_WRITE    UINT64_C(0x00000fff00000000)
#define LBA_ELMMIO_BASE_RESET    UINT64_C(0x80000000)
#define LBA_ELMMIO_BASE_WRITE    UINT64_C(0x7ff00001)
#define LBA_ELMMIO_MASK_RESET    UINT64_C(0x80000000)
#define LBA_ELMMIO_MASK_WRITE    UINT64_C(0x7ff00000)
#define LBA_MSI_BASE_RESET       UINT64_C(0x00000000fee00001)
#define LBA_MSI_BASE_WRITE       UINT64_C(0x00000fffffff0001)
#define LBA_MSI_MASK_RESET       UINT64_C(0x00000ffffff00000)
#define LBA_MSI_MASK_WRITE       UINT64_C(0x00000fffffff0000)
/* Range Enable (bit 0 of every BASE register) and the 44-bit address space. */
#define LBA_RANGE_ENABLE         UINT64_C(1)
#define LBA_ADDRESS_MASK         UINT64_C(0x00000fffffffffff)
#define LBA_MSI_WINDOW_MIN       UINT64_C(0x10000)

static void ia64_lba_update_msi(IA64LBAState *s);
/*
 * Bits 31:19 have no reset value in ERS 9.13; SAL_B sets bit 31 on a PCI
 * rope (FFEB8276) and the rx2600 reads it back (capture 2026-10-03, IOA-3).
 */
#define LBA_SLAVE_CONTROL_RESET  UINT64_C(0x00000006)
#define LBA_SLAVE_CONTROL_WRITE  UINT64_C(0x8006200f)
/*
 * ERS 8.2 and 8.3: of the PCI command only Memory Space, Bus Master, Parity
 * Error Response and SERR# Enable exist (the rx2600 reads 0x0146), and the
 * line size and latency timer are plain storage; all reset to 0.
 */
#define LBA_FUNCTION_ID_WRITE    UINT64_C(0x0000014600000000)
#define LBA_FUNCTION_CLASS_WRITE UINT64_C(0x0000ffff00000000)
/* ERS 10.2 and 10.3: the arbiter's mode bits and its latency timer. */
#define LBA_ARBITRATION_MODE_WRITE UINT32_C(0x00ff0000)
#define LBA_MT_LATENCY_WRITE     UINT32_C(0x000000ff)
#define LBA_BUS_MODE_AGP         UINT64_C(0x00000001)
#define LBA_BUS_MODE_SIX_MASTERS (UINT64_C(1) << 3)
#define LBA_BUS_MODE_ROPE_2X_L   (UINT64_C(1) << 5)
/* drv_m66en, master_32bit (ERS 3.2.2) and bit 16, which POST sets. */
#define LBA_BUS_MODE_WRITE       UINT64_C(0x00018100)
/*
 * ERS 8.7: the PCI-X capability, with the command bits 22:16 writable and
 * the three split-completion status bits write-1-to-clear.
 */
#define LBA_PCIX_CAP_RESET       UINT64_C(0x0013ff0000000007)
#define LBA_PCIX_CAP_WRITE       UINT64_C(0x00000000007f0000)
#define LBA_PCIX_CAP_W1C         ((UINT64_C(1) << 61) | (UINT64_C(1) << 51) | \
                                  (UINT64_C(1) << 50))

static uint64_t ia64_lba_size_mask(unsigned int size)
{
    return size >= 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
}

/* Masked read-modify-write of a register latch (upstream ioa_latch_write). */
static void ia64_lba_latch(uint64_t *latch, uint64_t writable,
                           uint64_t access_mask, uint64_t data)
{
    uint64_t mask = writable & access_mask;

    *latch = (*latch & ~mask) | (data & mask);
}

/*
 * The 64-bit value at CSR offset [base, base+8) for reads.  hp-agp/hpagp read
 * sub-fields with byte-lane accesses; the caller extracts the requested lanes.
 * Every offset not modelled reads as zero.  CONFIG_DATA (0x48) is handled
 * separately by ia64_lba_config_read().
 */
static uint64_t ia64_lba_reg(IA64LBAState *s, uint64_t base, bool *modelled)
{
    *modelled = true;
    switch (base) {
    case LBA_FUNCTION_ID:
        /* vendor | device | command | status(CAP_LIST set). */
        return IA64_LBA_VENDOR_ID |
               (IA64_LBA_DEVICE_ID << 16) |
               s->pci_command |
               (IA64_LBA_PCI_STATUS_RESET << 48);
    case LBA_FUNCTION_CLASS:
        /* revision (byte 0x08) | class code (bytes 0x09-0x0b): host bridge. */
        return IA64_LBA_REVISION | (IA64_LBA_CLASS_CODE << 8) |
               s->line_latency;
    case LBA_CAP_POINTER:
        /* The capabilities pointer lands in byte 0x34. */
        return (uint64_t)((s->straps & LBA_BUS_MODE_AGP) ?
                          IA64_LBA_AGP_CAP_OFFSET : LBA_CAP_POINTER_PCIX) << 32;
    case LBA_CONFIG_ADDRESS:
        return s->config_address;
    case LBA_BUS_NUMBER:
        return s->bus_number;
    case LBA_AGP_CAPABILITY:
        /* AGP capability: id 0x02 at byte 0x60, AGP status at byte 0x64. */
        return IA64_LBA_AGP_CAPABILITY;
    case LBA_AGP_COMMAND:
        return s->agp_command;
    case LBA_ARBITRATION_MASK:
        return s->arbitration_mask;
    case LBA_ARBITRATION_MODE:
        return s->arbitration_mode;
    case LBA_MT_LATENCY:
        return s->mt_latency;
    case LBA_STATUS_CONTROL:
        /*
         * SIC latch.  RC (bit 32) "returns 1 while the PCI bus is held in
         * reset" (ioa ERS sec 8.2.1), and this model never asserts the
         * subordinate-bus reset the RF bit would drive, so it reads 0.
         */
        return s->status_control & LBA_SIC_LATCH_MASK;
    case LBA_LMMIO_BASE:   return s->lmmio_base;
    case LBA_LMMIO_MASK:   return s->lmmio_mask;
    case LBA_GMMIO_BASE:   return s->gmmio_base;
    case LBA_GMMIO_MASK:   return s->gmmio_mask;
    case LBA_WLMMIO_BASE:  return s->wlmmio_base;
    case LBA_WLMMIO_MASK:  return s->wlmmio_mask;
    case LBA_WGMMIO_BASE:  return s->wgmmio_base;
    case LBA_WGMMIO_MASK:  return s->wgmmio_mask;
    case LBA_ELMMIO_BASE:  return s->elmmio_base;
    case LBA_ELMMIO_MASK:  return s->elmmio_mask;
    case LBA_SLAVE_CONTROL: return s->slave_control;
    case LBA_MSI_BASE:     return s->msi_base;
    case LBA_MSI_MASK:     return s->msi_mask;
    case LBA_ROPE_CONFIG:
        return s->rope_config |
               ((s->straps & LBA_BUS_MODE_ROPE_2X_L) ? LBA_ROPE_SINGLE_WIDE : 0);
    case LBA_CLOCK_STATUS: return LBA_CLOCK_DLL_LOCKED;
    case LBA_BUS_MODE:     return s->bus_mode;
    case LBA_ERROR_CONFIG: return s->error_config;
    case LBA_ERROR_CONTROL: return s->error_control;
    case LBA_PCIX_CAP:     return s->pcix_cap;
    case LBA_ROPE_ERROR:   return s->rope_error;
    /* The log registers: storage, because this model logs no error itself. */
    case LBA_ERROR_STATUS:      return s->error_status;
    case LBA_ERROR_MASTER_ID:   return s->error_master_id;
    case LBA_INBOUND_ERR_ADDR:  return s->inbound_err_addr;
    case LBA_INBOUND_ERR_ATTR:  return s->inbound_err_attr;
    case LBA_COMPLETION_MSG:    return s->completion_msg;
    case LBA_OUTBOUND_ERR_ADDR: return s->outbound_err_addr;
    default:
        *modelled = false;
        return 0;
    }
}

/* Decode CONFIG_ADDRESS into a device on the Mercury bus + config offset. */
static PCIDevice *ia64_lba_config_target(IA64LBAState *s, unsigned int lane,
                                         uint32_t *reg)
{
    uint32_t addr = s->config_address & LBA_CONFIG_ADDRESS_MASK;
    uint8_t bus = (addr >> 16) & 0xff;
    uint8_t dev = (addr >> 11) & 0x1f;
    uint8_t func = (addr >> 8) & 0x7;

    *reg = (addr & 0xfc) | lane;
    if (s->config_bus == NULL) {
        return NULL;
    }
    /*
     * The bus field is rope-local: bus 0 is the bus behind this ioa, whatever
     * number the rest of the machine gives it.  The vendor firmware programs
     * BUS_NUMBER with this rope's place in the machine-wide numbering (rope 1
     * gets secondary 32, subordinate 63) and still addresses that bus as 0,
     * which is what ERS 8.10 means by "no effect on chip operation".  Windows
     * reaches this path through the vendor SAL, which serves the PCI config
     * calls it makes for the root its ACPI tables number 32
     * (WSRV03/base/hals/halia64/ia64/i64pcibus.c:1090).
     */
    if (bus == 0) {
        bus = pci_bus_num(s->config_bus);
    }
    return pci_find_device(s->config_bus, bus, PCI_DEVFN(dev, func));
}

static uint64_t ia64_lba_config_read(IA64LBAState *s, unsigned int lane,
                                     unsigned int size)
{

    unsigned int cycle = size == 8 ? 4 : size;
    uint32_t reg;
    PCIDevice *d;

    if (lane + cycle > 4) {
        return 0;
    }
    d = ia64_lba_config_target(s, lane, &reg);
    if (d == NULL) {
        return ia64_lba_size_mask(cycle);   /* open bus: all-ones */
    }
    return pci_host_config_read_common(d, reg, pci_config_size(d), cycle);
}

static void ia64_lba_config_write(IA64LBAState *s, unsigned int lane,
                                  unsigned int size, uint64_t value)
{
    unsigned int cycle = size == 8 ? 4 : size;
    uint32_t reg;
    PCIDevice *d;

    if (lane + cycle > 4) {
        return;
    }
    d = ia64_lba_config_target(s, lane, &reg);
    if (d != NULL) {
        pci_host_config_write_common(d, reg, pci_config_size(d),
                                     value & ia64_lba_size_mask(cycle), cycle);
    }
}


static MemTxResult ia64_lba_read(void *opaque, hwaddr addr, uint64_t *data,
                                 unsigned int size, MemTxAttrs attrs)
{
    IA64LBAState *s = opaque;
    unsigned int lane = addr & 7;
    uint64_t base = addr & ~UINT64_C(7);
    uint64_t reg;
    bool modelled;

    (void)attrs;
    if ((size != 1 && size != 2 && size != 4 && size != 8) || lane + size > 8) {
        *data = 0;
        return MEMTX_OK;
    }
    if (base == LBA_CONFIG_DATA) {
        *data = ia64_lba_config_read(s, (unsigned int)(addr - LBA_CONFIG_DATA),
                                     size);
        return MEMTX_OK;
    }
    reg = ia64_lba_reg(s, base, &modelled);
    if (!modelled && !test_and_set_bit(base, s->unimp_read)) {
        qemu_log_mask(LOG_UNIMP, "ia64-lba: unimplemented read at 0x%"
                      HWADDR_PRIx " (size %u)\n", s->csr_base + addr, size);
    }
    *data = (reg >> (lane * 8)) & ia64_lba_size_mask(size);
    return MEMTX_OK;
}

static MemTxResult ia64_lba_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned int size, MemTxAttrs attrs)
{
    IA64LBAState *s = opaque;
    unsigned int lane = addr & 7;
    uint64_t base = addr & ~UINT64_C(7);
    uint64_t mask, data, latch, writable;

    (void)attrs;
    if ((size != 1 && size != 2 && size != 4 && size != 8) || lane + size > 8) {
        return MEMTX_OK;
    }
    if (base == LBA_CONFIG_DATA) {
        ia64_lba_config_write(s, (unsigned int)(addr - LBA_CONFIG_DATA), size,
                              value);
        return MEMTX_OK;
    }

    /* Right-justified byte-lane write of the addressed sub-field. */
    mask = ia64_lba_size_mask(size) << (lane * 8);
    data = (value << (lane * 8)) & mask;

    switch (base) {
    case LBA_FUNCTION_ID:
        ia64_lba_latch(&s->pci_command, LBA_FUNCTION_ID_WRITE, mask, data);
        break;
    case LBA_FUNCTION_CLASS:
        ia64_lba_latch(&s->line_latency, LBA_FUNCTION_CLASS_WRITE, mask,
                       data);
        break;
    case LBA_ARBITRATION_MODE:
        latch = s->arbitration_mode;
        ia64_lba_latch(&latch, LBA_ARBITRATION_MODE_WRITE, mask, data);
        s->arbitration_mode = (uint32_t)latch;
        break;
    case LBA_MT_LATENCY:
        latch = s->mt_latency;
        ia64_lba_latch(&latch, LBA_MT_LATENCY_WRITE, mask, data);
        s->mt_latency = (uint32_t)latch;
        break;
    case LBA_BUS_NUMBER:
        latch = s->bus_number;
        ia64_lba_latch(&latch, LBA_BUS_NUMBER_WRITE, mask, data);
        s->bus_number = (uint32_t)latch;
        break;
    case LBA_CONFIG_ADDRESS:
        latch = s->config_address;
        ia64_lba_latch(&latch, LBA_CONFIG_ADDRESS_MASK, mask, data);
        s->config_address = (uint32_t)(latch & LBA_CONFIG_ADDRESS_MASK);
        break;
    case LBA_AGP_COMMAND:
        latch = s->agp_command;
        ia64_lba_latch(&latch, IA64_LBA_AGP_COMMAND_WRITABLE, mask, data);
        s->agp_command = (uint32_t)latch;
        break;
    case LBA_ARBITRATION_MASK:
        /* The F bit (six-masters arbitration) is writable only in that mode. */
        writable = LBA_ARBITRATION_WRITE;
        if (!(s->bus_mode & LBA_BUS_MODE_SIX_MASTERS)) {
            writable &= ~(uint64_t)LBA_ARBITRATION_F;
        }
        latch = s->arbitration_mask;
        ia64_lba_latch(&latch, writable, mask, data);
        s->arbitration_mask = (uint32_t)latch;
        break;
    case LBA_STATUS_CONTROL:
        latch = s->status_control;
        ia64_lba_latch(&latch, LBA_SIC_LATCH_MASK, mask, data);
        /*
         * Writing CL clears the error log only where CE was already set, and
         * a read of CL then says whether the clear succeeded (ERS 8.2.1).
         * The clear consumes the enable, so the pair reads back CE = 0 and
         * CL = 1: the vendor firmware sets CE, writes CL back on top of it
         * and polls for exactly that (SAL_B FFF3C050), 256 times before it
         * gives up with "Unable to clear the error logs".
         */
        if (mask & (LBA_SIC_CLEAR_LOG | LBA_SIC_CLEAR_ENABLE)) {
            bool cleared = (latch & LBA_SIC_CLEAR_LOG) &&
                           (s->status_control & LBA_SIC_CLEAR_ENABLE);

            latch &= ~(LBA_SIC_CLEAR_LOG | LBA_SIC_CLEAR_ENABLE);
            latch |= cleared ? LBA_SIC_CLEAR_LOG
                             : (data & mask & LBA_SIC_CLEAR_ENABLE);
        }
        s->status_control = (uint32_t)latch;
        break;
    case LBA_LMMIO_BASE:
        ia64_lba_latch(&s->lmmio_base, LBA_LMMIO_BASE_WRITE, mask, data);
        break;
    case LBA_LMMIO_MASK:
        ia64_lba_latch(&s->lmmio_mask, LBA_LMMIO_MASK_WRITE, mask, data);
        break;
    case LBA_GMMIO_BASE:
        ia64_lba_latch(&s->gmmio_base, LBA_GMMIO_BASE_WRITE, mask, data);
        break;
    case LBA_GMMIO_MASK:
        ia64_lba_latch(&s->gmmio_mask, LBA_GMMIO_MASK_WRITE, mask, data);
        break;
    case LBA_WLMMIO_BASE:
        ia64_lba_latch(&s->wlmmio_base, LBA_WLMMIO_BASE_WRITE, mask, data);
        break;
    case LBA_WLMMIO_MASK:
        ia64_lba_latch(&s->wlmmio_mask, LBA_WLMMIO_MASK_WRITE, mask, data);
        break;
    case LBA_WGMMIO_BASE:
        ia64_lba_latch(&s->wgmmio_base, LBA_WGMMIO_BASE_WRITE, mask, data);
        break;
    case LBA_WGMMIO_MASK:
        ia64_lba_latch(&s->wgmmio_mask, LBA_WGMMIO_MASK_WRITE, mask, data);
        break;
    case LBA_ELMMIO_BASE:
        ia64_lba_latch(&s->elmmio_base, LBA_ELMMIO_BASE_WRITE, mask, data);
        break;
    case LBA_ELMMIO_MASK:
        ia64_lba_latch(&s->elmmio_mask, LBA_ELMMIO_MASK_WRITE, mask, data);
        break;
    case LBA_SLAVE_CONTROL:
        ia64_lba_latch(&s->slave_control, LBA_SLAVE_CONTROL_WRITE, mask, data);
        break;
    case LBA_MSI_BASE:
        ia64_lba_latch(&s->msi_base, LBA_MSI_BASE_WRITE, mask, data);
        ia64_lba_update_msi(s);
        break;
    case LBA_MSI_MASK:
        ia64_lba_latch(&s->msi_mask, LBA_MSI_MASK_WRITE, mask, data);
        ia64_lba_update_msi(s);
        break;
    case LBA_ROPE_CONFIG:
        ia64_lba_latch(&s->rope_config, ~LBA_ROPE_SINGLE_WIDE, mask, data);
        break;
    case LBA_BUS_MODE:
        ia64_lba_latch(&s->bus_mode, LBA_BUS_MODE_WRITE, mask, data);
        break;
    case LBA_ERROR_CONFIG:
        ia64_lba_latch(&s->error_config, UINT64_MAX, mask, data);
        break;
    case LBA_PCIX_CAP:
        s->pcix_cap &= ~(data & LBA_PCIX_CAP_W1C);
        ia64_lba_latch(&s->pcix_cap, LBA_PCIX_CAP_WRITE, mask, data);
        break;
    case LBA_ROPE_ERROR:
        ia64_lba_latch(&s->rope_error, UINT64_MAX, mask, data);
        break;
    case LBA_ERROR_CONTROL:
        ia64_lba_latch(&s->error_control, UINT64_MAX, mask, data);
        break;
    case LBA_ERROR_STATUS:
        ia64_lba_latch(&s->error_status, UINT64_MAX, mask, data);
        break;
    case LBA_ERROR_MASTER_ID:
        ia64_lba_latch(&s->error_master_id, UINT64_MAX, mask, data);
        break;
    case LBA_INBOUND_ERR_ADDR:
        ia64_lba_latch(&s->inbound_err_addr, UINT64_MAX, mask, data);
        break;
    case LBA_INBOUND_ERR_ATTR:
        ia64_lba_latch(&s->inbound_err_attr, UINT64_MAX, mask, data);
        break;
    case LBA_COMPLETION_MSG:
        ia64_lba_latch(&s->completion_msg, UINT64_MAX, mask, data);
        break;
    case LBA_OUTBOUND_ERR_ADDR:
        ia64_lba_latch(&s->outbound_err_addr, UINT64_MAX, mask, data);
        break;
    default:
        /* Read-only and unmodelled registers ignore writes; name the latter. */
        if (!test_and_set_bit(base, s->unimp_write)) {
            qemu_log_mask(LOG_UNIMP, "ia64-lba: unimplemented write at 0x%"
                          HWADDR_PRIx " (size %u) value 0x%" PRIx64 "\n",
                          s->csr_base + addr, size, data);
        }
        break;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps ia64_lba_ops = {
    .read_with_attrs = ia64_lba_read,
    .write_with_attrs = ia64_lba_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

/*
 * Interrupt Space (ERS 9.2, Registers 9.11 and 9.12): a master's write that
 * falls in the MSI window is a message-signalled interrupt, which the mio
 * repeats on the processor bus as an interrupt transaction with the same
 * address and data (mio ERS 3.4.4.1).  A lowest-priority message, whose
 * address carries the redirection hint, goes to the processor it names: the
 * mio redirects nothing here.  A read of the window is target-aborted (mio
 * ERS 2.2).
 */
static MemTxResult ia64_lba_msi_read(void *opaque, hwaddr addr,
                                     uint64_t *data, unsigned size,
                                     MemTxAttrs attrs)
{
    (void)opaque;
    (void)addr;
    (void)size;
    (void)attrs;
    *data = ~0ULL;
    return MEMTX_ERROR;
}

static MemTxResult ia64_lba_msi_write(void *opaque, hwaddr addr,
                                      uint64_t data, unsigned size,
                                      MemTxAttrs attrs)
{
    IA64LBAState *s = opaque;
    uint64_t address = s->msi.addr + addr;

    (void)size;
    (void)attrs;
    if (((data >> 8) & 7) == 1) {
        data &= ~(7ULL << 8);
    }
    ia64_interrupt_transaction(address, data);
    return MEMTX_OK;
}

static const MemoryRegionOps ia64_lba_msi_ops = {
    .read_with_attrs = ia64_lba_msi_read,
    .write_with_attrs = ia64_lba_msi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

/*
 * Place the window where MSI_BASE and MSI_MASK name it (bits 43:16; the
 * mask's set bits select the compared address bits) and switch it with
 * MSI_BASE's Range Enable.
 */
static void ia64_lba_update_msi(IA64LBAState *s)
{
    uint64_t mask = s->msi_mask & LBA_MSI_MASK_WRITE;
    uint64_t size = (~mask & LBA_ADDRESS_MASK) + 1;

    if (!s->dma_attached) {
        return;
    }
    memory_region_transaction_begin();
    memory_region_set_enabled(&s->msi, (s->msi_base & LBA_RANGE_ENABLE) != 0);
    memory_region_set_size(&s->msi, size);
    memory_region_set_address(&s->msi, s->msi_base & mask);
    memory_region_transaction_commit();
}

static AddressSpace *ia64_lba_dma_as(PCIBus *bus, void *opaque, int devfn)
{
    IA64LBAState *s = opaque;

    (void)bus;
    (void)devfn;
    return &s->dma_as;
}

static const PCIIOMMUOps ia64_lba_dma_ops = {
    .get_address_space = ia64_lba_dma_as,
};

void ia64_lba_attach_dma(IA64LBAState *s, PCIBus *bus, MemoryRegion *dma)
{
    memory_region_init(&s->dma_root, OBJECT(s), "ia64-lba-inbound",
                       memory_region_size(dma));
    memory_region_init_alias(&s->dma_sba, OBJECT(s), "ia64-lba-dma", dma, 0,
                             memory_region_size(dma));
    memory_region_add_subregion(&s->dma_root, 0, &s->dma_sba);
    memory_region_init_io(&s->msi, OBJECT(s), &ia64_lba_msi_ops, s,
                          "ia64-lba-msi", LBA_MSI_WINDOW_MIN);
    memory_region_add_subregion_overlap(&s->dma_root, 0, &s->msi, 1);
    address_space_init(&s->dma_as, &s->dma_root, "ia64-lba-inbound");
    s->dma_attached = true;
    ia64_lba_update_msi(s);
    pci_setup_iommu(bus, &ia64_lba_dma_ops, s);
}


void ia64_lba_set_config_bus(IA64LBAState *s, PCIBus *bus)
{
    s->config_bus = bus;
}

qemu_irq ia64_lba_iosapic_input(IA64LBAState *s, unsigned int pin)
{
    assert(pin < LBA_IOSAPIC_PINS);
    return qdev_get_gpio_in(s->iosapic, pin);
}

static void ia64_lba_reset(DeviceState *dev);

static void ia64_lba_realize(DeviceState *dev, Error **errp)
{
    IA64LBAState *s = IA64_LBA(dev);

    s->unimp_read = bitmap_new(IA64_LBA_CSR_SIZE);
    s->unimp_write = bitmap_new(IA64_LBA_CSR_SIZE);
    memory_region_init(&s->csr, OBJECT(s), "ia64-zx1-lba",
                       IA64_LBA_CSR_SIZE);
    memory_region_init_io(&s->regs, OBJECT(s), &ia64_lba_ops, s,
                          "ia64-zx1-lba-regs", IA64_LBA_CSR_SIZE);
    memory_region_add_subregion(&s->csr, 0, &s->regs);

    s->iosapic = qdev_new(TYPE_IA64_IOSAPIC);
    object_property_add_child(OBJECT(s), "iosapic", OBJECT(s->iosapic));
    qdev_prop_set_uint32(s->iosapic, "num-pins", LBA_IOSAPIC_PINS);
    qdev_prop_set_uint32(s->iosapic, "version", LBA_IOSAPIC_VERSION);
    qdev_prop_set_uint8(s->iosapic, "face", IA64_IOSAPIC_FACE_IOA);
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->iosapic), errp)) {
        return;
    }
    memory_region_init_alias(&s->iosapic_mr, OBJECT(s), "ia64-zx1-lba-iosapic",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(s->iosapic),
                                                    0),
                             0, IA64_LBA_CSR_SIZE - LBA_IOSAPIC);
    memory_region_add_subregion_overlap(&s->csr, LBA_IOSAPIC,
                                        &s->iosapic_mr, 1);
    /*
     * A base of zero is an ioa that answers only in the rope guest window the
     * mio opens, which is where the vendor firmware looks; the one our own
     * firmware publishes through ACPI keeps its fixed base as well.
     */
    if (s->csr_base != 0) {
        memory_region_add_subregion(get_system_memory(), s->csr_base, &s->csr);
    }
    /*
     * Seed the register reset values here too: this device sits on no qbus
     * (it is mapped straight into system memory), so it is outside the machine
     * reset tree and its legacy reset method would not otherwise run.
     */
    ia64_lba_reset(dev);
}

static void ia64_lba_reset(DeviceState *dev)
{
    IA64LBAState *s = IA64_LBA(dev);

    s->bus_number = 0;
    s->config_address = 0;
    s->agp_command = 0;
    s->arbitration_mask = LBA_ARBITRATION_RESET;
    s->pci_command = 0;
    s->line_latency = 0;
    s->arbitration_mode = 0;
    s->mt_latency = 0;
    s->status_control = 0;
    s->lmmio_base = LBA_LMMIO_BASE_RESET;
    s->lmmio_mask = LBA_LMMIO_MASK_RESET;
    s->gmmio_base = 0;
    s->gmmio_mask = 0;
    s->wlmmio_base = LBA_WLMMIO_BASE_RESET;
    s->wlmmio_mask = LBA_WLMMIO_MASK_RESET;
    s->wgmmio_base = 0;
    s->wgmmio_mask = 0;
    s->elmmio_base = LBA_ELMMIO_BASE_RESET;
    s->elmmio_mask = LBA_ELMMIO_MASK_RESET;
    s->msi_base = LBA_MSI_BASE_RESET;
    s->msi_mask = LBA_MSI_MASK_RESET;
    ia64_lba_update_msi(s);
    s->slave_control = LBA_SLAVE_CONTROL_RESET;
    s->bus_mode = s->straps;
    s->rope_config = 0;
    s->error_config = 0;
    s->error_control = 0;
    s->pcix_cap = LBA_PCIX_CAP_RESET;
    s->rope_error = 0;
    s->error_status = 0;
    s->error_master_id = 0;
    s->inbound_err_addr = 0;
    s->inbound_err_attr = 0;
    s->completion_msg = 0;
    s->outbound_err_addr = 0;
}

static int ia64_lba_post_load(void *opaque, int version_id)
{
    (void)version_id;
    ia64_lba_update_msi(opaque);
    return 0;
}

static const VMStateDescription vmstate_ia64_lba = {
    .name = "ia64-zx1-lba",
    .version_id = 5,
    .minimum_version_id = 4,
    .post_load = ia64_lba_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(bus_number, IA64LBAState),
        VMSTATE_UINT32(config_address, IA64LBAState),
        VMSTATE_UINT32(agp_command, IA64LBAState),
        VMSTATE_UINT32(arbitration_mask, IA64LBAState),
        VMSTATE_UINT32(status_control, IA64LBAState),
        VMSTATE_UINT64(lmmio_base, IA64LBAState),
        VMSTATE_UINT64(lmmio_mask, IA64LBAState),
        VMSTATE_UINT64(gmmio_base, IA64LBAState),
        VMSTATE_UINT64(gmmio_mask, IA64LBAState),
        VMSTATE_UINT64(wlmmio_base, IA64LBAState),
        VMSTATE_UINT64(wlmmio_mask, IA64LBAState),
        VMSTATE_UINT64(wgmmio_base, IA64LBAState),
        VMSTATE_UINT64(wgmmio_mask, IA64LBAState),
        VMSTATE_UINT64(elmmio_base, IA64LBAState),
        VMSTATE_UINT64(elmmio_mask, IA64LBAState),
        VMSTATE_UINT64(msi_base, IA64LBAState),
        VMSTATE_UINT64(msi_mask, IA64LBAState),
        VMSTATE_UINT64(slave_control, IA64LBAState),
        VMSTATE_UINT64(bus_mode, IA64LBAState),
        VMSTATE_UINT64(error_config, IA64LBAState),
        VMSTATE_UINT64(error_control, IA64LBAState),
        VMSTATE_UINT64(pcix_cap, IA64LBAState),
        VMSTATE_UINT64(rope_error, IA64LBAState),
        VMSTATE_UINT64(error_status, IA64LBAState),
        VMSTATE_UINT64(error_master_id, IA64LBAState),
        VMSTATE_UINT64(inbound_err_addr, IA64LBAState),
        VMSTATE_UINT64(inbound_err_attr, IA64LBAState),
        VMSTATE_UINT64(completion_msg, IA64LBAState),
        VMSTATE_UINT64(outbound_err_addr, IA64LBAState),
        VMSTATE_UINT64_V(pci_command, IA64LBAState, 5),
        VMSTATE_UINT64_V(line_latency, IA64LBAState, 5),
        VMSTATE_UINT32_V(arbitration_mode, IA64LBAState, 5),
        VMSTATE_UINT32_V(mt_latency, IA64LBAState, 5),
        VMSTATE_END_OF_LIST()
    },
};

static void ia64_lba_unrealize(DeviceState *dev)
{
    IA64LBAState *s = IA64_LBA(dev);

    g_free(s->unimp_read);
    g_free(s->unimp_write);
}

static const Property ia64_lba_properties[] = {
    DEFINE_PROP_UINT64("csr-base", IA64LBAState, csr_base, IA64_LBA_CSR_BASE),
    DEFINE_PROP_UINT32("straps", IA64LBAState, straps, IA64_LBA_STRAPS_AGP),
};

static void ia64_lba_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    (void)data;
    dc->realize = ia64_lba_realize;
    dc->unrealize = ia64_lba_unrealize;
    dc->desc = "HP zx1 LBA/Mercury CSR block";
    dc->vmsd = &vmstate_ia64_lba;
    device_class_set_legacy_reset(dc, ia64_lba_reset);
    device_class_set_props(dc, ia64_lba_properties);
    dc->user_creatable = false;
}

static const TypeInfo ia64_lba_info = {
    .name          = TYPE_IA64_LBA,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(IA64LBAState),
    .class_init    = ia64_lba_class_init,
};

static void ia64_lba_register_types(void)
{
    type_register_static(&ia64_lba_info);
}

type_init(ia64_lba_register_types)
