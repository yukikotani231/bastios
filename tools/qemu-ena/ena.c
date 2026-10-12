/*
 * A model of the AWS Elastic Network Adapter (ENA) for QEMU, written to test
 * BastiOS's driver (src/ena.bastio) where no real ENA is at hand. It follows
 * the register and descriptor layouts of Linux's drivers/net/ethernet/amazon/ena
 * (ena_regs_defs.h, ena_admin_defs.h, ena_eth_io_defs.h; GPL-2.0 OR Linux-OpenIB
 * headers), not the hardware, and it is deliberately strict: a driver that
 * breaks a rule of the interface (wrong phase bit, a command the device does
 * not know, a doorbell for a queue that does not exist) is told so in the
 * `-d guest_errors` log and the command or frame is dropped.
 *
 * Supported: the register file and reset, the readless register read, the
 * admin queue with GET/SET_FEATURE (device attributes, maximum queues, AENQ
 * configuration, MTU), CREATE/DESTROY_CQ and _SQ (host placement only: no LLQ),
 * one transmit and one receive queue pair, no interrupts, no asynchronous
 * events. The device property `readless` (default on) selects PCI revision 0
 * (register reads go through the response buffer, direct reads return
 * 0xFFFFFFFF) or revision 1 (direct reads work).
 *
 * Build: tools/qemu-ena/build.sh. SPDX-License-Identifier: Apache-2.0 OR MIT
 * (the model; QEMU itself is GPL-2.0-or-later and the patched build is
 * test-only tooling, not distributed).
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define TYPE_ENA "ena"
OBJECT_DECLARE_SIMPLE_TYPE(EnaState, ENA)

#define ENA_BAR0_SIZE   0x4000
#define ENA_ADMIN_DEPTH_MAX 256
#define ENA_NQ          2

/* registers */
#define R_VERSION       0x00
#define R_CTRL_VERSION  0x04
#define R_CAPS          0x08
#define R_CAPS_EXT      0x0c
#define R_AQ_BASE_LO    0x10
#define R_AQ_BASE_HI    0x14
#define R_AQ_CAPS       0x18
#define R_ACQ_BASE_LO   0x20
#define R_ACQ_BASE_HI   0x24
#define R_ACQ_CAPS      0x28
#define R_AQ_DB         0x2c
#define R_ACQ_TAIL      0x30
#define R_AENQ_CAPS     0x34
#define R_AENQ_BASE_LO  0x38
#define R_AENQ_BASE_HI  0x3c
#define R_AENQ_HEAD_DB  0x40
#define R_AENQ_TAIL     0x44
#define R_INTR_MASK     0x4c
#define R_DEV_CTL       0x54
#define R_DEV_STS       0x58
#define R_MMIO_READ     0x5c
#define R_MMIO_RESP_LO  0x60
#define R_MMIO_RESP_HI  0x64

/* doorbells and CQ registers, as the model hands them out */
#define DB_TX           0x2000   /* + 4 * sq index */
#define DB_RX           0x2100
#define CQ_UNMASK       0x3000   /* + 4 * cq index */

#define STS_READY       0x1
#define STS_RESET_IN_PROGRESS 0x8
#define STS_RESET_FINISHED 0x10
#define CTL_RESET       0x1

/* admin opcodes and status */
#define OP_CREATE_SQ    1
#define OP_DESTROY_SQ   2
#define OP_CREATE_CQ    3
#define OP_DESTROY_CQ   4
#define OP_GET_FEATURE  8
#define OP_SET_FEATURE  9
#define ST_SUCCESS      0
#define ST_BAD_OPCODE   2
#define ST_UNSUPPORTED  3
#define ST_MALFORMED    4
#define ST_ILLEGAL      5

#define F_DEVICE_ATTRIBUTES 1
#define F_MAX_QUEUES_EXT 7
#define F_MTU           14
#define F_AENQ_CONFIG   26

typedef struct EnaCq {
    bool used;
    uint16_t depth;
    uint16_t esize;      /* bytes */
    uint64_t base;
    uint32_t tail;       /* running count of entries the device wrote */
} EnaCq;

