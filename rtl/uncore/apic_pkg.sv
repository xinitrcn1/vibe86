// apic_pkg: the interrupt messages of the die's APIC bus (rtl/uncore/
// apic_bus.sv): IPIs from the local APICs' ICRs (Intel SDM Vol. 3A 12.6
// "Issuing Interprocessor Interrupts") and MSIs arriving over the I/O link
// (12.11 "Message Signalled Interrupts"), as one format.
package apic_pkg;
  typedef struct packed {
    logic [31:0] dest;       // destination field (8 bits in xAPIC messages and MSIs, 32 in x2APIC)
    logic        x2;         // dest is an x2APIC 32-bit ID (else 8 bits)
    logic        dm;         // destination mode: 0 physical, 1 logical
    logic [1:0]  sh;         // shorthand: 0 none, 1 self, 2 all including self, 3 all excluding self
    logic [2:0]  dlv;        // delivery mode: 0 fixed, 1 lowest priority, 2 SMI, 4 NMI, 5 INIT, 6 SIPI, 7 ExtINT
    logic        level;      // level: 1 assert (0 with dlv INIT and trig 1: INIT level de-assert)
    logic        trig;       // trigger mode: 0 edge, 1 level
    logic [7:0]  vec;
    logic [31:0] src;        // the sender's APIC ID (for the self and all-but-self shorthands); FFFFFFFFh for MSIs
  } apic_msg_t;

  /* verilator lint_off UNUSEDPARAM */
  localparam logic [2:0] DLV_FIXED = 3'd0, DLV_LOWEST = 3'd1, DLV_SMI = 3'd2, DLV_NMI = 3'd4,
                         DLV_INIT = 3'd5, DLV_SIPI = 3'd6, DLV_EXTINT = 3'd7;
  /* verilator lint_on UNUSEDPARAM */
endpackage
