/*
 * Shell: the firmware's built-in EFI shell, a UEFI application in FV_MAIN
 * that the boot manager offers as "UEFI: Built-in EFI Shell". A small
 * subset of the UEFI Shell Specification 2.2 (UEFI Forum, 2016): mappings
 * fsN: (file systems) and blkN: (block devices), cd, ls/dir, cat/type, map,
 * memmap, pci, dmem, devices, ver, date, time, echo, cls, reset, exit, help;
 * any other word runs an .efi image from the current directory (with the
 * command line as its load options), and startup.nsh on the first file
 * system with one is run at start (3.6 "startup.nsh").
 */
#include "uefilib.h"
#include "vibe86fw.h"
#include "devname.h"

#define MAX_MAP		16
#define LINE		256

struct map {
	char name[8];
	EFI_HANDLE h;
	int fs;			/* file system (fsN) or block device (blkN) */
};

static struct map maps[MAX_MAP];
static int nmaps;
static int cur = -1;			/* the current file system mapping */
static CHAR16 cwd[MAX_MAP][LINE];	/* per mapping, "\" or "\dir\sub" */
static int exit_shell;
static char history[8][LINE];
static int nhist;

/* ---------------------------------------------------------------- console */

static int
getkey(EFI_INPUT_KEY *k)
{
	UINTN i;
	gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &i);
	return gST->ConIn->ReadKeyStroke(gST->ConIn, k) == EFI_SUCCESS;
}

static void
putcs(const CHAR16 *s)
{
	gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)s);
}

static void
read_line(char *buf, int n)
{
	int len = 0, h = nhist;
	buf[0] = 0;
	for (;;) {
		EFI_INPUT_KEY k;
		if (!getkey(&k))
			continue;
		if (k.UnicodeChar == CHAR_CARRIAGE_RETURN) {
			Print("\n");
			break;
		}
		if (k.UnicodeChar == CHAR_BACKSPACE) {
			if (len) {
				len--;
				Print("\b \b");
			}
			continue;
		}
		if (k.ScanCode == SCAN_UP || k.ScanCode == SCAN_DOWN) {
			int nh = k.ScanCode == SCAN_UP ? h - 1 : h + 1;
			if (nh < 0 || nh > nhist)
				continue;
			h = nh;
			while (len--)
				Print("\b \b");
			strncpy0(buf, h < nhist ? history[h] : "", (size_t)n);
			len = (int)strlen(buf);
			Print("%s", buf);
			continue;
		}
		if (k.ScanCode == SCAN_ESC) {
			while (len--)
				Print("\b \b");
			len = 0;
			continue;
		}
		if (k.UnicodeChar >= 0x20 && k.UnicodeChar < 0x7F && len < n - 1) {
			buf[len++] = (char)k.UnicodeChar;
			Print("%c", k.UnicodeChar);
		}
	}
	buf[len] = 0;
	if (len) {
		if (nhist == ARRAY_SIZE(history)) {
			memmove(history[0], history[1], sizeof history - sizeof history[0]);
			nhist--;
		}
		strncpy0(history[nhist++], buf, LINE);
	}
}

/* ---------------------------------------------------------------- mappings */

static void
build_maps(void)
{
	EFI_HANDLE *hs;
	UINTN n;
	nmaps = 0;
	int nfs = 0, nblk = 0;
	if (gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, 0, &n, &hs) == EFI_SUCCESS) {
		for (UINTN i = 0; i < n && nmaps < MAX_MAP; i++) {
			snprintf(maps[nmaps].name, sizeof maps[0].name, "fs%d", nfs++);
			maps[nmaps].h = hs[i];
			maps[nmaps].fs = 1;
			cwd[nmaps][0] = '\\';
			cwd[nmaps][1] = 0;
			nmaps++;
		}
		free(hs);
	}
	if (gBS->LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid, 0, &n, &hs) == EFI_SUCCESS) {
		for (UINTN i = 0; i < n && nmaps < MAX_MAP; i++) {
			snprintf(maps[nmaps].name, sizeof maps[0].name, "blk%d", nblk++);
			maps[nmaps].h = hs[i];
			maps[nmaps].fs = 0;
			nmaps++;
		}
		free(hs);
	}
	cur = nmaps && maps[0].fs ? 0 : -1;
}