typedef struct EnaSq {
    bool used;
    uint16_t depth;
    uint16_t cq;
    uint64_t base;
    uint32_t head;       /* running count of descriptors consumed */
    uint32_t tail;       /* last doorbell value */
} EnaSq;

struct EnaState {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    NICState *nic;
    NICConf conf;
    bool readless;

    uint32_t dev_ctl, dev_sts;
    uint64_t aq_base, acq_base, aenq_base, mmio_resp;
    uint32_t aq_caps, acq_caps, aenq_caps;
    uint32_t aq_head;    /* running */
    uint32_t acq_tail;   /* running */
    uint32_t mtu;
    uint32_t aenq_enabled;

    EnaCq cq[2 * ENA_NQ];
    EnaSq sq[2][ENA_NQ]; /* [0] transmit, [1] receive */

    uint8_t pkt[65536];
    uint32_t pkt_len;
    bool pkt_open;
    uint16_t pkt_req_id;
    bool pkt_comp;
};

#define GE(fmt, ...) qemu_log_mask(LOG_GUEST_ERROR, "ena: " fmt "\n", ##__VA_ARGS__)

static void ena_reset(EnaState *s)
{
    memset(s->cq, 0, sizeof(s->cq));
    memset(s->sq, 0, sizeof(s->sq));
    s->aq_base = s->acq_base = s->aenq_base = s->mmio_resp = 0;
    s->aq_caps = s->acq_caps = s->aenq_caps = 0;
    s->aq_head = s->acq_tail = 0;
    s->mtu = 9216;
    s->aenq_enabled = 0;
    s->pkt_open = false;
    s->pkt_len = 0;
}

static bool dma_r(EnaState *s, uint64_t a, void *b, size_t n)
{
    if (pci_dma_read(PCI_DEVICE(s), a, b, n) != MEMTX_OK) {
        GE("DMA read of %zu bytes at 0x%" PRIx64 " failed", n, a);
        return false;
    }
    return true;
}

static bool dma_w(EnaState *s, uint64_t a, const void *b, size_t n)
{
    if (pci_dma_write(PCI_DEVICE(s), a, b, n) != MEMTX_OK) {
        GE("DMA write of %zu bytes at 0x%" PRIx64 " failed", n, a);
        return false;
    }
    return true;
}

static uint32_t reg_value(EnaState *s, hwaddr off)
{
    switch (off) {
    case R_VERSION:      return 0x0200;                 /* 2.0 */
    case R_CTRL_VERSION: return (0 << 16) | (0 << 8) | 1;
    case R_CAPS:         /* reset timeout 1 (100 ms), 48-bit DMA, admin timeout 3 */
        return (1 << 1) | (48 << 8) | (3 << 16);
    case R_CAPS_EXT:     return 0;
    case R_DEV_STS:      return s->dev_sts;
    case R_DEV_CTL:      return s->dev_ctl;
    case R_AQ_CAPS:      return s->aq_caps;
    case R_ACQ_CAPS:     return s->acq_caps;
    default:             return 0;
    }
}

static uint64_t ena_mmio_read(void *opaque, hwaddr off, unsigned size)
{
    EnaState *s = opaque;
    if (size != 4) {
        GE("register read of %u bytes at 0x%" HWADDR_PRIx, size, off);
        return 0xFFFFFFFF;
    }
    if (s->readless) {
        /* Registers are read through the response buffer only. */
        return 0xFFFFFFFF;
    }
    return reg_value(s, off);
}

/* ---- admin commands ---- */

static void put_u16(uint8_t *p, int at, uint16_t v) { stw_le_p(p + at, v); }
static void put_u32(uint8_t *p, int at, uint32_t v) { stl_le_p(p + at, v); }
static uint16_t get_u16(const uint8_t *p, int at) { return lduw_le_p(p + at); }
static uint32_t get_u32(const uint8_t *p, int at) { return ldl_le_p(p + at); }

/* The 48-bit address of an ena_common_mem_addr at p + at. */
static uint64_t get_addr(const uint8_t *p, int at)
{
    return (uint64_t)get_u32(p, at) | ((uint64_t)get_u16(p, at + 4) << 32);
}

