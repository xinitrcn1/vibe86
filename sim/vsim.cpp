// vsim - vibe86 board simulator: the Verilated core + board model.
//
// vsim [-b server|test] [-C cpu] [-n] [-d] [-J strap] [-m dimm]... [-P slot] [-X slot] [-U image] [-K script] [-D port]
//      [-c cycles] [-H cycles] [-t] [-v] [-x] [-s scale] [-V vga.bmp] [-Y vga.txt] [-T tpm-state]
//      [-N nvram] [-l file@addr]... [-e file.elf]... [eeprom.bin]
//
// Serial (COM1) always goes to stdout. Window mode (default) shows the
// on-board VGA's picture in an SDL3 window (the "monitor"). Keys and the
// mouse in the window go to the USB keyboard and mouse on the xHCI
// controller (to the UART when there's no controller, -X none); terminal
// input on stdin goes to the UART; Pause is the power button. -K injects
// keys on a cycle schedule (headless too). -n runs headless: no window.

#include <SDL3/SDL.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <vector>
#include <string>
#include <elf.h>
#include <fcntl.h>
#include <poll.h>
#include <strings.h>
#include <unistd.h>

#include "cpu.h"
#include "jtag_bitbang.h"
#include "board.h"
#include "iod.h"
#include "vr.h"
#include "verilated.h"

