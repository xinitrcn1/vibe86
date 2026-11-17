/*
 * PciDxe: PCI Express enumeration and the PCI I/O protocol (UEFI 2.10 ch. 14).
 *
 * The root complex (docs/platform.md "PCIe"): bus 0 holds the i440FX/PIIX
 * functions and the root ports (00:10.0-00:17.0), one slot below each.
 * Config space through ECAM at E000_0000h (PCIe Base 5.0 7.2.2). Bus
 * numbers are assigned depth-first; memory BARs are packed, largest first,
 * into 1 MiB-aligned bridge windows: 32-bit and non-prefetchable 64-bit BARs
 * in the 32-bit window (C000_0000h, 512 MiB), prefetchable 64-bit BARs in
 * the 64-bit window (2 TiB, 64 GiB) through the bridges' prefetchable
 * windows (PCI-to-PCI Bridge Architecture Specification 1.2, 3.2.5.8-10).
 *
 * Decoding stays off until a driver asks for it through PciIo->Attributes:
 * enabling memory space or bus mastering on a function also enables it on
 * every bridge above it. The vibe86 root ports enforce Bus Master Enable
 * (PCIe Base 7.5.1.1.3), so a device's DMA only reaches memory when its
 * root port has BME set too (fw/README.md).
 */
#include "uefilib.h"
#include "x86.h"
#include "devname.h"

#define MAX_FUNCS	64
#define WIN_ALIGN	0x100000ull

enum { BAR_NONE, BAR_MEM32, BAR_MEM64, BAR_PREF64, BAR_IO };

struct pci_func;

struct bar {
	int type;
	uint64_t size, addr;
};

struct pci_func {
	EFI_PCI_IO_PROTOCOL io;		/* first: the protocol's This */
	unsigned bus, dev, fn;
	uint16_t vendor, device;
	uint32_t class;			/* class << 16 | subclass << 8 | prog-if */
	int bridge;
	unsigned secondary, subordinate;
	struct pci_func *parent;
	struct bar bars[6];
	uint64_t attributes;
	uint64_t mem_need, pref_need;	/* bridges: the window sizes */
	uint64_t mem_base, pref_base;
	EFI_DEVICE_PATH_PROTOCOL *path;
	VIBE86_DEVICE_NAME name;
	EFI_HANDLE handle;
};

static struct pci_func funcs[MAX_FUNCS];
static unsigned nfuncs;
static unsigned next_bus;

static uint32_t cfg_r32(const struct pci_func *f, unsigned r) { return pci_r32(f->bus, f->dev, f->fn, r); }
static void cfg_w32(const struct pci_func *f, unsigned r, uint32_t v) { pci_w32(f->bus, f->dev, f->fn, r, v); }
static uint16_t cfg_r16(const struct pci_func *f, unsigned r) { return pci_r16(f->bus, f->dev, f->fn, r); }
static void cfg_w16(const struct pci_func *f, unsigned r, uint16_t v) { pci_w16(f->bus, f->dev, f->fn, r, v); }

const char *
pci_class_name(uint32_t class)
{
	switch (class >> 8) {
	case 0x0600: return "Host bridge";
	case 0x0601: return "ISA bridge";
	case 0x0604: return "PCI Express root port";
	case 0x0680: return "Bridge";
	case 0x0C03: return (class & 0xFF) == 0x30 ? "USB xHCI controller" : "USB controller";
	case 0x0880: return "System peripheral";
	case 0x0100: case 0x0106: case 0x0108: return "Storage controller";
	case 0x0200: return "Ethernet controller";
	case 0x0300: return "Display controller";
	}
	return "Device";
}

/* ---------------------------------------------------------------- enumeration */

