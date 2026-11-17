// ddr5_sub: the memory controller of one DDR5 subchannel (32 DQ + 8 CB),
// driving its DRAMs' pins directly (docs/die-interface.md). One request at a
// time (a 64-byte line); open-page policy; refresh every tREFI.
//
// Initialization after the channel's RESET_n is released (Micron DDR5 core
// data sheet, "Power-up Initialization Sequence" and Table 10): CS_n LOW
// tINIT3, then HIGH (DES) tINIT4, NOP for tINIT5, tXPR, then multicycle MPCs
// (MR2:OP[4] = 0 before training): SET 1N command timing (2N is the reset
// default, MR2:OP[2] = 0), Configure tDLLK/tCCD_L (MR13), DLL RESET, ZQCal
// Start, ZQCal Latch (the order of steps 7a-c), MRW MR0 = BL16/CL40,
// and tDLLK before the first READ or WRITE. CS/CA/write-leveling training is
// not needed by the simulated DRAMs (the datasheet allows "host-provided
// successful training solutions"); a real PHY would train here.
//
// Address (the DRAM byte offset from the fabric; sim/ddr5.h uses the same
// map): [7:6] subchannel (decoded by the fabric), [13:8] C[9:4],
// [16:14] BG, [18:17] BA, [34:19] row, [35] rank (2-rank DIMMs) else DIMM,
// [36] DIMM (2-rank DIMMs).
module ddr5_sub
  import ddr5_pkg::*;