namespace {

// ---------------------------------------------------------------- options

struct opts {
	bool headless = false;
	const char *preload[16] = {};	// -l file@addr
	int npreload = 0;
	const char *elf[16] = {};	// -e file.elf
	int nelf = 0;
	uint64_t halt_idle = 1000000;	// -H: stop after this long in HLT
	int board_kind = BOARD_SERVER;
	bool debug = false;
	int ecc_strap = -1;		// -1: default (on with -d)
	uint64_t max_cycles = 0;	// 0: none in window mode, 100M headless
	bool trace = false;
	bool exit_on_off = false;
	int scale = 2;
	const char *shot = nullptr;	// -S: save the window here on exit
	const char *image = nullptr;
	bool verbose = false;
	bool dimms_given = false;
	int pci_slot = 0;		// -P: the DMA test endpoint's slot, -1 none
	int xhci_slot = 1;		// -X: the xHCI controller's slot, -1 none
	const char *usb_floppy = nullptr;	// -U: USB floppy image (xHCI port 3)
	bool usb_floppy_ro = false;
	const char *keys = nullptr;	// -K: key script
	int jtag_port = 0;		// -D: JTAG remote_bitbang server port
	ddr5_slot_cfg dimms[DDR5_SLOTS] = {};
	int meminit_fast = 1;		// -F: the die's meminit_fast strap (default 1 in simulation)
	const char *vga_bmp = nullptr;	// -V: dump the VGA picture on exit
	const char *vga_txt = nullptr;	// -Y: dump the VGA text cells on exit
	const char *nvram = nullptr;	// -N: NVRAM file
	const char *debugcon = nullptr;	// -g: the debug console (port 402h) to this file, - stderr
	std::vector<const char *> vr_faults;	// -W: VR faults (vr.h)
	const char *fwcfg[16] = {};	// -f name=file: fw_cfg files
	int nfwcfg = 0;
	const char *cpu = nullptr;	// -C: the core version (default: the latest)
};

void
usage()
{
	std::fprintf(stderr,
	    "usage: vsim [-b server|test] [-C cpu] [-n] [-d] [-J ecc-skip|ecc-check] [-m dimm]... [-F 0|1] [-P slot] [-X slot]\n"
	    "            [-K script] [-c cycles] [-H cycles] [-t] [-v] [-x] [-s scale] [-V vga.bmp] [-Y vga.txt]\n"
	    "            [-T tpm-state] [-N nvram] [-l file@addr]... [-e file.elf]... [eeprom.bin]\n"
	    "  eeprom.bin  flash image, placed so its last byte is at 0xFFFFFFFF (reset vector\n"
	    "      0xFFFFFFF0); optional with -e or -l (the EEPROM is then erased)\n"
	    "  -b  board model: server (default) or test (the same, ECC_SKIP fitted)\n"
	    "  -C  the core version in the socket: %s (default %s; rtl/<version>)\n"
	    "  -n  headless: no window (serial is always on stdout)\n"
	    "  -d  debug board: sets the ECC_SKIP jumper unless -J ecc-check\n"
	    "  -J  set a jumper explicitly\n"
	    "  -m  DIMM: slot:size[:noecc][:badspd], slot 0..3 = channel*2 + DIMM, size 16G (1Rx8)\n"
	    "      or 32G (2Rx8); repeatable; replaces the default (0:16G); -m none: no DIMMs\n"
	    "  -F  the die's meminit_fast strap: 1 (default) scales the JEDEC init waits by\n"
	    "      1/1024, 0 runs them in full (tINIT3 alone is 9.6M cycles)\n"
	    "  -P  PCIe slot of the DMA test endpoint: 0..7 or none (default 0)\n"
	    "  -X  PCIe slot of the xHCI USB controller (keyboard, mouse): 0..7 or none (default 1)\n"
	    "  -D  a JTAG probe: OpenOCD remote_bitbang server on 127.0.0.1:PORT (docs/jtag.md)\n"
	    "  -U  a USB floppy drive on the xHCI's port 3 with this disk image (IMAGE[,ro]:\n"
	    "      read into memory; writes are not saved to the file)\n"
	    "  -K  key script, items 'delay:key' (delay in cycles after the previous item, k/M\n"
	    "      suffixes; key: F1-F12 Up Down Left Right Enter Esc Tab Backspace Space Home End\n"
	    "      PgUp PgDn Insert Delete CapsLock LShift RShift LCtrl RCtrl LAlt RAlt, one\n"
	    "      character, text:string, +key / -key (press / release only), mouse:dx,dy,\n"
	    "      button:1|2|3); e.g. -K \"20M:F2 2M:Down 2M:Enter 1M:text:hello\"; -K @file\n"
	    "  -c  stop after this many cycles (headless default 100000000)\n"
	    "  -H  stop once the core has been halted (HLT) this many cycles (default 1000000)\n"
	    "  -t  trace every retired instruction, exception, interrupt and POST code on stderr\n"
	    "  -v  print the last POST code and memory-controller statistics on exit\n"
	    "  -x  close the window when the board powers off\n"
	    "  -s  window scale for the VGA picture (default 2)\n"
	    "  -S  save the window as a BMP when the simulator exits\n"
	    "  -V  save the VGA picture as a BMP on exit (works headless)\n"
	    "  -Y  save the VGA text cells as 25 lines of text on exit\n"
	    "  -T  keep the debug TPM's NV state in this file (default: fresh each run)\n"
	    "  -N  keep the NVRAM (firmware settings) in this file (default: erased each run)\n"
	    "  -g  write the debug console (I/O port 402h) to this file (- for stderr)\n"
	    "  -W  a voltage regulator fault for tests: vr0:nak, vr1:stuck, vr0:parity, ... (vr.h);\n"
	    "      repeatable\n"
	    "  -f  name=file: offer a file through fw_cfg (e.g. etc/acpi/tables=tables.bin); repeatable\n"
	    "  -l  file@addr: copy a raw image into memory before reset (DRAM through the\n"
	    "      DIMMs' backdoor, or the EEPROM range); repeatable\n"
	    "  -e  load an ELF64 x86-64 executable's PT_LOAD segments the same way; repeatable\n"
	    "Window: keys and the mouse go to the USB keyboard and mouse (to COM1 with -X none);\n"
	    "Pause is the power button.\n", cpu_names(), cpu_default());
	std::exit(4);
}

void
tx_stdout(void *, uint8_t c)
{
	std::fputc(c, stdout);
	if (c == '\n')
		std::fflush(stdout);
}

// ---------------------------------------------------------------- key script (-K)

// One scheduled input event. Key usages are those of the HID Usage Tables'
// Keyboard/Keypad page (USB-IF, "HID Usage Tables", page 0x07); without a
// USB controller a key press sends 'uart' to COM1 instead.
struct kev {
	uint64_t at;
	enum { KEY, BTN, MOVE } kind;
	unsigned code;		// usage, or button bit
	bool down;
	int dx, dy;
	std::string uart;	// the bytes for COM1 (presses only)
};

const uint64_t KEY_HOLD = 100000;	// cycles between a press and its release

struct keyname {
	const char *name;
	unsigned usage;
	const char *uart;	// VT100/xterm sequence for the serial console
};

const keyname key_names[] = {
	{ "F1", 0x3A, "\x1bOP" }, { "F2", 0x3B, "\x1bOQ" }, { "F3", 0x3C, "\x1bOR" },
	{ "F4", 0x3D, "\x1bOS" }, { "F5", 0x3E, "\x1b[15~" }, { "F6", 0x3F, "\x1b[17~" },
	{ "F7", 0x40, "\x1b[18~" }, { "F8", 0x41, "\x1b[19~" }, { "F9", 0x42, "\x1b[20~" },
	{ "F10", 0x43, "\x1b[21~" }, { "F11", 0x44, "\x1b[23~" }, { "F12", 0x45, "\x1b[24~" },
	{ "Up", 0x52, "\x1b[A" }, { "Down", 0x51, "\x1b[B" }, { "Right", 0x4F, "\x1b[C" },
	{ "Left", 0x50, "\x1b[D" }, { "Home", 0x4A, "\x1b[H" }, { "End", 0x4D, "\x1b[F" },
	{ "PgUp", 0x4B, "\x1b[5~" }, { "PgDn", 0x4E, "\x1b[6~" }, { "Insert", 0x49, "\x1b[2~" },
	{ "Delete", 0x4C, "\x1b[3~" }, { "Enter", 0x28, "\r" }, { "Esc", 0x29, "\x1b" },
	{ "Tab", 0x2B, "\t" }, { "Backspace", 0x2A, "\x08" }, { "Space", 0x2C, " " },
	{ "CapsLock", 0x39, "" }, { "LCtrl", 0xE0, "" }, { "LShift", 0xE1, "" },
	{ "LAlt", 0xE2, "" }, { "LGui", 0xE3, "" }, { "RCtrl", 0xE4, "" }, { "RShift", 0xE5, "" },
	{ "RAlt", 0xE6, "" }, { "RGui", 0xE7, "" },
};

// a character on a US keyboard: its usage and whether it needs Shift
bool
char_usage(char c, unsigned &u, bool &shift)
{
	static const char plain[] = "-=[]\\#;'`,./", shifted[] = "_+{}|~:\"~<>?";
	static const unsigned pu[] = { 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
	    0x36, 0x37, 0x38 };
	static const char digsh[] = ")!@#$%^&*(";
	shift = false;
	if (c >= 'a' && c <= 'z') { u = 0x04 + (unsigned)(c - 'a'); return true; }
	if (c >= 'A' && c <= 'Z') { u = 0x04 + (unsigned)(c - 'A'); shift = true; return true; }
	if (c >= '1' && c <= '9') { u = 0x1E + (unsigned)(c - '1'); return true; }
	if (c == '0') { u = 0x27; return true; }
	if (c == ' ') { u = 0x2C; return true; }
	for (int i = 0; i < 10; i++)
		if (c == digsh[i]) { u = i ? 0x1E + (unsigned)(i - 1) : 0x27; shift = true; return true; }
	for (int i = 0; plain[i]; i++) {
		if (plain[i] == '#')		// the non-US # key: not on a US keyboard
			continue;
		if (c == plain[i]) { u = pu[i]; return true; }
		if (c == shifted[i] && shifted[i] != '~') { u = pu[i]; shift = true; return true; }
	}
	if (c == '~') { u = 0x35; shift = true; return true; }
	return false;
}

bool
parse_keys(const char *script, std::vector<kev> &out)
{
	uint64_t t = 0;
	std::string s(script);
	size_t i = 0;
	auto press = [&](unsigned u, bool down, const std::string &uart) {
		kev e{};
		e.at = t;
		e.kind = kev::KEY;
		e.code = u;
		e.down = down;
		if (down)
			e.uart = uart;
		out.push_back(e);
	};
	auto type_char = [&](char c) -> bool {
		unsigned u;
		bool sh;
		if (!char_usage(c, u, sh))
			return false;
		if (sh)
			press(0xE1, true, "");
		press(u, true, std::string(1, c));
		t += KEY_HOLD;
		press(u, false, "");
		if (sh)
			press(0xE1, false, "");
		return true;
	};
	while (i < s.size()) {
		while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n'))
			i++;
		if (i == s.size())
			break;
		size_t j = i;
		while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\n')
			j++;
		std::string item = s.substr(i, j - i);
		i = j;
		size_t colon = item.find(':');
		if (colon == std::string::npos || colon == 0) {
			std::fprintf(stderr, "vsim: -K: '%s': expected delay:key\n", item.c_str());
			return false;
		}
		char *end;
		double d = std::strtod(item.c_str(), &end);
		if (*end == 'k')
			d *= 1e3, end++;
		else if (*end == 'M')
			d *= 1e6, end++;
		if (end != item.c_str() + colon || d < 0) {
			std::fprintf(stderr, "vsim: -K: '%s': bad delay\n", item.c_str());
			return false;
		}
		t += (uint64_t)d;
		std::string key = item.substr(colon + 1);
		if (key.compare(0, 5, "text:") == 0) {
			for (size_t k = 5; k < key.size(); k++) {
				if (k > 5)
					t += KEY_HOLD;
				if (!type_char(key[k])) {
					std::fprintf(stderr, "vsim: -K: can't type '%c'\n", key[k]);
					return false;
				}
			}
			continue;
		}
		if (key.compare(0, 6, "mouse:") == 0) {
			kev e{};
			e.at = t;
			e.kind = kev::MOVE;
			if (std::sscanf(key.c_str() + 6, "%d,%d", &e.dx, &e.dy) != 2) {
				std::fprintf(stderr, "vsim: -K: '%s': expected mouse:dx,dy\n", key.c_str());
				return false;
			}
			out.push_back(e);
			continue;
		}
		if (key.compare(0, 7, "button:") == 0) {
			int b = std::atoi(key.c_str() + 7);
			if (b < 1 || b > 3) {
				std::fprintf(stderr, "vsim: -K: '%s': button 1, 2 or 3\n", key.c_str());
				return false;
			}
			kev e{};
			e.at = t;
			e.kind = kev::BTN;
			e.code = b == 1 ? 0 : b == 2 ? 1 : 2;	// left, right, middle
			e.down = true;
			out.push_back(e);
			t += KEY_HOLD;
			e.at = t;
			e.down = false;
			out.push_back(e);
			continue;
		}
		int only = 0;			// +key: press only, -key: release only
		if (key.size() > 1 && (key[0] == '+' || key[0] == '-')) {
			only = key[0] == '+' ? 1 : -1;
			key = key.substr(1);
		}
		const keyname *kn = nullptr;
		for (const keyname &n : key_names)
			if (!strcasecmp(n.name, key.c_str()))
				kn = &n;
		if (!kn && key.size() == 1 && !only) {
			if (!type_char(key[0])) {
				std::fprintf(stderr, "vsim: -K: can't type '%s'\n", key.c_str());
				return false;
			}
			continue;
		}
		unsigned u;
		bool sh;
		std::string uart;
		if (kn) {
			u = kn->usage;
			uart = kn->uart;
		} else if (key.size() == 1 && char_usage(key[0], u, sh)) {
			uart = key;
		} else {
			std::fprintf(stderr, "vsim: -K: unknown key '%s'\n", key.c_str());
			return false;
		}
		if (only >= 0)
			press(u, true, uart);
		if (!only)
			t += KEY_HOLD;
		if (only <= 0)
			press(u, false, "");
	}
	std::stable_sort(out.begin(), out.end(),
	    [](const kev &a, const kev &b) { return a.at < b.at; });
	return true;
}

// ---------------------------------------------------------------- machine

// x86 exception and interrupt vectors (Intel SDM Vol. 3A, "Exception and
// Interrupt Reference"): their mnemonics for -t and the stop messages
const char *
vector_name(unsigned v)
{
	static const char *const n[32] = {
		"#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
		"#DF", "#CSO", "#TS", "#NP", "#SS", "#GP", "#PF", "#15",
		"#MF", "#AC", "#MC", "#XM", "#VE", "#CP", "#22", "#23",
		"#24", "#25", "#26", "#27", "#HV", "#VC", "#SX", "#31",
	};
	return v < 32 ? n[v] : "INTR";
}

struct machine {
	std::unique_ptr<VerilatedContext> ctx;
	std::unique_ptr<cpu> top;	// -C: the core version in the socket
	board brd{};
	uint64_t cycles = 0;
	bool trace = false;
	iod *io = nullptr;		// the I/O die at the far end of the link
	vrs *vr = nullptr;		// the board's voltage regulators (vr.h)
	// why the run stopped, besides the board's power state
	enum { RUNNING, SHUTDOWN, HALTED } stop = RUNNING;
	unsigned stop_core = 0;
	uint64_t stop_rip = 0;
	uint64_t halt_idle = 1000000;	// -H
	uint64_t halted_for[cpu::MAXCORES] = {};
	jtag_bitbang *jtag = nullptr;	// -D: the JTAG probe (OpenOCD remote_bitbang)
	bool jtag_srst = false;