static void admin_cmd(EnaState *s, const uint8_t *cmd, uint8_t *resp, uint8_t *status)
{
    uint8_t op = cmd[2];
    memset(resp, 0, 64);
    *status = ST_SUCCESS;

    if (cmd[3] & 2) {
        /* a control buffer: none of the commands here takes one */
        if (op == OP_GET_FEATURE || op == OP_SET_FEATURE) {
            GE("admin: control buffer given to opcode %u", op);
            *status = ST_MALFORMED;
            return;
        }
    }

    switch (op) {
    case OP_GET_FEATURE: {
        uint8_t id = cmd[17];
        switch (id) {
        case F_DEVICE_ATTRIBUTES:
            put_u32(resp, 8, 0x10);                         /* impl_id */
            put_u32(resp, 12, 0x0200);                      /* device_version */
            put_u32(resp, 16, (1u << F_MAX_QUEUES_EXT) | (1u << F_MTU) | (1u << F_AENQ_CONFIG));
            put_u32(resp, 20, 0);                           /* capabilities */
            put_u32(resp, 24, 48);                          /* phys_addr_width */
            put_u32(resp, 28, 48);
            memcpy(resp + 32, s->conf.macaddr.a, 6);
            put_u32(resp, 40, 9216);                        /* max_mtu */
            break;
        case F_MAX_QUEUES_EXT:
            if (cmd[18] != 1) {
                GE("admin: max queues ext feature version %u", cmd[18]);
            }
            resp[8] = 1;                                    /* version */
            put_u32(resp, 12, ENA_NQ);                      /* max_tx_sq_num */
            put_u32(resp, 16, ENA_NQ);
            put_u32(resp, 20, ENA_NQ);
            put_u32(resp, 24, ENA_NQ);
            put_u32(resp, 28, 1024);                        /* max_tx_sq_depth */
            put_u32(resp, 32, 1024);
            put_u32(resp, 36, 1024);
            put_u32(resp, 40, 1024);
            put_u32(resp, 44, 96);                          /* max_tx_header_size */
            put_u16(resp, 48, 16);                          /* max_per_packet_tx_descs */
            put_u16(resp, 50, 16);
            break;
        case F_AENQ_CONFIG:
            put_u32(resp, 8, 0x1f);                         /* supported_groups */
            put_u32(resp, 12, s->aenq_enabled);
            break;
        case F_MTU:
            put_u32(resp, 8, s->mtu);
            break;
        default:
            *status = ST_UNSUPPORTED;
        }
        break;
    }
    case OP_SET_FEATURE: {
        uint8_t id = cmd[17];
        switch (id) {
        case F_AENQ_CONFIG:
            s->aenq_enabled = get_u32(cmd, 24);
            if (s->aenq_enabled & ~0x1fu) {
                *status = ST_ILLEGAL;
            }
            break;
        case F_MTU:
            s->mtu = get_u32(cmd, 20);
            if (s->mtu < 128 || s->mtu > 9216) {
                *status = ST_ILLEGAL;
            }
            break;
        default:
            *status = ST_UNSUPPORTED;
        }
        break;
    }
    case OP_CREATE_CQ: {
        uint8_t caps2 = cmd[5] & 0x1f;          /* entry size in 32-bit words */
        uint16_t depth = get_u16(cmd, 6);
        uint64_t base = get_addr(cmd, 12);
        int i;
        if (!(caps2 == 2 || caps2 == 4 || caps2 == 8) || depth == 0
            || (depth & (depth - 1)) || (base & 3) || get_u16(cmd, 18)) {
            GE("admin: bad CREATE_CQ (words %u depth %u base 0x%" PRIx64 ")", caps2, depth, base);
            *status = ST_ILLEGAL;
            break;
        }
        for (i = 0; i < 2 * ENA_NQ && s->cq[i].used; i++) {
        }
        if (i == 2 * ENA_NQ) {
            *status = ST_ILLEGAL;
            break;
        }
        s->cq[i] = (EnaCq){ .used = true, .depth = depth, .esize = caps2 * 4, .base = base };
        put_u16(resp, 8, i);
        put_u16(resp, 10, depth);
        put_u32(resp, 12, 0);                   /* numa node register: none */
        put_u32(resp, 16, 0);
        put_u32(resp, 20, CQ_UNMASK + 4 * i);
        break;
    }
    case OP_CREATE_SQ: {
        int dir = (cmd[4] >> 5) & 7;            /* 1 transmit, 2 receive */
        uint8_t placement = cmd[6] & 0xf;
        uint8_t policy = (cmd[6] >> 4) & 7;
        uint16_t cq = get_u16(cmd, 8);
        uint16_t depth = get_u16(cmd, 10);
        uint64_t base = get_addr(cmd, 12);
        int i;
        if ((dir != 1 && dir != 2) || placement != 1 || policy != 0 || !(cmd[7] & 1)
            || depth == 0 || (depth & (depth - 1)) || (base & 4095)
            || cq >= 2 * ENA_NQ || !s->cq[cq].used) {
            GE("admin: bad CREATE_SQ (dir %d placement %u policy %u cq %u depth %u base 0x%" PRIx64 ")",
               dir, placement, policy, cq, depth, base);
            *status = ST_ILLEGAL;
            break;
        }
        for (i = 0; i < ENA_NQ && s->sq[dir - 1][i].used; i++) {
        }
        if (i == ENA_NQ) {
            *status = ST_ILLEGAL;
            break;
        }
        s->sq[dir - 1][i] = (EnaSq){ .used = true, .depth = depth, .cq = cq, .base = base };
        put_u16(resp, 8, i);
        put_u32(resp, 12, (dir == 1 ? DB_TX : DB_RX) + 4 * i);
        break;
    }
    case OP_DESTROY_SQ: {
        uint16_t idx = get_u16(cmd, 4);
        int dir = (cmd[6] >> 5) & 7;
        if ((dir != 1 && dir != 2) || idx >= ENA_NQ || !s->sq[dir - 1][idx].used) {
            *status = ST_ILLEGAL;
            break;
        }
        s->sq[dir - 1][idx].used = false;
        break;
    }
    case OP_DESTROY_CQ: {
        uint16_t idx = get_u16(cmd, 4);
        if (idx >= 2 * ENA_NQ || !s->cq[idx].used) {
            *status = ST_ILLEGAL;
            break;
        }
        s->cq[idx].used = false;
        break;
    }
    default:
        GE("admin: unknown opcode %u", op);
        *status = ST_BAD_OPCODE;
    }
}

