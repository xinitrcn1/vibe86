/*
 * SerialDxe: the Serial I/O protocol (UEFI 2.10 12.8) on COM1, the
 * platform controller's 16550 at 3F8h (National Semiconductor PC16550D
 * datasheet), with the ACPI device path of the DSDT's COM1 (PNP0501, UID 0)
 * and a UART node. The console driver puts a terminal on it.
 */
#include "uefilib.h"
#include "x86.h"

#define LSR	5
#define LSR_DR	0x01
#define LSR_THRE 0x20
#define MSR	6

#pragma pack(push, 1)
static struct {
	ACPI_HID_DEVICE_PATH root;
	ACPI_HID_DEVICE_PATH uart_dev;
	UART_DEVICE_PATH uart;
	EFI_DEVICE_PATH_PROTOCOL end;
} path;
#pragma pack(pop)

static SERIAL_IO_MODE mode;
static EFI_SERIAL_IO_PROTOCOL sio;
static const EFI_GUID uart_type = EFI_GUID_DEF(0x6ad9a60f, 0x5815, 0x4c7c, 0x8a, 0x10, 0x50, 0x53, 0xd2, 0xbf, 0x7a, 0x1b);	/* EFI_SERIAL_TERMINAL_DEVICE_TYPE_GUID */

static EFI_STATUS EFIAPI
s_reset(EFI_SERIAL_IO_PROTOCOL *This)
{
	com1_init();
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
s_set_attributes(EFI_SERIAL_IO_PROTOCOL *This, UINT64 BaudRate, UINT32 ReceiveFifoDepth, UINT32 Timeout,
    EFI_PARITY_TYPE Parity, UINT8 DataBits, EFI_STOP_BITS_TYPE StopBits)
{
	/* the board's UART has no line to misconfigure: accept 8N1 at any rate */
	if ((DataBits && DataBits != 8) || (Parity > NoParity) || (StopBits > OneStopBit))
		return EFI_UNSUPPORTED;
	if (BaudRate)
		mode.BaudRate = BaudRate;
	if (Timeout)
		mode.Timeout = Timeout;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
s_set_control(EFI_SERIAL_IO_PROTOCOL *This, UINT32 Control)
{
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
s_get_control(EFI_SERIAL_IO_PROTOCOL *This, UINT32 *Control)
{
	UINT32 c = EFI_SERIAL_CLEAR_TO_SEND | EFI_SERIAL_DATA_SET_READY | EFI_SERIAL_CARRIER_DETECT;
	if (!(inb(COM1 + LSR) & LSR_DR))
		c |= EFI_SERIAL_INPUT_BUFFER_EMPTY;
	*Control = c;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
s_write(EFI_SERIAL_IO_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer)
{
	const UINT8 *p = Buffer;
	for (UINTN i = 0; i < *BufferSize; i++)
		com1_putc(p[i]);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
s_read(EFI_SERIAL_IO_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer)
{
	UINT8 *p = Buffer;
	UINTN n = 0;
	int c;
	while (n < *BufferSize && (c = com1_getc()) >= 0)
		p[n++] = (UINT8)c;
	EFI_STATUS s = n == *BufferSize ? EFI_SUCCESS : EFI_TIMEOUT;
	*BufferSize = n;
	return s;
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	uefi_init(image, st);
	com1_init();
	dp_set_node(&path.root.Header, ACPI_DEVICE_PATH, ACPI_DP, sizeof path.root);
	path.root.HID = EISA_PNP_ID(0x0A03);
	dp_set_node(&path.uart_dev.Header, ACPI_DEVICE_PATH, ACPI_DP, sizeof path.uart_dev);
	path.uart_dev.HID = EISA_PNP_ID(0x0501);
	dp_set_node(&path.uart.Header, MESSAGING_DEVICE_PATH, MSG_UART_DP, sizeof path.uart);
	path.uart.BaudRate = 115200;
	path.uart.DataBits = 8;
	path.uart.Parity = NoParity;
	path.uart.StopBits = OneStopBit;
	dp_set_end(&path.end);
	mode = (SERIAL_IO_MODE){ 0, 1000000, 115200, 16, 8, NoParity, OneStopBit };
	sio = (EFI_SERIAL_IO_PROTOCOL){ EFI_SERIAL_IO_PROTOCOL_REVISION, s_reset, s_set_attributes,
	    s_set_control, s_get_control, s_write, s_read, &mode, &uart_type };
	EFI_HANDLE h = 0;
	return gBS->InstallMultipleProtocolInterfaces(&h, &gEfiSerialIoProtocolGuid, &sio,
	    &gEfiDevicePathProtocolGuid, &path, (void *)0);
}