	// the JTAG pins, from the probe; without one, TRST_n is held low (as
	// boards pull it down) and the TAP stays in Test-Logic-Reset
	void jtag_pins()
	{
		if (!jtag) {
			top->trst_n = 0;
			return;
		}
		jtag_bitbang_poll(jtag, top->tdo);
		top->tck = jtag->tck;
		top->tms = jtag->tms;
		top->tdi = jtag->tdi;
		top->trst_n = !jtag->trst;
		if (jtag->srst != jtag_srst) {	// SRST: the probe holds RESET_L
			jtag_srst = jtag->srst;
			top->rst_n = !jtag_srst;
		}
	}
	uint64_t last_rip[cpu::MAXCORES] = {};	// of the last retired instruction
	// -K: scheduled input
	std::vector<kev> keys;
	size_t nkey = 0;
	uint64_t next_key = UINT64_MAX;

	void inject()
	{
		while (nkey < keys.size() && keys[nkey].at <= cycles) {
			const kev &e = keys[nkey++];
			if (!brd.xhci) {
				if (e.kind == kev::KEY && e.down)
					for (char c : e.uart)
						uart_rx_push(&brd.uart, (uint8_t)c);
				continue;
			}
			switch (e.kind) {
			case kev::KEY: xhci_key(brd.xhci, e.code, e.down); break;
			case kev::BTN: xhci_mouse_button(brd.xhci, e.code, e.down); break;
			case kev::MOVE: xhci_mouse_move(brd.xhci, e.dx, e.dy); break;
			}
		}
		next_key = nkey < keys.size() ? keys[nkey].at : UINT64_MAX;
	}