static void
size_bars(struct pci_func *f)
{
	unsigned nbars = f->bridge ? 2 : 6;
	uint16_t cmd = cfg_r16(f, 4);
	cfg_w16(f, 4, cmd & ~3);		/* no decoding while sizing */
	for (unsigned i = 0; i < nbars; i++) {
		unsigned r = 0x10 + 4 * i;
		uint32_t old = cfg_r32(f, r);
		cfg_w32(f, r, 0xFFFFFFFF);
		uint32_t v = cfg_r32(f, r);
		cfg_w32(f, r, old);
		if (!v || v == 0xFFFFFFFF)
			continue;
		if (v & 1) {
			f->bars[i].type = BAR_IO;
			f->bars[i].size = (uint64_t)(~(v & ~3u) + 1) & 0xFFFF;
			continue;
		}
		int is64 = ((v >> 1) & 3) == 2;
		uint64_t mask = v & ~0xFull;
		if (is64 && i + 1 < nbars) {
			uint32_t oldh = cfg_r32(f, r + 4);
			cfg_w32(f, r + 4, 0xFFFFFFFF);
			mask |= (uint64_t)cfg_r32(f, r + 4) << 32;
			cfg_w32(f, r + 4, oldh);
		} else
			mask |= 0xFFFFFFFF00000000ull;
		f->bars[i].size = ~mask + 1;
		f->bars[i].type = !is64 ? BAR_MEM32 : (v & 8) ? BAR_PREF64 : BAR_MEM64;
		if (is64)
			i++;
	}
}

static void scan_bus(unsigned bus, struct pci_func *parent);

static struct pci_func *
add_func(unsigned bus, unsigned dev, unsigned fn, struct pci_func *parent)
{
	if (nfuncs == MAX_FUNCS)
		return 0;
	struct pci_func *f = &funcs[nfuncs++];
	f->bus = bus;
	f->dev = dev;
	f->fn = fn;
	f->parent = parent;
	f->vendor = cfg_r16(f, 0);
	f->device = cfg_r16(f, 2);
	f->class = cfg_r32(f, 8) >> 8;
	f->bridge = (pci_r8(bus, dev, fn, 0x0E) & 0x7F) == 1;
	size_bars(f);
	if (f->bridge) {
		/* depth-first bus numbers (PCI-to-PCI Bridge 1.2, 3.2.5.3-5) */
		f->secondary = ++next_bus;
		cfg_w32(f, 0x18, (uint32_t)bus | f->secondary << 8 | 0xFFu << 16);
		scan_bus(f->secondary, f);
		f->subordinate = next_bus;
		cfg_w32(f, 0x18, (uint32_t)bus | f->secondary << 8 | f->subordinate << 16);
	}
	return f;
}

static void
scan_bus(unsigned bus, struct pci_func *parent)
{
	for (unsigned dev = 0; dev < 32; dev++) {
		if (pci_r16(bus, dev, 0, 0) == 0xFFFF)
			continue;
		unsigned nfn = (pci_r8(bus, dev, 0, 0x0E) & 0x80) ? 8 : 1;
		for (unsigned fn = 0; fn < nfn; fn++)
			if (pci_r16(bus, dev, fn, 0) != 0xFFFF)
				add_func(bus, dev, fn, parent);
	}
}

/* ---------------------------------------------------------------- resources */

static int
children(const struct pci_func *b, struct pci_func **out)
{
	int n = 0;
	for (unsigned i = 0; i < nfuncs; i++)
		if (funcs[i].parent == b)
			out[n++] = &funcs[i];
	return n;
}

/* the size a bridge's windows need: its children's BARs and windows */
static void
measure(struct pci_func *b)
{
	struct pci_func *c[MAX_FUNCS];
	int n = children(b, c);
	uint64_t mem = 0, pref = 0;
	for (int i = 0; i < n; i++) {
		if (c[i]->bridge) {
			measure(c[i]);
			mem = ALIGN_UP(mem, WIN_ALIGN) + c[i]->mem_need;
			pref = ALIGN_UP(pref, WIN_ALIGN) + c[i]->pref_need;
		}
		for (int k = 0; k < 6; k++) {
			struct bar *r = &c[i]->bars[k];
			if (r->type == BAR_MEM32 || r->type == BAR_MEM64)
				mem = ALIGN_UP(mem, r->size) + r->size;
			else if (r->type == BAR_PREF64)
				pref = ALIGN_UP(pref, r->size) + r->size;
		}
	}
	b->mem_need = ALIGN_UP(mem, WIN_ALIGN);
	b->pref_need = ALIGN_UP(pref, WIN_ALIGN);
}

static void
write_bar(struct pci_func *f, unsigned i)
{
	struct bar *r = &f->bars[i];
	unsigned reg = 0x10 + 4 * i;
	cfg_w32(f, reg, (uint32_t)r->addr | (cfg_r32(f, reg) & 0xF));
	if (r->type == BAR_MEM64 || r->type == BAR_PREF64)
		cfg_w32(f, reg + 4, (uint32_t)(r->addr >> 32));
}

