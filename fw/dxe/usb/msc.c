/*
 * UsbDxe: USB mass storage, Bulk-Only Transport ([BOT] USB-IF, "Universal
 * Serial Bus Mass Storage Class Bulk-Only Transport" 1.0, 1999), SCSI
 * commands (T10 SPC/SBC; a USB floppy answers the same ones, UFI 1.0), as
 * the Block I/O protocol (UEFI 2.10 13.9).
 *
 * Each command: a 31-byte CBW on bulk OUT, the data stage, a 13-byte CSW on
 * bulk IN (BOT 5); a stalled data stage is cleared and the CSW still read
 * (BOT 6.7.2); a bad CSW gets the reset recovery of BOT 5.3.4 (Bulk-Only
 * Mass Storage Reset, then CLEAR_FEATURE(ENDPOINT_HALT) on both pipes).
 */
#include "usb.h"

#define CBW_SIG		0x43425355u	/* "USBC" */
#define CSW_SIG		0x53425355u	/* "USBS" */

struct msc {
	EFI_BLOCK_IO_PROTOCOL bio;	/* first: This */
	EFI_BLOCK_IO_MEDIA media;
	struct udev *d;
	volatile uint8_t *cbw, *csw, *buf;
	uint32_t tag;
	VIBE86_DEVICE_NAME name;
};

static void
put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t
get_be32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void
reset_recovery(struct msc *m)
{
	struct udev *d = m->d;
	control(d, 0x21, 0xFF, 0, (int)d->iface, 0, 0);		/* Bulk-Only Mass Storage Reset */
	control(d, 0x02, 1, 0, (int)d->bin_addr, 0, 0);
	control(d, 0x02, 1, 0, (int)d->bout_addr, 0, 0);
}

/*
 * One SCSI command: cdb (12 bytes), the data stage of len bytes in m->buf
 * (in: from the device). Returns the CSW status (0 passed, 1 failed, 2
 * phase error) or < 0 for a transport error.
 */
static int
scsi(struct msc *m, const uint8_t *cdb, unsigned cdblen, int in, uint32_t len)
{
	struct udev *d = m->d;
	if (len > MSC_BUF)
		return USB_EIO;
	volatile uint8_t *w = m->cbw;
	memset((void *)w, 0, 31);
	uint32_t tag = ++m->tag;
	*(volatile uint32_t *)(w + 0) = CBW_SIG;
	*(volatile uint32_t *)(w + 4) = tag;
	*(volatile uint32_t *)(w + 8) = len;
	w[12] = in ? 0x80 : 0x00;
	w[13] = 0;				/* LUN 0 */
	w[14] = (uint8_t)cdblen;
	memcpy((void *)(w + 15), cdb, cdblen);
	int r = bulk(d, 0, w, 31);
	if (r != 31) {
		reset_recovery(m);
		return r < 0 ? r : USB_EIO;
	}
	if (len) {
		r = bulk(d, in, m->buf, len);
		if (r < 0 && r != USB_ESTALL) {
			reset_recovery(m);
			return r;
		}
	}
	r = bulk(d, 1, m->csw, 13);
	if (r == USB_ESTALL)
		r = bulk(d, 1, m->csw, 13);	/* BOT 6.7.2: retry the CSW once */
	if (r != 13 || *(volatile uint32_t *)m->csw != CSW_SIG ||
	    *(volatile uint32_t *)(m->csw + 4) != tag) {
		reset_recovery(m);
		return r < 0 ? r : USB_EIO;
	}
	return m->csw[12];
}

static int
request_sense(struct msc *m, uint8_t *key, uint8_t *asc)
{
	uint8_t cdb[12] = { 0x03, 0, 0, 0, 18 };
	int r = scsi(m, cdb, 12, 1, 18);
	if (r == 0) {
		*key = m->buf[2] & 15;
		*asc = m->buf[12];
	}
	return r;
}