	// RESET# asserted for one clock, then released; the core fetches from
	// the reset vector
	void reset()
	{
		top->rst_n = 0;
		top->pwrok = vr_power_good(vr);
		top->svd_i = 1;
		top->svt = 1;
		top->vr_hot_n = 1;
		top->trst_n = 0;		// power-on: the TAP resets too
		top->clk = 0;
		top->iol_tx_ready = 0;
		top->iol_rx_valid = 0;
		top->eval();
		top->clk = 1;
		top->eval();
		top->clk = 0;
		top->rst_n = 1;
		top->eval();
		board_reset(&brd);
		iod_reset(io);
		stop = RUNNING;
		for (uint64_t &h : halted_for)
			h = 0;
	}

	// One clock of the die's pins outside the die: sampled with the clock
	// low, applied after the rising edge. The I/O link's handshakes complete
	// at the edge (iod.h); the DIMMs see the DDR5 outputs the edge registered
	// and answer for the next edge (ddr5.h).
	int link_tx_valid = 0, link_rx_ready = 0;
	uint64_t link_tx_flit = 0;
	int vr_svc = 1, vr_svd = 1;
	bool rail_flagged = false;
	bool retired = false;		// an instruction has retired since power-on

	void pins_pre_edge()
	{
		vr_svc = top->svc;
		vr_svd = top->svd_o;
		link_tx_valid = top->iol_tx_valid;
		link_tx_flit = top->iol_tx_flit;
		link_rx_ready = top->iol_rx_ready;
	}

	void pins_post_edge()
	{
		int txr = 0, rxv = 0;
		uint64_t rxf = 0;
		iod_cycle(io, link_tx_valid, link_tx_flit, link_rx_ready, &txr, &rxv, &rxf);
		top->iol_tx_ready = txr;
		top->iol_rx_valid = rxv;
		top->iol_rx_flit = rxf;
		ddr5_eval_pins(brd.ddr5, *top, cycles);
		// the VRs: SVD is open drain (the line is low if either pulls it),
		// PWROK is their power-good (D-019)
		int svd = 1, svt = 1;
		vr_cycle(vr, vr_svc, vr_svd, &svd, &svt);
		top->svd_i = vr_svd && svd;
		top->svt = svt;
		top->pwrok = vr_power_good(vr);
	}

	// Board events: the chipset's NMI, SMI and machine checks are
	// messages on the I/O link, and so is the 8259's INTR (a virtual wire
	// with its vector, resent after each INTA the die sends back: iod.c).
	// MSIs are link writes (D-017, D-019).

	void events()
	{
		struct board_event e;
		if (board_event(&brd, &e)) {
			switch (e.kind) {
			case EV_SMI:
				iod_vw(io, VW_SMI, 1, 0);
				halted_for[0] = 0;
				break;
			case EV_NMI:
				iod_vw(io, VW_NMI, 1, 0);
				halted_for[0] = 0;
				break;
			case EV_MCE:
				iod_mce(io, e.type & 3, e.addr);
				break;
			default:
				break;
			}
		}
		uint8_t v = 0;
		int p = board_pic_pending(&brd, &v);
		if (p)
			halted_for[0] = 0;
		iod_intr(io, p, v);
	}

	void step()
	{
		if (board_suspended(&brd)) {	// the PMC stopped the core's clock
			iod_board_tick(io);
			cycles++;
			if (cycles >= next_key)
				inject();
			return;
		}
		jtag_pins();
		top->clk = 0;
		top->eval();
		pins_pre_edge();
		events();
		top->clk = 1;
		top->eval();
		pins_post_edge();
		iod_board_tick(io);
		cycles++;
		if (cycles >= next_key)
			inject();
		for (unsigned c = 0; c < top->ncores(); c++) {
			const cpu::retire &r = top->rt[c];
			const cpu::event &e = top->ev[c];
			// with several cores, each line names its core ("c1 ")
			char cs[8] = "";
			if (top->ncores() > 1)
				std::snprintf(cs, sizeof cs, "c%u ", c);
			if (r.valid) {
				last_rip[c] = r.rip;
				retired = true;
			}
			if (r.valid && !vr_cpu_rail_set(vr) && !rail_flagged) {
				// the die's sequencer must not run the cores before their
				// rail is at the voltage it set (docs/package.md)
				std::fprintf(stderr, "vsim: an instruction retired with VDDCR_CPU at %u mV, "
				    "not at a voltage the die set and the VR settled\n", vr_mv(vr, 0));
				rail_flagged = true;
				brd.power = PWR_FAIL;
				brd.failcode = 0x5652;		/* 'VR' */
			}
			if (trace && r.valid) {
				char hex[3 * 16 + 1] = "";
				unsigned len = r.len > 15 ? 15 : r.len;
				for (unsigned i = 0; i < len; i++)
					std::snprintf(hex + 3 * i, 4, "%02x ", r.bytes[i]);
				std::fprintf(stderr, "%10llu %s%016llx  %-45s (%u uops)\n",
				    (unsigned long long)cycles, cs, (unsigned long long)r.rip, hex,
				    r.uops);
			}
			if (trace && e.valid) {
				char err[24] = "";
				if (e.has_error)
					std::snprintf(err, sizeof err, "  error %08x", e.error);
				std::fprintf(stderr, "%10llu %s    %s (vector %u)%s  rip %016llx\n",
				    (unsigned long long)cycles, cs, vector_name(e.vector), e.vector,
				    err, (unsigned long long)e.rip);
			}
			if (stop != RUNNING)
				continue;
			if (top->shutdown[c]) {
				stop = SHUTDOWN;
				stop_core = c;
				stop_rip = top->shutdown_rip[c];
			} else if (top->halted[c]) {
				if (++halted_for[c] >= halt_idle) {
					stop = HALTED;
					stop_core = c;
					stop_rip = last_rip[c];	// the HLT
				}
			} else
				halted_for[c] = 0;
		}
	}