static void admin_run(EnaState *s, uint32_t tail)
{
    uint16_t qd = s->aq_caps & 0xffff;
    uint16_t qe = s->aq_caps >> 16;
    uint16_t cd = s->acq_caps & 0xffff;
    uint16_t ce = s->acq_caps >> 16;

    if (!qd || (qd & (qd - 1)) || qd > ENA_ADMIN_DEPTH_MAX || qe != 64 || cd != qd || ce != 64
        || !s->aq_base || !s->acq_base || (s->aq_base & 3) || (s->acq_base & 3)) {
        GE("admin: queue not set up (aq depth %u entry %u, acq depth %u entry %u)", qd, qe, cd, ce);
        return;
    }
    if ((uint16_t)(tail - s->aq_head) > qd) {
        GE("admin: doorbell %u with head %u: queue overrun", tail, s->aq_head);
        return;
    }
    while (s->aq_head != tail) {
        uint8_t cmd[64], resp[64], status;
        uint32_t slot = s->aq_head % qd;
        uint8_t want_phase = ((s->aq_head / qd) & 1) ^ 1;   /* first pass is phase 1 */
        uint16_t id;
        if (!dma_r(s, s->aq_base + slot * 64, cmd, 64)) {
            return;
        }
        if ((cmd[3] & 1) != want_phase) {
            GE("admin: command %u has phase %u, expected %u", s->aq_head, cmd[3] & 1, want_phase);
        }
        id = get_u16(cmd, 0) & 0xfff;
        admin_cmd(s, cmd, resp, &status);
        put_u16(resp, 0, id);
        resp[2] = status;
        put_u16(resp, 4, 0);
        s->aq_head++;
        put_u16(resp, 6, s->aq_head % qd);
        resp[3] = ((s->acq_tail / qd) & 1) ^ 1;             /* phase */
        if (!dma_w(s, s->acq_base + (s->acq_tail % qd) * 64, resp, 64)) {
            return;
        }
        s->acq_tail++;
    }
}