static EFI_STATUS
rw(struct msc *m, int write, EFI_LBA lba, UINTN size, UINT8 *buf)
{
	UINT32 bs = m->media.BlockSize;
	UINTN per = MSC_BUF / bs;
	while (size) {
		UINTN n = MIN(size / bs, per);
		uint8_t cdb[12] = { write ? 0x2A : 0x28 };	/* READ(10) / WRITE(10) */
		put_be32(cdb + 2, (uint32_t)lba);
		cdb[7] = (uint8_t)(n >> 8);
		cdb[8] = (uint8_t)n;
		if (write)
			memcpy((void *)m->buf, buf, n * bs);
		int r = -1;
		for (int tries = 0; tries < 3 && r != 0; tries++) {
			r = scsi(m, cdb, 12, !write, (uint32_t)(n * bs));
			if (r == 1) {
				uint8_t key = 0, asc = 0;
				request_sense(m, &key, &asc);
				if (key == 0x07)
					return EFI_WRITE_PROTECTED;
				if (key == 0x02 && asc == 0x3A) {
					m->media.MediaPresent = FALSE;
					return EFI_NO_MEDIA;
				}
			}
		}
		if (r != 0)
			return EFI_DEVICE_ERROR;
		if (!write)
			memcpy(buf, (const void *)m->buf, n * bs);
		lba += n;
		buf += n * bs;
		size -= n * bs;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS
check(struct msc *m, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buf)
{
	if (!Buf)
		return EFI_INVALID_PARAMETER;
	if (!m->media.MediaPresent)
		return EFI_NO_MEDIA;
	if (MediaId != m->media.MediaId)
		return EFI_MEDIA_CHANGED;
	if (Size % m->media.BlockSize)
		return EFI_BAD_BUFFER_SIZE;
	if (Lba > m->media.LastBlock || Size / m->media.BlockSize > m->media.LastBlock - Lba + 1)
		return EFI_INVALID_PARAMETER;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
bio_read(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buf)
{
	struct msc *m = (struct msc *)This;
	EFI_STATUS s = check(m, MediaId, Lba, Size, Buf);
	if (EFI_ERROR(s) || !Size)
		return s;
	return rw(m, 0, Lba, Size, Buf);
}

static EFI_STATUS EFIAPI
bio_write(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buf)
{
	struct msc *m = (struct msc *)This;
	EFI_STATUS s = check(m, MediaId, Lba, Size, Buf);
	if (EFI_ERROR(s) || !Size)
		return s;
	if (m->media.ReadOnly)
		return EFI_WRITE_PROTECTED;
	return rw(m, 1, Lba, Size, Buf);
}

static EFI_STATUS EFIAPI
bio_flush(EFI_BLOCK_IO_PROTOCOL *This)
{
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
bio_reset(EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Ext)
{
	reset_recovery((struct msc *)This);
	return EFI_SUCCESS;
}

/* the device's identity and capacity; installs Block I/O on its handle */
int
msc_start(struct udev *d)
{
	struct msc *m = calloc(1, sizeof *m);
	if (!m)
		return -1;
	m->d = d;
	m->cbw = dma(64, 64);
	m->csw = dma(64, 64);
	m->buf = dma(MSC_BUF, MSC_BUF);
	if (!m->cbw || !m->csw || !m->buf)
		return -1;
	d->priv = m;

	uint8_t inq[12] = { 0x12, 0, 0, 0, 36 };	/* INQUIRY */
	char vendor[9] = "", product[17] = "";
	int removable = 1;
	if (scsi(m, inq, 12, 1, 36) == 0) {
		removable = (m->buf[1] & 0x80) != 0;
		for (int i = 0; i < 8; i++)
			vendor[i] = (char)m->buf[8 + i];
		for (int i = 0; i < 16; i++)
			product[i] = (char)m->buf[16 + i];
		for (int i = 7; i >= 0 && vendor[i] == ' '; i--)
			vendor[i] = 0;
		for (int i = 15; i >= 0 && product[i] == ' '; i--)
			product[i] = 0;
	}
	/* TEST UNIT READY until the unit attention / not ready clears */
	int ready = 0;
	for (int i = 0; i < 5 && !ready; i++) {
		uint8_t tur[12] = { 0 };
		int r = scsi(m, tur, 12, 0, 0);
		if (r == 0)
			ready = 1;
		else if (r == 1) {
			uint8_t key = 0, asc = 0;
			request_sense(m, &key, &asc);
			if (key == 0x02 && asc == 0x3A)
				break;		/* no medium */
		} else
			break;
	}
	uint8_t cap[12] = { 0x25 };			/* READ CAPACITY(10) */
	uint64_t blocks = 0;
	uint32_t bs = 512;
	if (ready && scsi(m, cap, 12, 1, 8) == 0) {
		blocks = (uint64_t)get_be32(m->buf) + 1;
		bs = get_be32(m->buf + 4);
	}
	m->media = (EFI_BLOCK_IO_MEDIA){ 1, (BOOLEAN)removable, (BOOLEAN)(blocks != 0), FALSE, FALSE,
	    FALSE, bs, 0, blocks ? blocks - 1 : 0, 0, 1, 0 };
	m->bio = (EFI_BLOCK_IO_PROTOCOL){ 0x00010000, &m->media, bio_reset, bio_read, bio_write, bio_flush };
	snprintf(m->name.name, sizeof m->name.name, "%s %s", vendor, product);
	DEBUG("USB: port %d: %s, %lu blocks of %u bytes%s\n", d->port, m->name.name, blocks, bs,
	    removable ? ", removable" : "");
	hc.info.storage++;
	static EFI_GUID name_guid = VIBE86_DEVICE_NAME_GUID;
	usb_install(d, &gEfiBlockIoProtocolGuid, &m->bio, &name_guid, &m->name);
	return 0;
}