	bool running() const { return stop == RUNNING && brd.power == PWR_ON; }
};

void
post_trace(void *ctx, uint8_t code)
{
	const machine *m = static_cast<const machine *>(ctx);
	std::fprintf(stderr, "%10llu     POST %02x\n", (unsigned long long)m->cycles, code);
}

// ---------------------------------------------------------------- VGA

uint32_t vga_pix[VGA_MAXW * VGA_MAXH];

void
dump_vga(const vga &g, const char *bmp, const char *txt)
{
	if (bmp) {
		int w, h;
		vga_render(&g, vga_pix, &w, &h);
		SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, vga_pix, w * 4);
		if (!s || !SDL_SaveBMP(s, bmp))
			std::fprintf(stderr, "vsim: %s: %s\n", bmp, SDL_GetError());
		SDL_DestroySurface(s);
	}
	if (txt) {
		char lines[25][81];
		vga_text(&g, lines);
		FILE *f = std::fopen(txt, "w");
		if (!f) {
			std::fprintf(stderr, "vsim: %s: %s\n", txt, std::strerror(errno));
			return;
		}
		for (int i = 0; i < 25; i++) {
			int n = 80;
			while (n && lines[i][n - 1] == ' ')
				n--;
			std::fprintf(f, "%.*s\n", n, lines[i]);
		}
		std::fclose(f);
	}
}


// why the core stopped (shutdown, halted): one line on f
void
report_stop(const machine &m, FILE *f)
{
	char cs[16] = "";
	if (m.top->ncores() > 1)
		std::snprintf(cs, sizeof cs, " (core %u)", m.stop_core);
	if (m.stop == machine::SHUTDOWN)
		std::fprintf(f, "vsim: shutdown (triple fault)%s at rip %016llx after %llu cycles\n",
		    cs, (unsigned long long)m.stop_rip, (unsigned long long)m.cycles);
	else if (m.stop == machine::HALTED)
		std::fprintf(f, "vsim: halted%s at rip %016llx after %llu cycles\n", cs,
		    (unsigned long long)m.stop_rip, (unsigned long long)m.cycles);
}

// ---------------------------------------------------------------- headless

// stdin (a terminal, a pipe or a file) is the UART's receive side: bytes
// are moved in as the receive FIFO has room, so piped input is never lost
struct serial_in {
	bool eof = false;
	uint8_t buf[256];
	size_t n = 0, i = 0;

	void pump(uart16550 &u)
	{
		for (;;) {
			if (i == n) {
				if (eof)
					return;
				pollfd p = { STDIN_FILENO, POLLIN, 0 };
				if (poll(&p, 1, 0) <= 0)
					return;
				ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
				if (r <= 0) {
					eof = true;
					return;
				}
				n = (size_t)r;
				i = 0;
			}
			if (u.rxt - u.rxh >= sizeof u.rx)
				return;
			uart_rx_push(&u, buf[i] == '\n' ? '\r' : buf[i]);
			i++;
		}
	}
};

int
run_headless(machine &m, uint64_t max)
{
	m.brd.uart.tx = tx_stdout;
	serial_in in;
	m.reset();
	for (;;) {
		while (m.running() && m.cycles < max) {
			for (int k = 0; k < 4096 && m.running() && m.cycles < max; k++)
				m.step();
			in.pump(m.brd.uart);
		}
		if (m.brd.power == PWR_RESET) {
			m.reset();
			continue;
		}
		break;
	}
	std::fflush(stdout);
	if (m.stop != machine::RUNNING) {
		report_stop(m, stderr);
		return m.stop == machine::SHUTDOWN ? 2 : 6;
	}
	if (m.brd.power == PWR_FAIL) {
		std::fprintf(stderr, "vsim: board reported failure %u after %llu cycles\n",
		    m.brd.failcode, (unsigned long long)m.cycles);
		return 1;
	}
	if (m.brd.power == PWR_OFF)
		return 0;
	if (m.brd.power == PWR_THERMAL) {
		std::fprintf(stderr, "vsim: thermal shutdown: die %lld mC (critical trip %lld mC)\n",
		    (long long)pmc_die_mc(&m.brd.pmc), (long long)m.brd.pmc.trip_crit);
		return 5;
	}
	std::fprintf(stderr, "vsim: stopped after %llu cycles\n", (unsigned long long)m.cycles);
	if (!m.retired)
		std::fprintf(stderr, "vsim: the cores never left reset\n");
	return 3;
}

// ---------------------------------------------------------------- window