/* place the BARs and windows below bridge b (or the root: b = NULL) */
static void
assign(struct pci_func *b, uint64_t mem, uint64_t pref)
{
	struct pci_func *c[MAX_FUNCS];
	int n = children(b, c);
	/* windows first (1 MiB aligned), then BARs largest first */
	for (int i = 0; i < n; i++) {
		if (!c[i]->bridge)
			continue;
		mem = ALIGN_UP(mem, WIN_ALIGN);
		pref = ALIGN_UP(pref, WIN_ALIGN);
		c[i]->mem_base = mem;
		c[i]->pref_base = pref;
		assign(c[i], mem, pref);
		/* program the windows: memory base/limit (1 MiB units in [15:4]),
		   prefetchable base/limit with the upper 32 bits */
		struct pci_func *br = c[i];
		if (br->mem_need) {
			uint64_t lim = mem + br->mem_need - 1;
			cfg_w32(br, 0x20, (uint32_t)((mem >> 16) & 0xFFF0) | (uint32_t)(lim & 0xFFF00000));
		} else
			cfg_w32(br, 0x20, 0x0000FFF0);		/* base > limit: closed */
		if (br->pref_need) {
			uint64_t lim = pref + br->pref_need - 1;
			cfg_w32(br, 0x24, (uint32_t)((pref >> 16) & 0xFFF0) | (uint32_t)(lim & 0xFFF00000) | 0x10001);
			cfg_w32(br, 0x28, (uint32_t)(pref >> 32));
			cfg_w32(br, 0x2C, (uint32_t)(lim >> 32));
		} else {
			cfg_w32(br, 0x24, 0x0000FFF1);
			cfg_w32(br, 0x28, 0xFFFFFFFF);
			cfg_w32(br, 0x2C, 0);
		}
		mem += br->mem_need;
		pref += br->pref_need;
	}
	for (;;) {
		struct bar *best = 0;
		struct pci_func *bf = 0;
		unsigned bi = 0;
		for (int i = 0; i < n; i++)
			for (unsigned k = 0; k < 6; k++) {
				struct bar *r = &c[i]->bars[k];
				if (r->type == BAR_NONE || r->type == BAR_IO || r->addr)
					continue;
				if (!best || r->size > best->size)
					best = r, bf = c[i], bi = k;
			}
		if (!best)
			break;
		if (best->type == BAR_PREF64) {
			pref = ALIGN_UP(pref, best->size);
			best->addr = pref;
			pref += best->size;
		} else {
			mem = ALIGN_UP(mem, best->size);
			best->addr = mem;
			mem += best->size;
		}
		write_bar(bf, bi);
	}
}

/* ---------------------------------------------------------------- PCI I/O */

#define F(p) ((struct pci_func *)(p))

static unsigned
width_bytes(EFI_PCI_IO_PROTOCOL_WIDTH w)
{
	return 1u << (w & 3);
}

