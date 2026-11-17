// v1_vrf: the v1 core's vector-side register file (D-015).
//
// One file of 72 512-bit entries, indexed by the physical vector selector
// (the core resolves the decoder-relative VREG/VRM/VVVV/VIS4 and x87 ST(i)
// selectors of rtl/v1 uop VSEL before they get here):
//   0-31   ZMM0-ZMM31 (XMM/YMM are their low 128/256 bits)
//   32-39  VT0-VT7, vector micro temporaries
//   48-55  the x87 physical registers R0-R7 (80 bits used; ST(i) is
//          R[(TOP + i) mod 8], resolved by the core from FSW.TOP)
//   56-63  the opmask registers k0-k7 (64 bits used)
//   68-71  KT0-KT3, mask micro temporaries
// (40-47 and 64-67 are decoder-relative names: never stored.)
//
// Like the GPRs (D-005), every entry has a working copy, which micro-ops
// read and write, and an architectural copy: END commits the working copy
// and a fault rolls it back, so an instruction that faults half-way leaves
// no partial vector write (Hwu & Patt, "Checkpoint Repair for Out-of-order
// Execution Machines", ISCA 1987, doi:10.1145/30350.30352). Only the
// entries written since the last commit (a dirty mask) are copied either
// way. Writes are bit-masked, so a lane insert and a zero-upper are one
// write each.
module v1_vrf (
  input  logic         clk,
  input  logic         rst_n,
  // a combinational read port
  input  logic [6:0]   ra,
  output logic [511:0] rda,
  // one write port: entry, data, bit mask
  input  logic         we,
  input  logic [6:0]   wa,
  input  logic [511:0] wd,
  input  logic [511:0] wm,
  // checkpoint
  input  logic         commit,       // END: working -> architectural
  input  logic         rollback      // a fault: architectural -> working
);
  localparam int N = 72;

  logic [511:0] w [N];
  logic [511:0] a [N];
  logic [N-1:0] dirty;

  assign rda = (ra < 7'(N)) ? w[ra] : '0;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      // power-up: XMM/YMM/ZMM, opmasks and x87 registers +0 (Intel SDM Vol. 3A
      // Table 10-1, AMD APM Vol. 2 Table 14-1)
      for (int i = 0; i < N; i++) begin
        w[i] <= '0;
        a[i] <= '0;
      end
      dirty <= '0;
    end else begin
      if (rollback) begin
        for (int i = 0; i < N; i++)
          if (dirty[i]) w[i] <= a[i];
        dirty <= '0;
      end else begin
        if (commit) begin
          for (int i = 0; i < N; i++)
            if (dirty[i]) a[i] <= w[i];
        end
        if (we && wa < 7'(N)) begin
          w[wa] <= (w[wa] & ~wm) | (wd & wm);
          if (commit) begin
            // a write in the committing clock: the architectural copy too
            a[wa] <= (w[wa] & ~wm) | (wd & wm);
            dirty <= '0;
          end else dirty[wa] <= 1'b1;
        end else if (commit) dirty <= '0;
      end
    end
  end

endmodule