int
run_window(machine &m, const opts &o)
{
	m.brd.uart.tx = tx_stdout;
	// terminal input goes to the UART too (non-blocking)
	bool tty_in = isatty(STDIN_FILENO);
	if (tty_in)
		fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		std::fprintf(stderr, "vsim: SDL_Init: %s (use -n for headless)\n", SDL_GetError());
		return 4;
	}
	SDL_Window *win = nullptr;
	SDL_Renderer *ren = nullptr;
	if (!SDL_CreateWindowAndRenderer("vibe86", 640 * o.scale, 480 * o.scale,
	    SDL_WINDOW_RESIZABLE, &win, &ren)) {
		std::fprintf(stderr, "vsim: window: %s\n", SDL_GetError());
		SDL_Quit();
		return 4;
	}
	SDL_SetRenderVSync(ren, 1);
	// every mode is shown at 4:3, like a monitor (text: 720x400 stretched)
	SDL_SetRenderLogicalPresentation(ren, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888,
	    SDL_TEXTUREACCESS_STREAMING, VGA_MAXW, VGA_MAXH);
	SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
	// with a USB controller the window is its keyboard and mouse; without
	// one, typed text goes to the UART
	pcidev *usb = m.brd.xhci;
	if (!usb)
		SDL_StartTextInput(win);
	float mx = 0, my = 0;		// sub-pixel mouse motion not sent yet

	m.reset();
	bool quit = false;
	char title[128];
	const char *last = nullptr;
	int lw = 640, lh = 480;
	while (!quit) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_EVENT_QUIT)
				quit = true;
			else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_PAUSE) {
				if (!ev.key.repeat)
					pmc_button(&m.brd.pmc);
			} else if (usb) {
				// SDL scancodes are the HID Keyboard/Keypad page's usage IDs
				switch (ev.type) {
				case SDL_EVENT_KEY_DOWN:
				case SDL_EVENT_KEY_UP:
					if (!ev.key.repeat && ev.key.scancode >= 4 && ev.key.scancode <= 0xE7)
						xhci_key(usb, (unsigned)ev.key.scancode, ev.type == SDL_EVENT_KEY_DOWN);
					break;
				case SDL_EVENT_WINDOW_FOCUS_LOST:
					xhci_key_release_all(usb);
					for (unsigned b = 0; b < 3; b++)
						xhci_mouse_button(usb, b, 0);
					break;
				case SDL_EVENT_MOUSE_MOTION: {
					mx += ev.motion.xrel;
					my += ev.motion.yrel;
					int dx = (int)mx, dy = (int)my;
					mx -= (float)dx;
					my -= (float)dy;
					if (dx || dy)
						xhci_mouse_move(usb, dx, dy);
					break;
				}
				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_MOUSE_BUTTON_UP: {
					// HID buttons: 1 left, 2 right, 3 middle
					unsigned b = ev.button.button == SDL_BUTTON_LEFT ? 0
					    : ev.button.button == SDL_BUTTON_RIGHT ? 1
					    : ev.button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
					if (b < 3)
						xhci_mouse_button(usb, b, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
					break;
				}
				default:
					break;
				}
			} else if (ev.type == SDL_EVENT_TEXT_INPUT) {
				for (const char *p = ev.text.text; *p; p++)
					if ((unsigned char)*p < 0x80)
						uart_rx_push(&m.brd.uart, (uint8_t)*p);
			} else if (ev.type == SDL_EVENT_KEY_DOWN) {
				switch (ev.key.key) {
				case SDLK_RETURN: uart_rx_push(&m.brd.uart, '\r'); break;
				case SDLK_BACKSPACE: uart_rx_push(&m.brd.uart, 0x08); break;
				case SDLK_TAB: uart_rx_push(&m.brd.uart, '\t'); break;
				case SDLK_ESCAPE: uart_rx_push(&m.brd.uart, 0x1B); break;
				default: break;
				}
			}
		}
		if (tty_in) {
			uint8_t buf[64];
			ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
			for (ssize_t i = 0; i < n; i++)
				uart_rx_push(&m.brd.uart, buf[i] == '\n' ? '\r' : buf[i]);
		}

		// run for most of a frame
		Uint64 t0 = SDL_GetTicksNS();
		while (m.running() && SDL_GetTicksNS() - t0 < 12000000ull) {
			for (int i = 0; i < 4096 && m.running(); i++)
				m.step();
			if (o.max_cycles && m.cycles >= o.max_cycles)
				break;
		}
		if (m.brd.power == PWR_RESET)
			m.reset();
		if (o.exit_on_off && !m.running())
			quit = true;

		// the board's state goes in the title; the window is the VGA only
		const char *st = m.stop == machine::SHUTDOWN ? "shutdown (triple fault, see stderr)"
		    : m.stop == machine::HALTED ? "halted"
		    : m.brd.power == PWR_THERMAL ? "thermal shutdown"
		    : board_suspended(&m.brd) ? "suspended (Pause: power button)"
		    : m.brd.power == PWR_OFF ? "powered off"
		    : m.brd.power == PWR_FAIL ? "powered off (failure)"
		    : (o.max_cycles && m.cycles >= o.max_cycles) ? "cycle limit" : "running";
		if (st != last) {
			std::snprintf(title, sizeof title, "vibe86 - %s", st);
			SDL_SetWindowTitle(win, title);
			last = st;
		}

		int w, h;
		vga_render(&m.brd.vga, vga_pix, &w, &h);
		if (w > 0 && h > 0 && (w != lw || h != lh)) {
			SDL_SetRenderLogicalPresentation(ren, w, w * 3 / 4, SDL_LOGICAL_PRESENTATION_LETTERBOX);
			lw = w;
			lh = h;
		}
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
		SDL_RenderClear(ren);
		if (tex && w > 0 && h > 0) {
			SDL_Rect src = { 0, 0, w, h };
			SDL_UpdateTexture(tex, &src, vga_pix, w * 4);
			SDL_FRect fsrc = { 0, 0, (float)w, (float)h };
			SDL_RenderTexture(ren, tex, &fsrc, nullptr);	// fills the 4:3 picture
		}
		if (quit && o.shot) {
			SDL_Surface *img = SDL_RenderReadPixels(ren, nullptr);
			if (!img || !SDL_SaveBMP(img, o.shot))
				std::fprintf(stderr, "vsim: screenshot: %s\n", SDL_GetError());
			SDL_DestroySurface(img);
		}
		SDL_RenderPresent(ren);

		static bool reported;
		if (m.stop != machine::RUNNING && !reported) {
			report_stop(m, stderr);
			reported = true;
		}
		if (!m.running())
			SDL_Delay(16);
	}
	std::fflush(stdout);

	if (!usb)
		SDL_StopTextInput(win);
	if (tex)
		SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	if (m.stop != machine::RUNNING)
		return m.stop == machine::SHUTDOWN ? 2 : 6;
	if (m.brd.power == PWR_THERMAL)
		return 5;
	return m.brd.power == PWR_FAIL ? 1 : 0;
}

/*
 * -e: an ELF64 x86-64 executable (ld.lld output, say). Each PT_LOAD segment
 * goes to its physical address (p_paddr; equal to p_vaddr unless the linker
 * script says AT), with the bytes past p_filesz zeroed, through
 * board_preload: the test board's SRAM or the EEPROM range. The entry point
 * is not used: the core starts at the reset vector.
 */