static EFI_STATUS
mem_access(struct pci_func *f, int we, EFI_PCI_IO_PROTOCOL_WIDTH w, UINT8 bar, UINT64 off, UINTN count,
    void *buf)
{
	if (bar >= 6 || f->bars[bar].type == BAR_NONE || f->bars[bar].type == BAR_IO || w >= EfiPciIoWidthMaximum)
		return EFI_INVALID_PARAMETER;
	unsigned n = width_bytes(w);
	int fifo = w >= EfiPciIoWidthFifoUint8 && w <= EfiPciIoWidthFifoUint64;
	int fill = w >= EfiPciIoWidthFillUint8;
	if (off + (fifo ? n : (uint64_t)n * count) > f->bars[bar].size)
		return EFI_UNSUPPORTED;
	uint64_t a = f->bars[bar].addr + off;
	UINT8 *b = buf;
	for (UINTN i = 0; i < count; i++) {
		uint64_t addr = fifo ? a : a + (uint64_t)i * n;
		UINT8 *p = fill ? b : b + (uint64_t)i * n;
		if (we) {
			switch (n) {
			case 1: mmio_w8(addr, *p); break;
			case 2: mmio_w16(addr, *(UINT16 *)p); break;
			case 4: mmio_w32(addr, *(UINT32 *)p); break;
			default: mmio_w64(addr, *(UINT64 *)p); break;
			}
		} else {
			switch (n) {
			case 1: *p = mmio_r8(addr); break;
			case 2: *(UINT16 *)p = mmio_r16(addr); break;
			case 4: *(UINT32 *)p = mmio_r32(addr); break;
			default: *(UINT64 *)p = mmio_r64(addr); break;
			}
		}
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
mem_read(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 Bar, UINT64 Off, UINTN Count, VOID *Buf)
{
	return mem_access(F(This), 0, W, Bar, Off, Count, Buf);
}

static EFI_STATUS EFIAPI
mem_write(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 Bar, UINT64 Off, UINTN Count, VOID *Buf)
{
	return mem_access(F(This), 1, W, Bar, Off, Count, Buf);
}

static EFI_STATUS EFIAPI
io_read(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 Bar, UINT64 Off, UINTN Count, VOID *Buf)
{
	return EFI_UNSUPPORTED;		/* no I/O windows below the root ports */
}

static EFI_STATUS EFIAPI
io_write(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 Bar, UINT64 Off, UINTN Count, VOID *Buf)
{
	return EFI_UNSUPPORTED;
}

static EFI_STATUS
cfg_access(struct pci_func *f, int we, EFI_PCI_IO_PROTOCOL_WIDTH w, UINT32 off, UINTN count, void *buf)
{
	unsigned n = width_bytes(w);
	if (w > EfiPciIoWidthUint32 || off + (uint64_t)n * count > 4096 || (off & (n - 1)))
		return EFI_UNSUPPORTED;
	UINT8 *p = buf;
	for (UINTN i = 0; i < count; i++, off += n, p += n) {
		uint64_t a = pci_ecam(f->bus, f->dev, f->fn, off);
		if (we) {
			if (n == 1) mmio_w8(a, *p);
			else if (n == 2) mmio_w16(a, *(UINT16 *)p);
			else mmio_w32(a, *(UINT32 *)p);
		} else {
			if (n == 1) *p = mmio_r8(a);
			else if (n == 2) *(UINT16 *)p = mmio_r16(a);
			else *(UINT32 *)p = mmio_r32(a);
		}
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
pci_read(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 Off, UINTN Count, VOID *Buf)
{
	return cfg_access(F(This), 0, W, Off, Count, Buf);
}

static EFI_STATUS EFIAPI
pci_write(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 Off, UINTN Count, VOID *Buf)
{
	return cfg_access(F(This), 1, W, Off, Count, Buf);
}

static EFI_STATUS EFIAPI
poll_mem(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 Bar, UINT64 Off, UINT64 Mask,
    UINT64 Value, UINT64 Delay, UINT64 *Result)
{
	uint64_t end = time_us() + Delay / 10;
	for (;;) {
		UINT64 v = 0;
		EFI_STATUS s = mem_access(F(This), 0, W, Bar, Off, 1, &v);
		if (EFI_ERROR(s))
			return s;
		*Result = v;
		if ((v & Mask) == Value)
			return EFI_SUCCESS;
		if (time_us() >= end)
			return EFI_TIMEOUT;
	}
}

static EFI_STATUS EFIAPI
poll_io(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 Bar, UINT64 Off, UINT64 Mask,
    UINT64 Value, UINT64 Delay, UINT64 *Result)
{
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
copy_mem(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 DBar, UINT64 DOff, UINT8 SBar,
    UINT64 SOff, UINTN Count)
{
	unsigned n = width_bytes(W);
	for (UINTN i = 0; i < Count; i++) {
		UINT64 v;
		EFI_STATUS s = mem_access(F(This), 0, W & 3, SBar, SOff + i * n, 1, &v);
		if (!EFI_ERROR(s))
			s = mem_access(F(This), 1, W & 3, DBar, DOff + i * n, 1, &v);
		if (EFI_ERROR(s))
			return s;
	}
	return EFI_SUCCESS;
}

/* DMA: the IOMMU is in bypass and memory is coherent, so addresses map 1:1 */
static EFI_STATUS EFIAPI
map(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_OPERATION Op, VOID *Host, UINTN *Bytes,
    EFI_PHYSICAL_ADDRESS *Dev, VOID **Mapping)
{
	if (!Host || !Bytes || !Dev || !Mapping || Op >= EfiPciIoOperationMaximum)
		return EFI_INVALID_PARAMETER;
	*Dev = (EFI_PHYSICAL_ADDRESS)(uintptr_t)Host;
	*Mapping = Host;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
unmap(EFI_PCI_IO_PROTOCOL *This, VOID *Mapping)
{
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
allocate_buffer(EFI_PCI_IO_PROTOCOL *This, EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MT, UINTN Pages,
    VOID **Host, UINT64 Attr)
{
	if (!Host || (MT != EfiBootServicesData && MT != EfiRuntimeServicesData))
		return EFI_INVALID_PARAMETER;
	EFI_PHYSICAL_ADDRESS a = 0xFFFFFFFF;
	EFI_STATUS s = gBS->AllocatePages(AllocateMaxAddress, MT, Pages, &a);
	if (!EFI_ERROR(s))
		*Host = (void *)(uintptr_t)a;
	return s;
}

static EFI_STATUS EFIAPI
free_buffer(EFI_PCI_IO_PROTOCOL *This, UINTN Pages, VOID *Host)
{
	return gBS->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)Host, Pages);
}

static EFI_STATUS EFIAPI
flush(EFI_PCI_IO_PROTOCOL *This)
{
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
get_location(EFI_PCI_IO_PROTOCOL *This, UINTN *Seg, UINTN *Bus, UINTN *Dev, UINTN *Fn)
{
	if (!Seg || !Bus || !Dev || !Fn)
		return EFI_INVALID_PARAMETER;
	*Seg = 0;
	*Bus = F(This)->bus;
	*Dev = F(This)->dev;
	*Fn = F(This)->fn;
	return EFI_SUCCESS;
}

#define SUPPORTED_ATTRS (EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER | \
	EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE)

/* command register bits for a set of attributes */
static uint16_t
cmd_bits(uint64_t a)
{
	return (uint16_t)(((a & EFI_PCI_IO_ATTRIBUTE_IO) ? 1 : 0) |
	    ((a & (EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)) ? 2 : 0) |
	    ((a & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) ? 4 : 0));
}

static void
apply(struct pci_func *f)
{
	uint16_t want = cmd_bits(f->attributes);
	uint16_t cmd = cfg_r16(f, 4);
	cfg_w16(f, 4, (uint16_t)((cmd & ~7) | want));
	/* the bridges above forward memory and pass DMA upstream */
	for (struct pci_func *b = f->parent; b; b = b->parent) {
		uint16_t bc = cfg_r16(b, 4);
		if ((bc & want) != want)
			cfg_w16(b, 4, bc | want);
	}
}

static EFI_STATUS EFIAPI
attributes(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION Op, UINT64 Attr, UINT64 *Result)
{
	struct pci_func *f = F(This);
	switch (Op) {
	case EfiPciIoAttributeOperationGet:
		if (!Result)
			return EFI_INVALID_PARAMETER;
		*Result = f->attributes;
		return EFI_SUCCESS;
	case EfiPciIoAttributeOperationSupported:
		if (!Result)
			return EFI_INVALID_PARAMETER;
		*Result = SUPPORTED_ATTRS;
		return EFI_SUCCESS;
	case EfiPciIoAttributeOperationSet:
	case EfiPciIoAttributeOperationEnable:
	case EfiPciIoAttributeOperationDisable:
		if (Attr & ~SUPPORTED_ATTRS)
			return EFI_UNSUPPORTED;
		if (Op == EfiPciIoAttributeOperationSet)
			f->attributes = Attr;
		else if (Op == EfiPciIoAttributeOperationEnable)
			f->attributes |= Attr;
		else
			f->attributes &= ~Attr;
		apply(f);
		return EFI_SUCCESS;
	default:
		return EFI_INVALID_PARAMETER;
	}
}

static EFI_STATUS EFIAPI
get_bar_attributes(EFI_PCI_IO_PROTOCOL *This, UINT8 Bar, UINT64 *Supports, VOID **Resources)
{
	struct pci_func *f = F(This);
	if (Bar >= 6 || (!Supports && !Resources))
		return EFI_INVALID_PARAMETER;
	if (Supports)
		*Supports = 0;
	if (!Resources)
		return EFI_SUCCESS;
	/* an ACPI QWORD address space descriptor and an end tag (ACPI 6.5 6.4.3.5.1, 6.4.2.9) */
	UINT8 *d = calloc(1, 46 + 2);
	if (!d)
		return EFI_OUT_OF_RESOURCES;
	struct bar *r = &f->bars[Bar];
	if (r->type == BAR_NONE) {
		d[0] = 0x79;
		*Resources = d;
		return EFI_SUCCESS;
	}
	d[0] = 0x8A;
	d[1] = 43;
	d[3] = r->type == BAR_IO ? 1 : 0;
	d[5] = r->type == BAR_PREF64 ? 6 : 0;		/* cacheable/prefetchable */
	d[6] = r->type == BAR_MEM32 ? 32 : 64;		/* granularity */
	memcpy(d + 14, &r->addr, 8);
	uint64_t max = r->addr + r->size - 1;
	memcpy(d + 22, &max, 8);
	memcpy(d + 38, &r->size, 8);
	d[46] = 0x79;
	*Resources = d;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
set_bar_attributes(EFI_PCI_IO_PROTOCOL *This, UINT64 Attr, UINT8 Bar, UINT64 *Off, UINT64 *Len)
{
	return EFI_UNSUPPORTED;
}

/* ---------------------------------------------------------------- the handles */

static EFI_DEVICE_PATH_PROTOCOL *
make_path(struct pci_func *f)
{
	if (!f)
		return 0;
	EFI_DEVICE_PATH_PROTOCOL *up = f->parent ? f->parent->path : 0;
	EFI_DEVICE_PATH_PROTOCOL *base = up;
	if (!base) {
#pragma pack(push, 1)
		struct {
			ACPI_HID_DEVICE_PATH root;
			EFI_DEVICE_PATH_PROTOCOL end;
		} r;
#pragma pack(pop)
		dp_set_node(&r.root.Header, ACPI_DEVICE_PATH, ACPI_DP, sizeof r.root);
		r.root.HID = EISA_PNP_ID(0x0A03);
		r.root.UID = 0;
		dp_set_end(&r.end);
		base = dp_dup(&r.root.Header);
	}
	PCI_DEVICE_PATH n;
	dp_set_node(&n.Header, HARDWARE_DEVICE_PATH, HW_PCI_DP, sizeof n);
	n.Device = (UINT8)f->dev;
	n.Function = (UINT8)f->fn;
	EFI_DEVICE_PATH_PROTOCOL *p = dp_append_node(base, &n.Header);
	if (!up)
		free(base);
	return p;
}

static void
install(struct pci_func *f)
{
	f->io = (EFI_PCI_IO_PROTOCOL){ poll_mem, poll_io, { mem_read, mem_write }, { io_read, io_write },
	    { pci_read, pci_write }, copy_mem, map, unmap, allocate_buffer, free_buffer, flush,
	    get_location, attributes, get_bar_attributes, set_bar_attributes, 0, 0 };
	f->path = make_path(f);
	snprintf(f->name.name, sizeof f->name.name, "%s %02x:%02x.%x", pci_class_name(f->class), f->bus,
	    f->dev, f->fn);
	static EFI_GUID name_guid = VIBE86_DEVICE_NAME_GUID;
	gBS->InstallMultipleProtocolInterfaces(&f->handle, &gEfiPciIoProtocolGuid, &f->io,
	    &gEfiDevicePathProtocolGuid, f->path, &name_guid, &f->name, (void *)0);
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	uefi_init(image, st);
	next_bus = 0;
	scan_bus(0, 0);
	/* the root's needs, then the windows from the bottom of the apertures */
	struct pci_func root;
	memset(&root, 0, sizeof root);
	for (unsigned i = 0; i < nfuncs; i++)
		if (!funcs[i].parent)
			funcs[i].parent = &root;
	measure(&root);
	if (root.mem_need > PCI_MEM32_SIZE || root.pref_need > PCI_MEM64_SIZE)
		DEBUG("PciDxe: the BARs don't fit the apertures\n");
	assign(&root, PCI_MEM32_BASE, PCI_MEM64_BASE);
	for (unsigned i = 0; i < nfuncs; i++)
		if (funcs[i].parent == &root)
			funcs[i].parent = 0;
	/* parents before children: funcs[] is in that order */
	for (unsigned i = 0; i < nfuncs; i++) {
		struct pci_func *f = &funcs[i];
		install(f);
		DEBUG("PciDxe: %02x:%02x.%x %04x:%04x class %06x %s", f->bus, f->dev, f->fn, f->vendor,
		    f->device, f->class, pci_class_name(f->class));
		if (f->bridge)
			DEBUG(" bus %u-%u mem %lx+%lx", f->secondary, f->subordinate, f->mem_base, f->mem_need);
		DEBUG("\n");
		for (unsigned k = 0; k < 6; k++)
			if (f->bars[k].type != BAR_NONE)
				DEBUG("PciDxe:   BAR%u %016lx size %lx\n", k, f->bars[k].addr, f->bars[k].size);
	}
	return EFI_SUCCESS;
}