(
  input  logic         clk,
  input  logic         rst_n,
  // initialization (rtl/uncore/meminit.sv)
  input  logic         in_reset,       // the channel's RESET_n is LOW: CS_n LOW
  input  logic         start,          // RESET_n has gone HIGH: run the init sequence
  input  logic         fast,           // meminit_fast: init waits / 1024
  input  logic [3:0]   pop,            // populated chip selects (DIMM*2 + rank)
  input  logic         two_rank,
  output logic         ready,
  // requests (rtl/uncore/fabric.sv)
  input  logic         req_valid,
  output logic         req_ready,
  input  logic         req_we,
  input  logic [36:8]  req_off,        // the line's DRAM offset above the subchannel bits
  input  logic [511:0] req_wdata,
  input  logic [63:0]  req_be,
  output logic         rsp_valid,
  output logic [511:0] rsp_rdata,
  output logic         rsp_ue,         // an uncorrectable ECC error in the line
  output logic         rsp_ce,         // corrected errors
  // pins
  output logic [3:0]   cs_n,
  output logic [13:0]  ca,
  output logic [79:0]  dq_o,
  output logic         dq_oe,
  output logic [1:0]   dqs_o,
  input  logic [79:0]  dq_i,
  input  logic [1:0]   dqs_i,
  // statistics
  output logic [31:0]  n_ce,
  output logic [31:0]  n_ue,
  output logic [31:0]  n_strobe_err    // a read beat without its strobe
);

  typedef enum logic [4:0] {
    S_RESET, S_CSLOW, S_DES, S_NOPS, S_XPR, S_INITCMD, S_INITWAIT,
    S_IDLE, S_PREAB, S_REF, S_PRE, S_ACT, S_COL, S_RDATA, S_WDATA, S_DONE
  } st_e;
  st_e st;

  logic [31:0] now;
  logic [31:0] wcnt;                 // wait counter for init steps and tRP/tRCD/tRFC
  logic [2:0]  istep;                // init command index
  logic [31:0] dll_at;               // DLL reset time (tDLLK)

  // command issue: 2-cycle command second half pending
  logic        second;
  logic [13:0] ca2;

  // multicycle MPC phases
  logic [3:0]  mpc_t;

  // bank state per chip select (4) x bank (32: BG*4 + BA)
  logic        b_open [4][32];
  logic [15:0] b_row  [4][32];
  logic [31:0] b_act  [4][32];       // ACTIVATE time
  logic [31:0] b_rdy  [4][32];       // earliest PRECHARGE (tRAS, tRTP, tWR)
  logic [31:0] b_pre  [4][32];       // PRECHARGE time
  logic [31:0] last_act;
  logic [31:0] last_pre;             // the last PRECHARGE (tRP before REFab)
  logic [3:0]  gate;                 // read: clocks the capture window has been open
  logic [2:0]  last_act_bg;
  logic [31:0] faw [4];              // the last four ACTIVATE times
  logic [31:0] last_col;
  logic [2:0]  last_col_bg;
  logic        last_col_wr;
  logic [31:0] refi;

  // the request being served
  logic        r_we;
  logic [1:0]  r_cs;
  logic [4:0]  r_bank;
  logic [2:0]  r_bg;
  logic [1:0]  r_ba;
  logic [15:0] r_row;
  logic [9:0]  r_col;
  logic [511:0] r_data;
  logic [63:0] r_be;
  logic        r_ue, r_ce;
  logic [3:0]  beat;                 // data clock 0..7
  logic        rmw_read;             // the read half of a read-modify-write

  function automatic logic [31:0] scale(input int t);
    return fast ? ((32'(t) >> 10) + 32'd1) : 32'(t);
  endfunction

  // the chip select (DIMM*2 + rank) of an offset's bits [36:35]
  function automatic logic [1:0] cs_of(input logic [1:0] hi);
    return two_rank ? hi : {hi[0], 1'b0};
  endfunction

  logic [3:0] all_cs;
  assign all_cs = pop;

  // timing checks for the request's bank
  logic [31:0] t_act_ok, t_col_ok, t_pre_ok;
  always_comb begin
    logic [31:0] t;
    // ACTIVATE: tRP after PRE, tRC after the last ACT of the bank, tRRD, tFAW
    t = b_pre[r_cs][r_bank] + 32'(T_RP);
    if (b_act[r_cs][r_bank] + 32'(T_RC) > t) t = b_act[r_cs][r_bank] + 32'(T_RC);
    if (last_act + 32'(last_act_bg == r_bg ? T_RRD_L : T_RRD_S) > t)
      t = last_act + 32'(last_act_bg == r_bg ? T_RRD_L : T_RRD_S);
    if (faw[3] + 32'(T_FAW) > t) t = faw[3] + 32'(T_FAW);
    t_act_ok = t;
    // READ/WRITE: tRCD after ACT, tCCD after the last column command, tWTR
    t = b_act[r_cs][r_bank] + 32'(T_RCD);
    if (last_col + 32'(last_col_bg == r_bg ? T_CCD_L : T_CCD_S) > t)
      t = last_col + 32'(last_col_bg == r_bg ? T_CCD_L : T_CCD_S);
    if (last_col_wr && (!r_we || rmw_read))
      if (last_col + 32'(T_CWL + T_BURST + (last_col_bg == r_bg ? T_WTR_L : T_WTR_S)) > t)
        t = last_col + 32'(T_CWL + T_BURST + (last_col_bg == r_bg ? T_WTR_L : T_WTR_S));
    if (last_col_wr && r_we && !rmw_read && last_col_bg == r_bg)
      if (last_col + 32'(T_CCD_L_WR) > t) t = last_col + 32'(T_CCD_L_WR);
    t_col_ok = t;
    t_pre_ok = b_rdy[r_cs][r_bank];
  end

  // ECC of one 32-bit beat into the 40-bit DQ+CB word
  function automatic logic [39:0] enc(input logic [31:0] d);
    return {1'b0, ecc_check(d), d};
  endfunction

  assign req_ready = (st == S_IDLE) && ready && !(refi >= 32'(T_REFI));

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      st <= S_RESET;
      now <= '0;
      wcnt <= '0;
      istep <= '0;
      ready <= 1'b0;
      cs_n <= 4'h0;              // CS_n LOW while RESET_n is LOW (tINIT2 before release)
      ca <= CA_NOP;
      dq_o <= '0;
      dq_oe <= 1'b0;
      dqs_o <= 2'b00;
      second <= 1'b0;
      ca2 <= '0;
      mpc_t <= '0;
      rsp_valid <= 1'b0;
      last_act <= '0;
      last_pre <= '0;
      gate <= '0;
      r_data <= '0;
      r_ue <= 1'b0;
      r_ce <= 1'b0;
      last_act_bg <= '0;
      last_col <= '0;
      last_col_bg <= '0;
      last_col_wr <= 1'b0;
      refi <= '0;
      dll_at <= '0;
      n_ce <= '0;
      n_ue <= '0;
      n_strobe_err <= '0;
      rmw_read <= 1'b0;
      beat <= '0;
      for (int i = 0; i < 4; i++) faw[i] <= '0;
      for (int c = 0; c < 4; c++)
        for (int b = 0; b < 32; b++) begin
          b_open[c][b] <= 1'b0;
          b_row[c][b] <= '0;
          b_act[c][b] <= '0;
          b_rdy[c][b] <= '0;
          b_pre[c][b] <= '0;
        end
    end else begin
      now <= now + 32'd1;
      rsp_valid <= 1'b0;
      if (ready && st != S_REF) refi <= refi + 32'd1;
      // the default: deselect (DES); a pending second half goes out now (CS_n
      // HIGH on a 2-cycle command's second clock); states below override
      cs_n <= 4'hF;
      if (second) begin
        ca <= ca2;
        second <= 1'b0;
      end

      if (in_reset && st != S_RESET) begin
        // the channel's RESET_n went LOW again (a warm reset): start over
        ready <= 1'b0;
        dq_oe <= 1'b0;
        dqs_o <= 2'b00;
        second <= 1'b0;
        for (int c = 0; c < 4; c++)
          for (int b = 0; b < 32; b++) b_open[c][b] <= 1'b0;
        st <= S_RESET;
      end else
      case (st)
        // ---------------------------------------------------------- init
        S_RESET: begin
          cs_n <= 4'h0;
          if (start) begin
            wcnt <= scale(T_INIT3);
            st <= S_CSLOW;
          end
        end
        S_CSLOW: begin                 // tINIT3: CS_n LOW after RESET_n HIGH
          cs_n <= 4'h0;
          if (wcnt == 0) begin
            cs_n <= 4'hF;
            wcnt <= scale(T_INIT4);
            st <= S_DES;
          end else wcnt <= wcnt - 32'd1;
        end
        S_DES: begin                   // tINIT4: CS_n HIGH, ODT to the strap state
          if (wcnt == 0) begin
            wcnt <= 32'(T_INIT5);
            st <= S_NOPS;
          end else wcnt <= wcnt - 32'd1;
        end
        S_NOPS: begin                  // tINIT5 NOP commands
          cs_n <= ~all_cs;
          ca <= CA_NOP;
          if (wcnt == 1) begin
            wcnt <= scale(T_XPR);
            st <= S_XPR;
          end else wcnt <= wcnt - 32'd1;
        end
        S_XPR: begin
          if (wcnt == 0) begin
            istep <= '0;
            mpc_t <= '0;
            st <= S_INITCMD;
          end else wcnt <= wcnt - 32'd1;
        end
        S_INITCMD: begin
          if (istep == 3'd5) begin
            // MRW MR0: BL16, CL40 (Table 21); a 2-cycle command
            logic [27:0] c;
            c = cmd_mrw(8'h00, MR0_BL16_CL40);
            cs_n <= ~all_cs;
            ca <= c[13:0];
            ca2 <= c[27:14];
            second <= 1'b1;
            wcnt <= 32'(T_MRD);
            st <= S_INITWAIT;
          end else begin
            // a multicycle MPC: CA setup 3, CS_n LOW 4, CA hold 3 (Figure 11)
            logic [7:0] op;
            case (istep)
              3'd0: op = MPC_SET_1N;      // 1N command timing (2N is the reset default)
              3'd1: op = MPC_TDLLK_CCDL;
              3'd2: op = MPC_DLL_RESET;
              3'd3: op = MPC_ZQCAL_START;
              default: op = MPC_ZQCAL_LATCH;
            endcase
            ca <= cmd_mpc(op);
            if (mpc_t >= 4'(T_MPC_SETUP) && mpc_t < 4'(T_MPC_SETUP + T_MPC_CS)) cs_n <= ~all_cs;
            else cs_n <= 4'hF;
            if (mpc_t == 4'(T_MPC_SETUP + T_MPC_CS + T_MPC_HOLD - 1)) begin
              mpc_t <= '0;
              if (istep == 3'd2) dll_at <= now;
              wcnt <= (istep == 3'd3) ? scale(T_ZQCAL) : (istep == 3'd4) ? 32'(T_ZQLAT) : 32'(T_MPC);
              st <= S_INITWAIT;
            end else mpc_t <= mpc_t + 4'd1;
          end
        end
        S_INITWAIT: begin
          if (wcnt == 0) begin
            if (istep == 3'd5) begin
              // the DLL locks tDLLK after its reset; then normal operation
              if (now - dll_at >= scale(T_DLLK)) begin
                ready <= 1'b1;
                refi <= '0;
                st <= S_IDLE;
              end
            end else begin
              istep <= istep + 3'd1;
              st <= S_INITCMD;
            end
          end else wcnt <= wcnt - 32'd1;
        end

        // ---------------------------------------------------------- normal
        S_IDLE: begin
          if (refi >= 32'(T_REFI)) begin
            // refresh: close every bank (PREab) then REFab to all ranks
            logic anyopen;
            anyopen = 1'b0;
            for (int c = 0; c < 4; c++)
              for (int b = 0; b < 32; b++)
                if (b_open[c][b]) anyopen = 1'b1;
            st <= anyopen ? S_PREAB : S_REF;
            wcnt <= '0;
          end else if (req_valid) begin
            r_cs <= cs_of(req_off[36:35]);
            r_bg <= req_off[16:14];
            r_ba <= req_off[18:17];
            r_bank <= {req_off[16:14], req_off[18:17]};
            r_row <= req_off[34:19];
            r_col <= {req_off[13:8], 4'b0000};
            r_we <= req_we;
            rmw_read <= req_we && (req_be != '1);
            r_data <= req_wdata;
            r_be <= req_be;
            r_ue <= 1'b0;
            r_ce <= 1'b0;
            st <= S_PRE;
          end
        end
        S_PREAB: begin
          // wait until every open bank may be precharged, then PREab
          logic okall;
          okall = 1'b1;
          for (int c = 0; c < 4; c++)
            for (int b = 0; b < 32; b++)
              if (b_open[c][b] && now < b_rdy[c][b]) okall = 1'b0;
          if (okall && !second) begin
            cs_n <= ~all_cs;
            ca <= cmd_pre_ab();
            for (int c = 0; c < 4; c++)
              for (int b = 0; b < 32; b++)
                if (b_open[c][b]) begin
                  b_open[c][b] <= 1'b0;
                  b_pre[c][b] <= now;
                end
            last_pre <= now;
            st <= S_REF;
          end
        end
        S_REF: begin
          if (now >= last_pre + 32'(T_RP) && !second) begin
            cs_n <= ~all_cs;
            ca <= cmd_ref_ab();
            refi <= refi - 32'(T_REFI);
            wcnt <= 32'(T_RFC1);
            st <= S_DONE;            // S_DONE waits wcnt then returns to S_IDLE
          end
        end
        S_PRE: begin
          // the bank: open with the right row -> column command; open with
          // another -> PRECHARGE; closed -> ACTIVATE
          if (b_open[r_cs][r_bank] && b_row[r_cs][r_bank] == r_row) st <= S_COL;
          else if (!b_open[r_cs][r_bank]) st <= S_ACT;
          else if (now >= t_pre_ok && !second) begin
            cs_n <= ~(4'(1) << r_cs);
            ca <= cmd_pre_pb(r_bg, r_ba);
            b_open[r_cs][r_bank] <= 1'b0;
            b_pre[r_cs][r_bank] <= now;
            last_pre <= now;
            st <= S_ACT;
          end
        end
        S_ACT: begin
          if (now >= t_act_ok && !second) begin
            logic [27:0] c;
            c = cmd_act(r_row, r_bg, r_ba);
            cs_n <= ~(4'(1) << r_cs);
            ca <= c[13:0];
            ca2 <= c[27:14];
            second <= 1'b1;
            b_open[r_cs][r_bank] <= 1'b1;
            b_row[r_cs][r_bank] <= r_row;
            b_act[r_cs][r_bank] <= now;
            b_rdy[r_cs][r_bank] <= now + 32'(T_RAS);
            last_act <= now;
            last_act_bg <= r_bg;
            faw[0] <= now;
            faw[1] <= faw[0];
            faw[2] <= faw[1];
            faw[3] <= faw[2];
            st <= S_COL;
          end
        end
        S_COL: begin
          if (now >= t_col_ok && !second) begin
            logic [27:0] c;
            logic wr;
            wr = r_we && !rmw_read;
            c = wr ? cmd_wr(r_col, r_bg, r_ba, 1'b0) : cmd_rd(r_col, r_bg, r_ba, 1'b0);
            cs_n <= ~(4'(1) << r_cs);
            ca <= c[13:0];
            ca2 <= c[27:14];
            second <= 1'b1;
            last_col <= now;
            last_col_bg <= r_bg;
            last_col_wr <= wr;
            if (wr) begin
              // PRECHARGE after the burst plus tWR
              if (now + 32'(T_CWL + T_BURST + T_WR) > b_rdy[r_cs][r_bank])
                b_rdy[r_cs][r_bank] <= now + 32'(T_CWL + T_BURST + T_WR);
              // data from CWL after the command's final cycle (the datasheet's
              // reference: "from final cycle of the WRITE command")
              wcnt <= 32'(T_CWL);
              st <= S_WDATA;
            end else begin
              if (now + 32'(T_RTP) > b_rdy[r_cs][r_bank])
                b_rdy[r_cs][r_bank] <= now + 32'(T_RTP);
              wcnt <= 32'(T_CL - 1);      // the window opens a clock before data
              gate <= '0;
              st <= S_RDATA;
            end
            beat <= '0;
          end
        end
        S_RDATA: begin
          // data from T + CL for 8 clocks, two beats each; the capture window
          // opens a clock early and takes beats when the DQS strobe arrives
          // (as a PHY's read gate after read-leveling); no strobe within 4
          // clocks of the window is an error (the line reads as all ones)
          if (wcnt != 0) wcnt <= wcnt - 32'd1;
          else if (beat == 0 && dqs_i == 2'b00 && gate != 4'd4) gate <= gate + 4'd1;
          else begin
            for (int h = 0; h < 2; h++) begin
              logic [33:0] dc;
              logic [5:0] k;
              k = 6'({beat, 1'b0}) + 6'(h);
              if (!dqs_i[h]) begin
                n_strobe_err <= n_strobe_err + 32'd1;
                r_ue <= 1'b1;
              end
              dc = ecc_correct(dq_i[40 * h +: 32], dq_i[40 * h + 32 +: 7]);
              if (dc[33]) begin
                r_ue <= 1'b1;
                n_ue <= n_ue + 32'd1;
              end else if (dc[32]) begin
                r_ce <= 1'b1;
                n_ce <= n_ce + 32'd1;
              end
              // merge: a read-modify-write keeps the new bytes
              for (int y = 0; y < 4; y++)
                if (!(rmw_read && r_be[4 * k + 6'(y)]))
                  r_data[32 * k + 8 * y +: 8] <= dc[8 * y +: 8];
            end
            if (beat == 4'(T_BURST - 1)) begin
              if (rmw_read) begin
                rmw_read <= 1'b0;      // now write the merged line back
                st <= S_COL;
              end else begin
                rsp_valid <= 1'b1;
                st <= S_DONE;
                wcnt <= '0;
              end
            end
            beat <= beat + 4'd1;
          end
        end
        S_WDATA: begin
          if (wcnt != 0) wcnt <= wcnt - 32'd1;
          else begin
            for (int h = 0; h < 2; h++)
              dq_o[40 * h +: 40] <= enc(r_data[32 * {beat[2:0], 1'(h)} +: 32]);
            dq_oe <= 1'b1;
            dqs_o <= 2'b11;
            if (beat == 4'(T_BURST)) begin
              dq_oe <= 1'b0;
              dqs_o <= 2'b00;
              rsp_valid <= 1'b1;
              st <= S_DONE;
              wcnt <= '0;
            end
            beat <= beat + 4'd1;
          end
        end
        S_DONE: begin
          if (wcnt != 0) wcnt <= wcnt - 32'd1;
          else st <= S_IDLE;
        end
        default: st <= S_IDLE;
      endcase
    end
  end

  always_comb begin
    rsp_rdata = r_data;
    rsp_ue = r_ue;
    rsp_ce = r_ce;
  end

endmodule