int
load_elf(board &b, const char *path)
{
	FILE *f = std::fopen(path, "rb");
	if (!f) {
		std::fprintf(stderr, "vsim: %s: %s\n", path, std::strerror(errno));
		return -1;
	}
	std::vector<uint8_t> img;
	uint8_t chunk[65536];
	size_t n;
	while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0)
		img.insert(img.end(), chunk, chunk + n);
	std::fclose(f);
	Elf64_Ehdr eh;
	if (img.size() < sizeof eh) {
		std::fprintf(stderr, "vsim: %s: not an ELF file\n", path);
		return -1;
	}
	std::memcpy(&eh, img.data(), sizeof eh);
	if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_X86_64) {
		std::fprintf(stderr, "vsim: %s: not an ELF64 x86-64 file\n", path);
		return -1;
	}
	int loaded = 0;
	for (unsigned i = 0; i < eh.e_phnum; i++) {
		Elf64_Phdr ph;
		uint64_t off = eh.e_phoff + (uint64_t)i * eh.e_phentsize;
		if (eh.e_phentsize < sizeof ph || off + sizeof ph > img.size()) {
			std::fprintf(stderr, "vsim: %s: bad program header table\n", path);
			return -1;
		}
		std::memcpy(&ph, img.data() + off, sizeof ph);
		if (ph.p_type != PT_LOAD || ph.p_memsz == 0)
			continue;
		if (ph.p_filesz > ph.p_memsz || ph.p_offset + ph.p_filesz > img.size() ||
		    ph.p_offset + ph.p_filesz < ph.p_offset) {
			std::fprintf(stderr, "vsim: %s: segment %u is out of the file\n", path, i);
			return -1;
		}
		std::vector<uint8_t> seg(ph.p_memsz, 0);
		std::memcpy(seg.data(), img.data() + ph.p_offset, ph.p_filesz);
		if (board_preload(&b, ph.p_paddr, seg.data(), seg.size()))
			return -1;
		loaded++;
	}
	if (!loaded) {
		std::fprintf(stderr, "vsim: %s: no PT_LOAD segments\n", path);
		return -1;
	}
	return 0;
}

} // namespace

