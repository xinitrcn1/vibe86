/*
 * vsim's JTAG probe: an OpenOCD "remote_bitbang" server (vsim -D port) that
 * drives the die's TCK/TMS/TDI/TRST_n pins and reads TDO (docs/jtag.md).
 *
 * The protocol (OpenOCD User's Guide, "remote_bitbang" adapter driver;
 * OpenOCD's src/jtag/drivers/remote_bitbang.c): one ASCII character per
 * operation over TCP —
 *   '0'..'7'  write TCK, TMS, TDI (bits 2, 1, 0)
 *   'R'       read TDO: the server answers '0' or '1'
 *   'r'..'u'  TRST and SRST: 'r' neither asserted, 's' SRST, 't' TRST,
 *             'u' both
 *   'B', 'b'  the probe's LED (ignored)
 *   'Q'       the client quits
 * Unknown characters are ignored.
 *
 * jtag_bitbang_poll() is called once per simulated core cycle, before the
 * die is evaluated: it applies at most one pin change per cycle, so the die
 * sees every TCK edge, and answers reads with TDO as the previous change
 * left it.
 */
#ifndef JTAG_BITBANG_H
#define JTAG_BITBANG_H

#include <cstddef>

struct jtag_bitbang {
	int listen_fd = -1, fd = -1;
	/* the pins as the probe drives them; TRST and SRST: true = asserted */
	bool tck = false, tms = true, tdi = false, trst = false, srst = false;
	char buf[4096];
	size_t n = 0, i = 0;
	unsigned backoff = 0;		/* cycles until the next socket read attempt */
};

/* listen on 127.0.0.1:port; 0, or -1 with a message on stderr */
int jtag_bitbang_open(jtag_bitbang *jb, int port);
/* one simulated cycle: tdo is the die's TDO pin now */
void jtag_bitbang_poll(jtag_bitbang *jb, bool tdo);
void jtag_bitbang_close(jtag_bitbang *jb);

#endif