/* ---- data path ---- */

static void cq_put(EnaState *s, int cq, const uint8_t *entry)
{
    EnaCq *c = &s->cq[cq];
    uint8_t e[32];
    uint32_t slot = c->tail % c->depth;
    memcpy(e, entry, c->esize);
    /* the phase bit is the device's: bit 0 of byte 3 (transmit) / bit 24 of word 0 (receive) is set by caller */
    dma_w(s, c->base + (uint64_t)slot * c->esize, e, c->esize);
    c->tail++;
}

static uint8_t cq_phase(EnaCq *c)
{
    return ((c->tail / c->depth) & 1) ^ 1;
}

static void tx_run(EnaState *s, int q, uint32_t tail)
{
    EnaSq *sq = &s->sq[0][q];
    if (!sq->used) {
        GE("tx doorbell for queue %d, which does not exist", q);
        return;
    }
    if ((uint16_t)(tail - sq->head) > sq->depth - 1) {
        GE("tx doorbell %u with head %u: ring overrun", tail, sq->head);
        return;
    }
    sq->tail = tail;
    while ((uint16_t)sq->head != (uint16_t)tail) {
        uint8_t d[16];
        uint32_t slot = sq->head % sq->depth;
        uint32_t len_ctrl, meta_ctrl, addr_lo, hi_hdr;
        uint8_t want = ((sq->head / sq->depth) & 1) ^ 1;
        uint32_t len;
        uint64_t addr;
        if (!dma_r(s, sq->base + slot * 16, d, 16)) {
            return;
        }
        sq->head++;
        len_ctrl = ldl_le_p(d);
        meta_ctrl = ldl_le_p(d + 4);
        addr_lo = ldl_le_p(d + 8);
        hi_hdr = ldl_le_p(d + 12);
        if (((len_ctrl >> 24) & 1) != want) {
            GE("tx descriptor %u: phase %u, expected %u", sq->head - 1, (len_ctrl >> 24) & 1, want);
            continue;
        }
        if (len_ctrl & (1 << 23)) {
            continue;                       /* a metadata descriptor */
        }
        if (len_ctrl & (1u << 26)) {
            s->pkt_open = true;
            s->pkt_len = 0;
            s->pkt_req_id = ((meta_ctrl >> 22) & 0x3ff) | (((len_ctrl >> 16) & 0x3f) << 10);
            s->pkt_comp = (len_ctrl >> 28) & 1;
        } else if (!s->pkt_open) {
            GE("tx descriptor without a first descriptor before it");
            continue;
        }
        len = len_ctrl & 0xffff;
        addr = addr_lo | ((uint64_t)(hi_hdr & 0xffff) << 32);
        if (hi_hdr >> 24) {
            GE("tx descriptor with header length %u on a host queue", hi_hdr >> 24);
        }
        if (s->pkt_len + len > sizeof(s->pkt)) {
            GE("tx packet too long");
            s->pkt_open = false;
            continue;
        }
        if (!dma_r(s, addr, s->pkt + s->pkt_len, len)) {
            s->pkt_open = false;
            continue;
        }
        s->pkt_len += len;
        if (len_ctrl & (1u << 27)) {
            if (s->pkt_len > 14 + s->mtu) {
                GE("tx packet of %u bytes over the MTU %u", s->pkt_len, s->mtu);
            } else {
                qemu_send_packet(qemu_get_queue(s->nic), s->pkt, s->pkt_len);
            }
            s->pkt_open = false;
            if (s->pkt_comp) {
                uint8_t c[8] = { 0 };
                EnaCq *cq = &s->cq[sq->cq];
                put_u16(c, 0, s->pkt_req_id);
                c[2] = 0;
                c[3] = cq_phase(cq);
                put_u16(c, 4, q);
                put_u16(c, 6, sq->head % sq->depth);
                cq_put(s, sq->cq, c);
            }
        }
    }
}