static const char *
dev_name(EFI_HANDLE h, char *buf, size_t n)
{
	static EFI_GUID ng = VIBE86_DEVICE_NAME_GUID;
	EFI_DEVICE_PATH_PROTOCOL *p = dp_of(h), *q = p;
	EFI_HANDLE nh;
	VIBE86_DEVICE_NAME *dn;
	if (p && gBS->LocateDevicePath(&ng, &q, &nh) == EFI_SUCCESS &&
	    gBS->HandleProtocol(nh, &ng, (void **)&dn) == EFI_SUCCESS) {
		strncpy0(buf, dn->name, n);
		return buf;
	}
	buf[0] = 0;
	return buf;
}

static void
cmd_map(void)
{
	char path[160], name[64];
	Print("Mapping table\n");
	for (int i = 0; i < nmaps; i++) {
		dp_to_text(dp_of(maps[i].h), path, sizeof path);
		Print("%6s: %s %s\n", maps[i].name, maps[i].fs ? "Alias(s):" : "", dev_name(maps[i].h, name, sizeof name));
		Print("        %s\n", path);
	}
}

/* ---------------------------------------------------------------- files */

/* resolve a shell path ("fs1:\x\y", "\x", "y", "..") to a mapping and an
   absolute path on it */
static int
resolve(const char *arg, int *m, CHAR16 *out)
{
	int map = cur;
	const char *p = arg;
	const char *colon = 0;
	for (const char *s = arg; *s && *s != '\\'; s++)
		if (*s == ':')
			colon = s;
	if (colon) {
		map = -1;
		for (int i = 0; i < nmaps; i++)
			if ((size_t)(colon - arg) == strlen(maps[i].name) &&
			    !strncmp(maps[i].name, arg, (size_t)(colon - arg)))
				map = i;
		if (map < 0 || !maps[map].fs)
			return -1;
		p = colon + 1;
	}
	if (map < 0)
		return -1;
	CHAR16 tmp[LINE];
	if (*p == '\\' || colon)
		tmp[0] = 0;
	else
		wcsncpy0(tmp, cwd[map], LINE);
	/* append p, then normalise . and .. */
	size_t l = wcslen(tmp);
	if (*p && (!l || tmp[l - 1] != '\\') && l < LINE - 1)
		tmp[l++] = '\\';
	for (; *p && l < LINE - 1; p++)
		tmp[l++] = (CHAR16)(*p == '/' ? '\\' : *p);
	tmp[l] = 0;
	CHAR16 *o = out;
	*o = 0;
	size_t ol = 0;
	for (const CHAR16 *s = tmp; *s;) {
		while (*s == '\\')
			s++;
		const CHAR16 *e = s;
		while (*e && *e != '\\')
			e++;
		size_t cl = (size_t)(e - s);
		if (cl == 1 && s[0] == '.') {
		} else if (cl == 2 && s[0] == '.' && s[1] == '.') {
			while (ol && out[ol - 1] != '\\')
				ol--;
			if (ol)
				ol--;
		} else if (cl) {
			out[ol++] = '\\';
			for (size_t i = 0; i < cl && ol < LINE - 1; i++)
				out[ol++] = s[i];
		}
		out[ol] = 0;
		s = e;
	}
	if (!ol) {
		out[0] = '\\';
		out[1] = 0;
	}
	*m = map;
	return 0;
}

static EFI_FILE_PROTOCOL *
open_path(int map, const CHAR16 *path)
{
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
	EFI_FILE_PROTOCOL *root, *f;
	if (gBS->HandleProtocol(maps[map].h, &gEfiSimpleFileSystemProtocolGuid, (void **)&fs) ||
	    fs->OpenVolume(fs, &root))
		return 0;
	EFI_STATUS s = root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
	root->Close(root);
	return s == EFI_SUCCESS ? f : 0;
}

static EFI_FILE_INFO *
file_info(EFI_FILE_PROTOCOL *f)
{
	UINTN n = 0;
	EFI_FILE_INFO *i = 0;
	if (f->GetInfo(f, &gEfiFileInfoGuid, &n, 0) != EFI_BUFFER_TOO_SMALL || !(i = malloc(n)))
		return 0;
	if (f->GetInfo(f, &gEfiFileInfoGuid, &n, i)) {
		free(i);
		return 0;
	}
	return i;
}

