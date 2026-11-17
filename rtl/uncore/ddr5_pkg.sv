// ddr5_pkg: DDR5 command encodings, timings and the ECC code of the memory
// controllers (rtl/uncore/ddr5_ctrl.sv) and the init engine (meminit.sv).
//
// Command encodings: Micron, "DDR5 SDRAM Product Core Data Sheet",
// ddr5_sdram_core.pdf Rev. D 10/2022 (JESD79-5), Table 140 "Command Truth
// Table": two-cycle commands put their first half on CA[13:0] with CS_n LOW
// and the second half on the next clock with CS_n HIGH (non-target ODT
// variants aside); one-cycle commands drive CS_n LOW for one clock.
// Timings: DDR5-4800 (tCK 0.416 ns, one controller clock), the 4800B bin of
// Table 361 (CL-nRCD-nRP 40-39-39), the formulas of Tables 405-407 / the
// DDR5 4400-5200 tables evaluated at tCK = 0.416 ns, tRFC1 for 16 Gb from
// Table 176 (295 ns), tREFI 3.9 us. ECC: M. Y. Hsiao, "A Class of Optimal
// Minimum Odd-weight-column SEC-DED Codes", IBM J. Res. Dev. 14(4):395-401,
// 1970, https://doi.org/10.1147/rd.144.0395 — (39,32) per beat on CB[6:0].
package ddr5_pkg;

  /* verilator lint_off UNUSEDPARAM */   // a parameter table: each user takes what it needs
  // ---------------------------------------------------------------- timing
  // all in controller clocks (= nCK at DDR5-4800)
  localparam int T_CL      = 40;     // MR0 CL40 (4800B)
  localparam int T_CWL     = 38;     // CWL = CL - 2 (Table 21 note 2)
  localparam int T_RCD     = 39;     // 16 ns
  localparam int T_RP      = 39;     // 16 ns
  localparam int T_RAS     = 77;     // 32 ns
  localparam int T_RC      = 116;    // 48 ns
  localparam int T_RRD_S   = 8;
  localparam int T_RRD_L   = 12;     // max(8 nCK, 5 ns), rounded per the datasheet's rules
  localparam int T_FAW     = 32;     // 1 KB page: max(32 nCK, 13.333 ns)
  localparam int T_CCD_S   = 8;
  localparam int T_CCD_L   = 12;     // MR13 OP[3:0] = 0100b at 4400 < rate <= 4800 (Table 34)
  localparam int T_CCD_L_WR = 48;    // Table 34 (MR13 = 0100b)
  localparam int T_RTP     = 19;     // max(12 nCK, 7.5 ns)
  localparam int T_WR      = 73;     // 30 ns
  localparam int T_WTR_L   = 25;     // max(16 nCK, 10 ns)   (after CWL + BL/2)
  localparam int T_WTR_S   = 7;      // max(4 nCK, 2.5 ns)
  localparam int T_RFC1    = 709;    // 295 ns (16 Gb)
  localparam int T_REFI    = 9375;   // 3.9 us
  localparam int T_MRW     = 13;     // max(5 ns, 8 nCK)
  localparam int T_MRD     = 34;     // max(14 ns, 16 nCK)
  localparam int T_MPC     = 34;     // tMPC_delay = tMRD
  localparam int T_ZQCAL   = 2404;   // 1 us
  localparam int T_ZQLAT   = 73;     // max(30 ns, 8 nCK)
  localparam int T_DLLK    = 1536;    // Table 34 (MR13 = 0100b)
  localparam int T_BURST   = 8;      // BL16: 8 clocks of 2 beats
  // multicycle MPC before CS/CA training (MR2:OP[4] = 0): CA stable 3 nCK
  // before CS_n falls, CS_n low 3.5-8 nCK, CA held 3 nCK after (tMC_MPC_Setup,
  // tMPC_CS, tMC_MPC_Hold; Figure 11, the AC timing tables)
  localparam int T_MPC_SETUP = 3;
  localparam int T_MPC_CS    = 4;
  localparam int T_MPC_HOLD  = 3;
  // initialization (Table 10): tINIT1 200 us, tINIT3 4 ms, tINIT4 2 us,
  // tINIT5 3 nCK, tXPR = tXS = tRFC1; divided by 1024 with meminit_fast
  localparam int T_INIT1   = 480769;
  localparam int T_INIT2   = 25;     // 10 ns
  localparam int T_INIT3   = 9615385;
  localparam int T_INIT4   = 4808;
  localparam int T_INIT5   = 3;
  localparam int T_XPR     = T_RFC1;

  /* verilator lint_on UNUSEDPARAM */
  // ---------------------------------------------------------------- commands
  // {cycle 2 CA, cycle 1 CA}; 2-cycle commands use both
  function automatic logic [27:0] cmd_act(input logic [15:0] row, input logic [2:0] bg,
                                          input logic [1:0] ba);
    logic [13:0] c1, c2;
    c1 = {3'b000, bg, ba, row[3:0], 2'b00};        // CA0=L CA1=L, R0-R3, BA0-1, BG0-2, CID0-2
    c2 = {1'b0, 1'b0, row[15:4]};                   // R4-R15, R16=L, CID3/R17=L
    return {c2, c1};
  endfunction

  // RD: cycle 1 CA[4:0] = H L H H H (CA0..CA4), CA5 BL* = H (MR0's burst
  // length), BA on CA6-7, BG on CA8-10, CID0-2 on CA11-13; cycle 2 C2..C10 on
  // CA0..CA8, CA10 = H (no auto precharge) / L (RDA), CA13 = CID3
  /* verilator lint_off UNUSEDSIGNAL */  // col[1:0]/[2:0]: inside the burst, not sent
  function automatic logic [27:0] cmd_rd(input logic [9:0] col, input logic [2:0] bg,
                                         input logic [1:0] ba, input logic ap);
    logic [13:0] c1, c2;
    c1 = {3'b000, bg, ba, 1'b1, 5'b11101};
    c2 = {3'b000, ~ap, 1'b0, 1'b0, col[9:2]};        // CA8 = C10 = L (x8: C0-C9)
    return {c2, c1};
  endfunction

  // WR: cycle 1 CA[4:0] = H L H H L, then as RD; cycle 2 CA0 V, C3..C10 on
  // CA1..CA8, CA10 = H (no AP) / L (WRA), CA11 WR_P = H (a full write; L
  // would announce a partial write), CA13 = CID3
  function automatic logic [27:0] cmd_wr(input logic [9:0] col, input logic [2:0] bg,
                                         input logic [1:0] ba, input logic ap);
    logic [13:0] c1, c2;
    c1 = {3'b000, bg, ba, 1'b1, 5'b01101};
    c2 = {2'b00, 1'b1, ~ap, 1'b0, 1'b0, col[9:3], 1'b0};
    return {c2, c1};
  endfunction
  /* verilator lint_on UNUSEDSIGNAL */

  function automatic logic [13:0] cmd_pre_ab();
    return {3'b000, 1'b0, 3'b000, 2'b00, 5'b01011};   // H H L H L, CA10 = L (all banks)
  endfunction

  function automatic logic [13:0] cmd_pre_pb(input logic [2:0] bg, input logic [1:0] ba);
    return {3'b000, bg, ba, 1'b0, 5'b11011};          // H H L H H, BA, BG
  endfunction

  function automatic logic [13:0] cmd_ref_ab();
    // H H L L H, CA8 = H (1x rate when RIR is enabled; else V), CA9 = H, CA10 = L
    return {3'b000, 1'b0, 1'b1, 1'b1, 2'b00, 1'b0, 5'b10011};
  endfunction

  function automatic logic [27:0] cmd_mrw(input logic [7:0] mra, input logic [7:0] op);
    logic [13:0] c1, c2;
    c1 = {1'b0, mra, 5'b00101};                       // H L H L L, MRA0-7 on CA5-12
    c2 = {3'b000, 1'b0, 2'b00, op};                   // OP0-7 on CA0-7, CA10 CW = L
    return {c2, c1};
  endfunction

  function automatic logic [13:0] cmd_mpc(input logic [7:0] op);
    return {1'b0, op, 5'b01111};                      // H H H H L, OP0-7 on CA5-12
  endfunction

  localparam logic [13:0] CA_NOP = 14'h3FFF;          // NOP: H H H H H (CS_n LOW)

  /* verilator lint_off UNUSEDPARAM */
  // MPC opcodes (Table 143)
  localparam logic [7:0] MPC_DLL_RESET = 8'h02;
  localparam logic [7:0] MPC_ZQCAL_LATCH = 8'h04;
  localparam logic [7:0] MPC_ZQCAL_START = 8'h05;
  localparam logic [7:0] MPC_SET_1N = 8'h09;
  localparam logic [7:0] MPC_TDLLK_CCDL = 8'h84;      // 1000xxxx: MR13 OP[3:0] = 0100b (4400 < rate <= 4800)
  // MR0: BL16 (OP[1:0] = 00), CL40 (OP[7:2] = 001001b) (Table 21)
  localparam logic [7:0] MR0_BL16_CL40 = 8'b0010_0100;
  /* verilator lint_on UNUSEDPARAM */

  // ---------------------------------------------------------------- ECC
  // Hsiao (39,32): data bit i's column is the i-th 7-bit value of odd
  // weight >= 3 in order (weight 3 ascending, then weight 5 ascending)
  function automatic logic [6:0] hsiao_col(input int i);
    int n;
    n = 0;
    for (int w = 3; w <= 5; w += 2)
      for (int v = 0; v < 128; v++)
        if ($countones(7'(v)) == w) begin
          if (n == i) return 7'(v);
          n++;
        end
    return 7'h0;
  endfunction

  function automatic logic [6:0] ecc_check(input logic [31:0] d);
    logic [6:0] c;
    c = '0;
    for (int i = 0; i < 32; i++)
      if (d[i]) c = c ^ hsiao_col(i);
    return c;
  endfunction

  // decode: corrected data, ce = corrected single-bit error, ue = uncorrectable
  function automatic logic [33:0] ecc_correct(input logic [31:0] d, input logic [6:0] c);
    logic [6:0] s;
    logic [31:0] o;
    logic ce, ue;
    s = ecc_check(d) ^ c;
    o = d;
    ce = 1'b0;
    ue = 1'b0;
    if (s != 7'h0) begin
      if ($countones(s) == 1) ce = 1'b1;            // a check bit flipped
      else begin
        ue = 1'b1;
        for (int i = 0; i < 32; i++)
          if (hsiao_col(i) == s) begin
            o[i] = ~o[i];
            ce = 1'b1;
            ue = 1'b0;
          end
      end
    end
    return {ue, ce, o};
  endfunction

endpackage