int
main(int argc, char **argv)
{
	opts o;
	int c;

	while ((c = getopt(argc, argv, "b:ndJ:m:F:c:H:tvxs:S:P:X:U:K:V:Y:T:l:e:N:C:M:g:f:D:W:")) != -1) {
		switch (c) {
		case 'b':
			if (!std::strcmp(optarg, "server"))
				o.board_kind = BOARD_SERVER;
			else if (!std::strcmp(optarg, "test"))
				o.board_kind = BOARD_TEST;
			else
				usage();
			break;
		case 'n': o.headless = true; break;
		case 'd': o.debug = true; break;
		case 'J':
			if (!std::strcmp(optarg, "ecc-skip"))
				o.ecc_strap = 1;
			else if (!std::strcmp(optarg, "ecc-check"))
				o.ecc_strap = 0;
			else
				usage();
			break;
		case 'm':
			if (!o.dimms_given) {
				std::memset(o.dimms, 0, sizeof o.dimms);
				o.dimms_given = true;
			}
			if (std::strcmp(optarg, "none") && ddr5_parse_slot(optarg, o.dimms)) {
				std::fprintf(stderr, "vsim: bad DIMM '%s'\n", optarg);
				usage();
			}
			break;
		case 'v': o.verbose = true; break;
		case 'P':
			if (!std::strcmp(optarg, "none"))
				o.pci_slot = -1;
			else {
				o.pci_slot = std::atoi(optarg);
				if (o.pci_slot < 0 || o.pci_slot > 7)
					usage();
			}
			break;
		case 'X':
			if (!std::strcmp(optarg, "none"))
				o.xhci_slot = -1;
			else {
				char *end;
				long v = std::strtol(optarg, &end, 10);
				if (*end || end == optarg || v < 0 || v > 7)
					usage();
				o.xhci_slot = (int)v;
			}
			break;
		case 'U': {
			char *c = std::strrchr(optarg, ',');
			if (c && !std::strcmp(c, ",ro")) {
				*c = 0;
				o.usb_floppy_ro = true;
			}
			o.usb_floppy = optarg;
			break;
		}
		case 'K': o.keys = optarg; break;
		case 'D':
			o.jtag_port = std::atoi(optarg);
			if (o.jtag_port <= 0 || o.jtag_port > 65535)
				usage();
			break;
		case 'c': o.max_cycles = std::strtoull(optarg, nullptr, 0); break;
		case 'H': o.halt_idle = std::strtoull(optarg, nullptr, 0); break;
		case 'e':
			if (o.nelf == 16)
				usage();
			o.elf[o.nelf++] = optarg;
			break;
		case 'l':
			if (o.npreload == 16 || !std::strchr(optarg, '@'))
				usage();
			o.preload[o.npreload++] = optarg;
			break;
		case 't': o.trace = true; break;
		case 'x': o.exit_on_off = true; break;
		case 'S': o.shot = optarg; break;
		case 'V': o.vga_bmp = optarg; break;
		case 'Y': o.vga_txt = optarg; break;
		case 'T': tpm_set_state_file(optarg); break;
		case 'N': o.nvram = optarg; break;
		case 'M':
			// the test board's SRAM is gone (D-014): memory is the DIMMs (-m)
			std::fprintf(stderr, "vsim: -M is obsolete (memory is the DIMMs, -m); ignored\n");
			break;
		case 'F': o.meminit_fast = std::atoi(optarg) != 0; break;
		case 'g': o.debugcon = optarg; break;
		case 'f':
			if (o.nfwcfg == 16 || !std::strchr(optarg, '='))
				usage();
			o.fwcfg[o.nfwcfg++] = optarg;
			break;
		case 'C': o.cpu = optarg; break;
		case 'W': o.vr_faults.push_back(optarg); break;
		case 's': o.scale = std::atoi(optarg); if (o.scale < 1) o.scale = 1; break;
		default: usage();
		}
	}
	// the flash image is optional when -e or -l put the program in memory
	if (optind == argc - 1)
		o.image = argv[optind];
	else if (optind != argc || (!o.nelf && !o.npreload))
		usage();

	machine m;
	m.ctx = std::make_unique<VerilatedContext>();
	m.top = cpu_new(o.cpu ? o.cpu : cpu_default(), m.ctx.get());
	if (!m.top)
		return 4;
	m.trace = o.trace;
	if (o.jtag_port) {
		m.jtag = new jtag_bitbang;
		if (jtag_bitbang_open(m.jtag, o.jtag_port))
			return 4;
	}
	m.halt_idle = o.halt_idle ? o.halt_idle : 1;
	if (board_init(&m.brd, o.board_kind, o.image, o.dimms_given ? o.dimms : nullptr,
	    o.meminit_fast) || !(m.io = iod_new(&m.brd)))
		return 4;
	m.brd.iod = m.io;
	if (!(m.vr = vr_new()))
		return 4;
	for (const char *f : o.vr_faults)
		if (vr_fault(m.vr, f)) {
			std::fprintf(stderr, "vsim: -W %s: not a VR fault (vr0:nak, vr1:stuck, vr0:parity)\n", f);
			return 4;
		}
	vr_power_on(m.vr);
	m.top->meminit_fast = o.meminit_fast;
	if (o.debugcon) {
		FILE *dc = !std::strcmp(o.debugcon, "-") ? stderr : std::fopen(o.debugcon, "w");
		if (!dc) {
			std::fprintf(stderr, "vsim: %s: %s\n", o.debugcon, std::strerror(errno));
			return 4;
		}
		m.brd.debugcon = [](void *ctx, uint8_t c) {
			std::fputc(c, (FILE *)ctx);
			if (c == '\n')
				std::fflush((FILE *)ctx);
		};
		m.brd.dbgctx = dc;
	}
	for (int i = 0; i < o.nfwcfg; i++) {
		std::string spec(o.fwcfg[i]);
		size_t eq = spec.find('=');
		std::string name = spec.substr(0, eq), path = spec.substr(eq + 1);
		FILE *f = std::fopen(path.c_str(), "rb");
		if (!f) {
			std::fprintf(stderr, "vsim: %s: %s\n", path.c_str(), std::strerror(errno));
			return 4;
		}
		std::vector<uint8_t> buf;
		uint8_t chunk[65536];
		size_t n;
		while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0)
			buf.insert(buf.end(), chunk, chunk + n);
		std::fclose(f);
		if (pc_fwcfg_add(&m.brd.pc, name.c_str(), buf.data(), (uint32_t)buf.size())) {
			std::fprintf(stderr, "vsim: fw_cfg: can't add %s\n", name.c_str());
			return 4;
		}
	}
	if (o.pci_slot >= 0 && o.pci_slot == o.xhci_slot) {
		std::fprintf(stderr, "vsim: the DMA test endpoint (-P) and the xHCI controller (-X) "
		    "are both in PCIe slot %d\n", o.pci_slot);
		return 4;
	}
	if ((o.pci_slot != 0 || o.xhci_slot != 1) &&
	    board_pci_slots(&m.brd, o.pci_slot, o.xhci_slot))
		return 4;
	if (o.usb_floppy) {
		if (!m.brd.xhci) {
			std::fprintf(stderr, "vsim: -U needs the xHCI controller (-X)\n");
			return 4;
		}
		if (xhci_msc_attach(m.brd.xhci, o.usb_floppy, o.usb_floppy_ro)) {
			std::fprintf(stderr, "vsim: %s: not a readable disk image (a multiple of 512 bytes)\n",
			    o.usb_floppy);
			return 4;
		}
	}
	if (o.keys) {
		std::string script = o.keys;
		if (script[0] == '@') {		// -K @file: the script is in a file
			FILE *f = std::fopen(o.keys + 1, "r");
			if (!f) {
				std::fprintf(stderr, "vsim: %s: %s\n", o.keys + 1, std::strerror(errno));
				return 4;
			}
			script.clear();
			int ch;
			while ((ch = std::fgetc(f)) != EOF)
				script += (char)ch;
			std::fclose(f);
		}
		if (!parse_keys(script.c_str(), m.keys))
			return 4;
		m.next_key = m.keys.empty() ? UINT64_MAX : m.keys[0].at;
	}
	if (o.nvram && board_nvram_file(&m.brd, o.nvram)) {
		std::fprintf(stderr, "vsim: %s: %s\n", o.nvram, std::strerror(errno));
		return 4;
	}
	for (int i = 0; i < o.npreload; i++) {
		// -l file@addr: the last '@' separates the address
		std::string spec(o.preload[i]);
		size_t at = spec.rfind('@');
		std::string path = spec.substr(0, at);
		uint64_t addr = std::strtoull(spec.c_str() + at + 1, nullptr, 0);
		FILE *f = std::fopen(path.c_str(), "rb");
		if (!f) {
			std::fprintf(stderr, "vsim: %s: %s\n", path.c_str(), std::strerror(errno));
			return 4;
		}
		std::vector<uint8_t> buf;
		uint8_t chunk[65536];
		size_t n;
		while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0)
			buf.insert(buf.end(), chunk, chunk + n);
		std::fclose(f);
		if (board_preload(&m.brd, addr, buf.data(), buf.size()))
			return 4;
	}
	for (int i = 0; i < o.nelf; i++)
		if (load_elf(m.brd, o.elf[i]))
			return 4;
	if (o.trace) {
		m.brd.post_cb = post_trace;
		m.brd.post_ctx = &m;
	}
	// the ECC_SKIP jumper is fitted on debug boards unless asked otherwise
	if (o.ecc_strap == 1 || (o.ecc_strap < 0 && o.debug))
		m.brd.straps |= STRAP_ECC_SKIP;

	int r = o.headless ? run_headless(m, o.max_cycles ? o.max_cycles : 100000000ull)
	    : run_window(m, o);
	if (o.verbose)
		std::fprintf(stderr, "vsim: POST code %02x (%u written)\n", m.brd.post, m.brd.nposts);
	if (o.verbose && m.brd.ddr5) {
		char buf[256];
		ddr5_stats(m.brd.ddr5, buf, sizeof buf);
		std::fprintf(stderr, "vsim: %s\n", buf);
		iod_stats(m.io, buf, sizeof buf);
		std::fprintf(stderr, "vsim: %s\n", buf);
	}
	if (o.verbose) {
		char buf[256];
		vr_stats(m.vr, buf, sizeof buf);
		std::fprintf(stderr, "vsim: %s\n", buf);
	}
	dump_vga(m.brd.vga, o.vga_bmp, o.vga_txt);
	m.top->final();
	board_free(&m.brd);
	iod_free(m.io);
	vr_free(m.vr);
	return r;
}