static void
cmd_ls(const char *arg)
{
	int m;
	CHAR16 path[LINE];
	if (resolve(arg ? arg : "", &m, path)) {
		Print("ls: no such file system\n");
		return;
	}
	EFI_FILE_PROTOCOL *d = open_path(m, path);
	if (!d) {
		Print("ls: %ls: not found\n", path);
		return;
	}
	EFI_FILE_INFO *self = file_info(d);
	if (self && !(self->Attribute & EFI_FILE_DIRECTORY)) {
		Print("  %10lu  %ls\n", self->FileSize, self->FileName);
		free(self);
		d->Close(d);
		return;
	}
	free(self);
	Print("Directory of: %s:%ls\n", maps[m].name, path);
	UINT8 buf[SIZE_OF_EFI_FILE_INFO + 512];
	unsigned files = 0, dirs = 0;
	UINT64 bytes = 0;
	for (;;) {
		UINTN n = sizeof buf;
		if (d->Read(d, &n, buf) || !n)
			break;
		EFI_FILE_INFO *i = (EFI_FILE_INFO *)buf;
		EFI_TIME *t = &i->ModificationTime;
		if (i->Attribute & EFI_FILE_DIRECTORY) {
			Print("%02u/%02u/%04u  %02u:%02u <DIR>          %ls\n", t->Month, t->Day, t->Year,
			    t->Hour, t->Minute, i->FileName);
			dirs++;
		} else {
			Print("%02u/%02u/%04u  %02u:%02u %14lu  %ls\n", t->Month, t->Day, t->Year, t->Hour,
			    t->Minute, i->FileSize, i->FileName);
			files++;
			bytes += i->FileSize;
		}
	}
	Print("%10u File(s) %10lu bytes\n%10u Dir(s)\n", files, bytes, dirs);
	d->Close(d);
}

static void
cmd_cd(const char *arg)
{
	int m;
	CHAR16 path[LINE];
	if (!arg) {
		if (cur >= 0)
			Print("%s:%ls\n", maps[cur].name, cwd[cur]);
		return;
	}
	if (resolve(arg, &m, path)) {
		Print("cd: no such file system\n");
		return;
	}
	EFI_FILE_PROTOCOL *d = open_path(m, path);
	EFI_FILE_INFO *i = d ? file_info(d) : 0;
	if (!i || !(i->Attribute & EFI_FILE_DIRECTORY))
		Print("cd: %ls: not a directory\n", path);
	else
		wcsncpy0(cwd[m], path, LINE);
	free(i);
	if (d)
		d->Close(d);
}