static bool rx_have(EnaState *s)
{
    EnaSq *sq = &s->sq[1][0];
    return sq->used && (uint16_t)(sq->tail - sq->head) != 0;
}

static bool ena_can_receive(NetClientState *nc)
{
    return rx_have(qemu_get_nic_opaque(nc));
}

static ssize_t ena_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    EnaState *s = qemu_get_nic_opaque(nc);
    EnaSq *sq = &s->sq[1][0];
    size_t done = 0;
    bool first = true;

    if (!rx_have(s)) {
        return 0;
    }
    if (size > 14 + s->mtu + 4) {
        return size;                         /* dropped: over the MTU */
    }
    while (done < size) {
        uint8_t d[16], c[16] = { 0 };
        uint32_t slot, len, ctrl, w;
        uint64_t addr;
        uint8_t want;
        EnaCq *cq = &s->cq[sq->cq];
        if (!rx_have(s)) {
            GE("rx: packet of %zu bytes needs more buffers than the driver posted", size);
            return size;
        }
        slot = sq->head % sq->depth;
        want = ((sq->head / sq->depth) & 1) ^ 1;
        if (!dma_r(s, sq->base + slot * 16, d, 16)) {
            return size;
        }
        len = lduw_le_p(d);
        if (len == 0) {
            len = 65536;
        }
        ctrl = d[3];
        if ((ctrl & 1) != want) {
            GE("rx descriptor %u: phase %u, expected %u", sq->head, ctrl & 1, want);
            return size;
        }
        if (!(ctrl & 4) || !(ctrl & 8) || lduw_le_p(d + 6) || ldl_le_p(d + 12) >> 16) {
            GE("rx descriptor %u has wrong control bits 0x%x", sq->head, ctrl);
        }
        addr = ldl_le_p(d + 8) | ((uint64_t)lduw_le_p(d + 12) << 32);
        if (len > size - done) {
            len = size - done;
        }
        if (!dma_w(s, addr, buf + done, len)) {
            return size;
        }
        done += len;
        sq->head++;
        w = (cq_phase(cq) << 24) | ((uint32_t)first << 26) | ((uint32_t)(done == size) << 27) | (1u << 30);
        first = false;
        put_u32(c, 0, w);
        put_u16(c, 4, len);
        put_u16(c, 6, lduw_le_p(d + 4));      /* req_id */
        put_u32(c, 8, 0);
        put_u16(c, 12, 0);
        c[14] = 0;                            /* offset */
        cq_put(s, sq->cq, c);
    }
    return size;
}

