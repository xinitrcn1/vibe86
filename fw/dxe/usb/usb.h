/* UsbDxe's internal interfaces (xhci.c, hid.c, msc.c, main.c) */
#ifndef USB_H
#define USB_H

#include "uefilib.h"
#include "x86.h"
#include "devname.h"

/* limits */
#define MAXPORTS	16
#define MAXSLOTS	16
#define MAXDEV		8
#define CMD_TRBS	32
#define EVT_TRBS	64
#define EP0_TRBS	32
#define INT_TRBS	16
#define BULK_TRBS	32
#define BOUNCE		1024
#define MSC_BUF		16384		/* 16 KiB aligned: never crosses 64 KiB */

/* timeouts (TSC microseconds) */
#define T_CMD_US	100000
#define T_CTL_US	500000
#define T_BULK_US	2000000
#define T_RESET_US	1000000
#define T_HALT_US	20000		/* R/S changes (xHCI 5.4.1: 16 ms) */
#define T_PORT_US	200000

/* errors */
#define USB_ENODEV	(-1)
#define USB_ETIMEDOUT	(-2)
#define USB_ESTALL	(-3)
#define USB_EIO		(-4)
#define USB_ENOMEM	(-5)

/* operational registers (xHCI 5.4) */
#define USBCMD		0x00
#define USBSTS		0x04
#define PAGESIZE	0x08
#define CRCR		0x18
#define DCBAAP		0x30
#define CONFIG		0x38
#define PORTSC(n)	(0x400 + 0x10 * ((n) - 1))
#define CMD_RS		0x1
#define CMD_HCRST	0x2
#define STS_HCH		0x1
#define STS_HSE		0x4
#define STS_CNR		0x800
#define STS_HCE		0x1000
#define CRCR_CA		0x4
#define CRCR_CRR	0x8
#define PS_CCS		(1u << 0)
#define PS_PED		(1u << 1)
#define PS_PR		(1u << 4)
#define PS_PP		(1u << 9)
#define PS_SPEED(v)	(((v) >> 10) & 15)
#define PS_PIC		(3u << 14)
#define PS_CHANGE	(0x7Fu << 17)
#define PS_PRC		(1u << 21)
#define PS_WAKE		(7u << 25)
#define PS_KEEP		(PS_PP | PS_PIC | PS_WAKE)
#define IMAN		0x20
#define ERSTSZ		0x28
#define ERSTBA		0x30
#define ERDP		0x38
#define ERDP_EHB	0x8

#define SPEED_FULL	1
#define SPEED_LOW	2
#define SPEED_HIGH	3

/* TRBs (6.4) */
#define TRB_NORMAL	1
#define TRB_SETUP	2
#define TRB_DATA	3
#define TRB_STATUS	4
#define TRB_LINK	6
#define TRB_ENABLE_SLOT	9
#define TRB_ADDRESS	11
#define TRB_CONFIG_EP	12
#define TRB_EVAL_CTX	13
#define TRB_RESET_EP	14
#define TRB_STOP_EP	15
#define TRB_SET_DEQ	16
#define EV_TRANSFER	32
#define EV_CMD		33
#define EV_PORT		34
#define T_TYPE(t)	((uint32_t)(t) << 10)
#define T_TC		(1u << 1)
#define T_ISP		(1u << 2)
#define T_CH		(1u << 4)
#define T_IOC		(1u << 5)
#define T_IDT		(1u << 6)
#define T_DIR_IN	(1u << 16)
#define T_SLOT(s)	((uint32_t)(s) << 24)
#define T_EP(e)		((uint32_t)(e) << 16)

#define CC_SUCCESS	1
#define CC_STALL	6
#define CC_SHORT	13
#define CC_STOPPED	26
#define CC_STOPPED_LEN	27

#define EP_STATE_RUNNING 1
#define EP_STATE_HALTED	2

struct trb {
	uint64_t p;
	uint32_t s, c;
};
typedef volatile struct trb vtrb;

struct ring {
	vtrb *t;
	unsigned n, i;
	uint32_t cyc;
};

enum { K_OTHER, K_KBD, K_MOUSE, K_MSC };

struct udev {
	int port, slot, speed, kind;
	unsigned iface, mps0;
	char name[48];
	struct ring ep0;
	volatile uint32_t *in, *out;
	/* the control transfer in flight */
	uint64_t c_data, c_status;
	int c_done, c_cc;
	uint32_t c_resid;
	/* interrupt IN (HID) */
	unsigned int_dci, int_mps;
	struct ring ir;
	volatile uint8_t *ibuf;
	uint64_t i_trb;
	int i_busy, i_err, i_fail;
	uint8_t prev[8];
	/* bulk (mass storage) */
	unsigned bin_dci, bout_dci, bin_addr, bout_addr;
	struct ring bin, bout;
	uint64_t x_trb;
	int x_done, x_cc;
	uint32_t x_resid;
	void *priv;			/* the UEFI side: hid.c / msc.c */
};

struct hc {
	int ok, in_poll;
	uintptr_t op, rt, db, cap;
	unsigned maxslots, maxports, ctxsz;
	volatile uint64_t *dcbaa;
	struct ring cmd;
	vtrb *er;
	unsigned er_i;
	uint32_t er_cyc;
	uint8_t proto[MAXPORTS + 1];
	uint64_t cmd_ptr;
	int cmd_done, cmd_cc, cmd_slot;
	volatile uint8_t *bounce;
	struct udev dev[MAXDEV];
	int ndev;
	EFI_DEVICE_PATH_PROTOCOL *path;	/* the controller's */
	VIBE86_USB_INFO info;
};
extern struct hc hc;

/* xhci.c */
void *dma(unsigned long size, unsigned long align);
volatile uint32_t *ctx(volatile uint32_t *base, unsigned idx);
int ring_init(struct ring *r, unsigned n);
vtrb *ring_put(struct ring *r, uint64_t p, uint32_t s, uint32_t c, int hold, uint32_t *cw);
uint64_t ring_deq(const struct ring *r);
void doorbell(unsigned slot, unsigned target);
void events(void);
int wait_flag(volatile int *flag, uint64_t us);
int command(uint64_t p, uint32_t s, uint32_t c, int *slot);
int ep_recover(struct udev *d, unsigned dci, const struct ring *r);
int control(struct udev *d, int rt, int req, int val, int idx, void *data, int len);
int bulk(struct udev *d, int in, volatile void *buf, uint32_t len);
void int_queue(struct udev *d);
int xhci_init(uint64_t bar0);
void xhci_halt(void);
void usb_poll(void);

/* hid.c */
void hid_start(struct udev *d);
void hid_poll(void);
void key_report(struct udev *d, const volatile uint8_t *r, unsigned n);
void mouse_report(struct udev *d, const volatile uint8_t *r, unsigned n);

/* msc.c */
int msc_start(struct udev *d);

/* main.c */
EFI_HANDLE usb_install(struct udev *d, EFI_GUID *g1, void *i1, EFI_GUID *g2, void *i2);

#endif