static void
cmd_cat(const char *arg)
{
	int m;
	CHAR16 path[LINE];
	if (!arg || resolve(arg, &m, path)) {
		Print("cat: which file?\n");
		return;
	}
	EFI_FILE_PROTOCOL *f = open_path(m, path);
	if (!f) {
		Print("cat: %ls: not found\n", path);
		return;
	}
	char buf[257];
	for (;;) {
		UINTN n = 256;
		if (f->Read(f, &n, buf) || !n)
			break;
		buf[n] = 0;
		for (UINTN i = 0; i < n; i++)
			if ((unsigned char)buf[i] >= 0x80 || (buf[i] < 0x20 && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t'))
				buf[i] = '.';
		Print("%s", buf);
	}
	Print("\n");
	f->Close(f);
}

/* ---------------------------------------------------------------- system */

static const char *const mem_types[] = { "Reserved", "LoaderCode", "LoaderData", "BS_Code", "BS_Data",
	"RT_Code", "RT_Data", "Available", "Unusable", "ACPI_Recl", "ACPI_NVS", "MMIO", "MMIO_Port",
	"PalCode", "Persistent", "Unaccepted" };

static void
cmd_memmap(void)
{
	UINTN sz = 0, key, dsz;
	UINT32 ver;
	gBS->GetMemoryMap(&sz, 0, &key, &dsz, &ver);
	sz += 8 * dsz;
	UINT8 *b = malloc(sz);
	if (!b || gBS->GetMemoryMap(&sz, (void *)b, &key, &dsz, &ver)) {
		free(b);
		return;
	}
	Print("Type       Start            End              # Pages          Attributes\n");
	for (UINTN o = 0; o < sz; o += dsz) {
		EFI_MEMORY_DESCRIPTOR *d = (void *)(b + o);
		Print("%-10s %016lx-%016lx %016lx %016lx\n",
		    d->Type < ARRAY_SIZE(mem_types) ? mem_types[d->Type] : "OEM", d->PhysicalStart,
		    d->PhysicalStart + d->NumberOfPages * 4096 - 1, d->NumberOfPages, d->Attribute);
	}
	free(b);
}

static void
cmd_pci(void)
{
	EFI_HANDLE *hs;
	UINTN n;
	char name[64];
	if (gBS->LocateHandleBuffer(ByProtocol, &gEfiPciIoProtocolGuid, 0, &n, &hs))
		return;
	Print("   Seg  Bus  Dev  Func\n   ---  ---  ---  ----\n");
	for (UINTN i = 0; i < n; i++) {
		EFI_PCI_IO_PROTOCOL *io;
		UINTN s, b, d, f;
		UINT32 id, cls;
		gBS->HandleProtocol(hs[i], &gEfiPciIoProtocolGuid, (void **)&io);
		io->GetLocation(io, &s, &b, &d, &f);
		io->Pci.Read(io, EfiPciIoWidthUint32, 0, 1, &id);
		io->Pci.Read(io, EfiPciIoWidthUint32, 8, 1, &cls);
		Print("    %02lx   %02lx   %02lx    %02lx ==> %04x:%04x class %06x %s\n", s, b, d, f,
		    id & 0xFFFF, id >> 16, cls >> 8, dev_name(hs[i], name, sizeof name));
	}
	free(hs);
}

static void
cmd_devices(void)
{
	EFI_HANDLE *hs;
	UINTN n;
	char path[160];
	if (gBS->LocateHandleBuffer(AllHandles, 0, 0, &n, &hs))
		return;
	for (UINTN i = 0; i < n; i++) {
		EFI_DEVICE_PATH_PROTOCOL *p = dp_of(hs[i]);
		if (!p)
			continue;
		dp_to_text(p, path, sizeof path);
		Print("%3lu %s\n", i, path);
	}
	free(hs);
}

static void
cmd_dmem(const char *a, const char *l)
{
	if (!a) {
		Print("dmem address [length]\n");
		return;
	}
	uint64_t addr = str_to_u64(a, 0, 16), len = l ? str_to_u64(l, 0, 16) : 0x80;
	for (uint64_t o = 0; o < len; o += 16) {
		const volatile UINT8 *p = (const volatile UINT8 *)(uintptr_t)(addr + o);
		Print("%016lx: ", addr + o);
		for (int i = 0; i < 16; i++)
			Print("%02x%c", p[i], i == 7 ? '-' : ' ');
		Print(" ");
		for (int i = 0; i < 16; i++)
			Print("%c", p[i] >= 0x20 && p[i] < 0x7F ? p[i] : '.');
		Print("\n");
	}
}

/* gop [mode]: the Graphics Output Protocol's modes, or switch to one */
static void
cmd_gop(const char *arg)
{
	EFI_GRAPHICS_OUTPUT_PROTOCOL *g;
	if (gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, 0, (void **)&g)) {
		Print("gop: no Graphics Output Protocol\n");
		return;
	}
	if (arg) {
		EFI_STATUS s = g->SetMode(g, (UINT32)str_to_u64(arg, 0, 10));
		if (EFI_ERROR(s))
			Print("gop: SetMode: %r\n", s);
		return;
	}
	for (UINT32 m = 0; m < g->Mode->MaxMode; m++) {
		EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *i;
		UINTN sz;
		EFI_STATUS s = g->QueryMode(g, m, &sz, &i);
		if (s == EFI_NOT_STARTED) {
			Print("gop: %u modes; not started (the display is in text mode)\n", g->Mode->MaxMode);
			return;
		}
		if (EFI_ERROR(s))
			continue;
		Print("%c%u: %ux%u, format %u, %u pixels per line\n", m == g->Mode->Mode ? '*' : ' ', m,
		    i->HorizontalResolution, i->VerticalResolution, i->PixelFormat, i->PixelsPerScanLine);
		free(i);
	}
	Print("frame buffer %lx, %lu bytes\n", g->Mode->FrameBufferBase, g->Mode->FrameBufferSize);
}

static void
cmd_ver(void)
{
	Print("UEFI Interactive Shell (vibe86 built-in), UEFI v%u.%02u\n", gST->Hdr.Revision >> 16,
	    gST->Hdr.Revision & 0xFFFF);
	Print("UEFI Firmware: %ls rev. %u (%s %s)\n", gST->FirmwareVendor, gST->FirmwareRevision, FW_NAME,
	    FW_VERSION_STR);
}

static void
cmd_help(void)
{
	Print("cd [path]          change / show the current directory\n"
	    "ls | dir [path]    list a directory\n"
	    "cat | type file    print a file\n"
	    "fsN:               change the current file system\n"
	    "map                show the device mappings\n"
	    "memmap             show the UEFI memory map\n"
	    "pci                list the PCI functions\n"
	    "devices            list the device paths\n"
	    "dmem addr [len]    dump memory (hex)\n"
	    "gop [mode]         graphics modes, or set one\n"
	    "date | time        show the RTC\n"
	    "echo text          print text\n"
	    "cls                clear the screen\n"
	    "ver                firmware version\n"
	    "reset [-s|-w]      reset (cold), shut down or warm reset\n"
	    "exit               leave the shell\n"
	    "file[.efi] [args]  run a UEFI image\n");
}

/* ---------------------------------------------------------------- images */

static int
run_image(const char *word, const char *line)
{
	int m;
	CHAR16 path[LINE];
	if (resolve(word, &m, path))
		return -1;
	EFI_FILE_PROTOCOL *f = open_path(m, path);
	if (!f) {
		size_t l = wcslen(path);
		if (l + 4 < LINE) {
			wcs_from_ascii(path + l, ".efi", 5);
			f = open_path(m, path);
		}
	}
	if (!f)
		return -1;
	EFI_FILE_INFO *i = file_info(f);
	f->Close(f);
	if (!i || (i->Attribute & EFI_FILE_DIRECTORY)) {
		free(i);
		return -1;
	}
	free(i);
	EFI_DEVICE_PATH_PROTOCOL *dp = dp_append_file(dp_of(maps[m].h), path);
	EFI_HANDLE h;
	EFI_STATUS s = gBS->LoadImage(FALSE, gImageHandle, dp, 0, 0, &h);
	free(dp);
	if (EFI_ERROR(s)) {
		Print("%ls: LoadImage: %r\n", path, s);
		return 0;
	}
	EFI_LOADED_IMAGE_PROTOCOL *li;
	CHAR16 *opts = malloc((strlen(line) + 1) * 2);
	if (opts && gBS->HandleProtocol(h, &gEfiLoadedImageProtocolGuid, (void **)&li) == EFI_SUCCESS) {
		wcs_from_ascii(opts, line, strlen(line) + 1);
		li->LoadOptions = opts;
		li->LoadOptionsSize = (UINT32)((strlen(line) + 1) * 2);
	}
	s = gBS->StartImage(h, 0, 0);
	free(opts);
	if (EFI_ERROR(s))
		Print("%ls: %r\n", path, s);
	return 0;
}

/* ---------------------------------------------------------------- commands */

static void
execute(char *line)
{
	char *argv[8];
	int argc = 0;
	char full[LINE];
	strncpy0(full, line, sizeof full);
	for (char *p = line; *p && argc < 8;) {
		while (*p == ' ' || *p == '\t')
			*p++ = 0;
		if (!*p)
			break;
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}
	if (!argc || argv[0][0] == '#')
		return;
	const char *c = argv[0];
	const char *a1 = argc > 1 ? argv[1] : 0;
	size_t cl = strlen(c);
	if (c[cl - 1] == ':' && argc == 1) {
		for (int i = 0; i < nmaps; i++)
			if (cl - 1 == strlen(maps[i].name) && !strncmp(c, maps[i].name, cl - 1) && maps[i].fs) {
				cur = i;
				return;
			}
		Print("'%s' is not a correct mapping.\n", c);
	} else if (!strcmp(c, "help") || !strcmp(c, "?")) {
		cmd_help();
	} else if (!strcmp(c, "ls") || !strcmp(c, "dir")) {
		cmd_ls(a1);
	} else if (!strcmp(c, "cd")) {
		cmd_cd(a1);
	} else if (!strcmp(c, "cat") || !strcmp(c, "type")) {
		cmd_cat(a1);
	} else if (!strcmp(c, "map")) {
		if (a1 && !strcmp(a1, "-r"))
			build_maps();
		cmd_map();
	} else if (!strcmp(c, "memmap")) {
		cmd_memmap();
	} else if (!strcmp(c, "pci")) {
		cmd_pci();
	} else if (!strcmp(c, "devices")) {
		cmd_devices();
	} else if (!strcmp(c, "gop")) {
		cmd_gop(a1);
	} else if (!strcmp(c, "dmem")) {
		cmd_dmem(a1, argc > 2 ? argv[2] : 0);
	} else if (!strcmp(c, "date") || !strcmp(c, "time")) {
		EFI_TIME t;
		if (gRT->GetTime(&t, 0) == EFI_SUCCESS)
			Print(c[0] == 'd' ? "%02u/%02u/%04u\n" : "%02u:%02u:%02u\n",
			    c[0] == 'd' ? t.Month : t.Hour, c[0] == 'd' ? t.Day : t.Minute,
			    c[0] == 'd' ? t.Year : t.Second);
	} else if (!strcmp(c, "echo")) {
		Print("%s\n", argc > 1 ? full + (argv[1] - line) : "");
	} else if (!strcmp(c, "cls")) {
		gST->ConOut->ClearScreen(gST->ConOut);
	} else if (!strcmp(c, "ver")) {
		cmd_ver();
	} else if (!strcmp(c, "reset")) {
		EFI_RESET_TYPE t = a1 && !strcmp(a1, "-s") ? EfiResetShutdown : a1 && !strcmp(a1, "-w") ?
		    EfiResetWarm : EfiResetCold;
		gRT->ResetSystem(t, EFI_SUCCESS, 0, 0);
	} else if (!strcmp(c, "exit")) {
		exit_shell = 1;
	} else if (run_image(c, full) < 0) {
		Print("'%s' is not recognized as an internal or external command, operable program, or script file.\n", c);
	}
}

static void
prompt(void)
{
	if (cur >= 0)
		Print("%s:%ls> ", maps[cur].name, cwd[cur]);
	else
		Print("Shell> ");
}

/* startup.nsh (UEFI Shell 2.2 3.6) on the first file system with one */
static void
startup(void)
{
	for (int i = 0; i < nmaps; i++) {
		if (!maps[i].fs)
			continue;
		EFI_FILE_PROTOCOL *f = open_path(i, L"\\startup.nsh");
		if (!f)
			continue;
		Print("Press ESC in 1 seconds to skip startup.nsh or any other key to continue.\n");
		EFI_INPUT_KEY k;
		UINTN idx;
		EFI_EVENT ev[2];
		ev[0] = gST->ConIn->WaitForKey;
		gBS->CreateEvent(EVT_TIMER, 0, 0, 0, &ev[1]);
		gBS->SetTimer(ev[1], TimerRelative, 10000);	/* 1 ms: the simulator is slow */
		gBS->WaitForEvent(2, ev, &idx);
		gBS->CloseEvent(ev[1]);
		if (idx == 0 && gST->ConIn->ReadKeyStroke(gST->ConIn, &k) == EFI_SUCCESS && k.ScanCode == SCAN_ESC) {
			f->Close(f);
			return;
		}
		static char script[4096];
		UINTN n = sizeof script - 1;
		f->Read(f, &n, script);
		f->Close(f);
		script[n] = 0;
		cur = i;
		for (char *line = script; *line && !exit_shell;) {
			char *e = line;
			while (*e && *e != '\n' && *e != '\r')
				e++;
			char save = *e;
			*e = 0;
			prompt();
			Print("%s\n", line);
			execute(line);
			line = save ? e + 1 : e;
		}
		return;
	}
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	uefi_init(image, st);
	gST->ConOut->EnableCursor(gST->ConOut, TRUE);
	gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
	gST->ConOut->ClearScreen(gST->ConOut);
	putcs(L"UEFI Interactive Shell (vibe86 built-in)\r\n");
	cmd_ver();
	build_maps();
	cmd_map();
	startup();
	static char line[LINE];
	while (!exit_shell) {
		prompt();
		read_line(line, sizeof line);
		execute(line);
	}
	return EFI_SUCCESS;
}