static void rx_doorbell(EnaState *s, int q, uint32_t tail)
{
    EnaSq *sq = &s->sq[1][q];
    if (!sq->used) {
        GE("rx doorbell for queue %d, which does not exist", q);
        return;
    }
    if ((uint16_t)(tail - sq->head) > sq->depth - 1) {
        GE("rx doorbell %u with head %u: ring overrun", tail, sq->head);
        return;
    }
    sq->tail = tail;
    if (q == 0) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static void ena_mmio_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    EnaState *s = opaque;
    if (size != 4) {
        GE("register write of %u bytes at 0x%" HWADDR_PRIx, size, off);
        return;
    }
    if (off >= DB_TX && off < DB_TX + 4 * ENA_NQ) {
        tx_run(s, (off - DB_TX) / 4, v & 0xffff);
        return;
    }
    if (off >= DB_RX && off < DB_RX + 4 * ENA_NQ) {
        rx_doorbell(s, (off - DB_RX) / 4, v & 0xffff);
        return;
    }
    if (off >= CQ_UNMASK && off < CQ_UNMASK + 4 * 2 * ENA_NQ) {
        return;
    }
    switch (off) {
    case R_AQ_BASE_LO:   s->aq_base = (s->aq_base & ~0xffffffffull) | v; break;
    case R_AQ_BASE_HI:   s->aq_base = (s->aq_base & 0xffffffffull) | (v << 32); break;
    case R_AQ_CAPS:      s->aq_caps = v; break;
    case R_ACQ_BASE_LO:  s->acq_base = (s->acq_base & ~0xffffffffull) | v; break;
    case R_ACQ_BASE_HI:  s->acq_base = (s->acq_base & 0xffffffffull) | (v << 32); break;
    case R_ACQ_CAPS:     s->acq_caps = v; break;
    case R_AQ_DB:        admin_run(s, v & 0xffff); break;
    case R_AENQ_CAPS:    s->aenq_caps = v; break;
    case R_AENQ_BASE_LO: s->aenq_base = (s->aenq_base & ~0xffffffffull) | v; break;
    case R_AENQ_BASE_HI: s->aenq_base = (s->aenq_base & 0xffffffffull) | (v << 32); break;
    case R_AENQ_HEAD_DB: break;
    case R_INTR_MASK:    break;
    case R_MMIO_RESP_LO: s->mmio_resp = (s->mmio_resp & ~0xffffffffull) | v; break;
    case R_MMIO_RESP_HI: s->mmio_resp = (s->mmio_resp & 0xffffffffull) | (v << 32); break;
    case R_DEV_CTL:
        if (v & CTL_RESET) {
            ena_reset(s);
            s->dev_ctl = v;
            s->dev_sts = STS_READY | STS_RESET_IN_PROGRESS;
        } else {
            s->dev_ctl = v;
            s->dev_sts = STS_READY | ((s->dev_sts & STS_RESET_IN_PROGRESS) ? STS_RESET_FINISHED : 0);
        }
        break;
    case R_MMIO_READ: {
        uint8_t r[8];
        uint16_t reg = v >> 16;
        if (!s->mmio_resp) {
            GE("readless read with no response buffer set");
            break;
        }
        put_u16(r, 0, v & 0xffff);
        put_u16(r, 2, reg);
        put_u32(r, 4, reg_value(s, reg));
        dma_w(s, s->mmio_resp, r, 8);
        break;
    }
    default:
        GE("write of 0x%" PRIx64 " to unknown register 0x%" HWADDR_PRIx, v, off);
    }
}

static const MemoryRegionOps ena_mmio_ops = {
    .read = ena_mmio_read,
    .write = ena_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 8 },
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

static NetClientInfo ena_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = ena_can_receive,
    .receive = ena_receive,
};

static void ena_realize(PCIDevice *pci, Error **errp)
{
    EnaState *s = ENA(pci);

    pci->config[PCI_REVISION_ID] = s->readless ? 0 : 1;
    pci->config[PCI_INTERRUPT_PIN] = 1;
    memory_region_init_io(&s->bar0, OBJECT(s), &ena_mmio_ops, s, "ena-bar0", ENA_BAR0_SIZE);
    pci_register_bar(pci, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&ena_net_info, &s->conf, object_get_typename(OBJECT(s)),
                          DEVICE(s)->id, &DEVICE(s)->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    ena_reset(s);
    s->dev_sts = STS_READY;
}

static void ena_exit(PCIDevice *pci)
{
    EnaState *s = ENA(pci);
    qemu_del_nic(s->nic);
}

static void ena_qdev_reset(DeviceState *dev)
{
    EnaState *s = ENA(dev);
    ena_reset(s);
    s->dev_ctl = 0;
    s->dev_sts = STS_READY;
}

static Property ena_properties[] = {
    DEFINE_NIC_PROPERTIES(EnaState, conf),
    DEFINE_PROP_BOOL("readless", EnaState, readless, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void ena_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ena_realize;
    k->exit = ena_exit;
    k->vendor_id = 0x1d0f;
    k->device_id = 0xec20;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    dc->reset = ena_qdev_reset;
    dc->desc = "AWS Elastic Network Adapter (test model)";
    device_class_set_props(dc, ena_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo ena_info = {
    .name = TYPE_ENA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(EnaState),
    .class_init = ena_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ena_register_types(void)
{
    type_register_static(&ena_info);
}

type_init(ena_register_types)
