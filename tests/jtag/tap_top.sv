// tap_top: jtag_tap + jtag_dm with a stub core (halts as soon as asked;
// register n reads as n) and fabric (acknowledges at once; reads return the
// address XOR 5A5A...), for tap_harness.cpp: the remote_bitbang server
// against the real TAP before the die top exists.
module tap_top (
  input  logic clk,
  input  logic tck,
  input  logic tms,
  input  logic tdi,
  input  logic trst_n,
  output logic tdo
);
  wire        req_tog, req_we, ack_tog;
  wire [2:0]  req_op, req_size;
  wire [3:0]  req_ctl;
  wire [7:0]  req_sel;
  wire [63:0] req_data, req_addr, reg_rdata, sba_rdata;
  wire        st_halted, st_haltreq, st_sba_err, st_reg_err;
  wire        dbg_req, dbg_step, dbg_reg_we;
  wire [7:0]  dbg_reg_sel;
  wire [63:0] dbg_reg_wdata;
  wire        dm_req, dm_we;
  wire [63:0] dm_addr, dm_wdata;
  wire [2:0]  dm_size;
  wire [3:0]  tap_state;
  logic       dm_ack;
  always_ff @(posedge clk) dm_ack <= dm_req && !dm_ack;
  jtag_tap tap (.tck, .tms, .tdi, .trst_n, .tdo, .req_tog, .req_op, .req_ctl, .req_we,
                .req_sel, .req_data, .req_addr, .req_size, .ack_tog, .reg_rdata, .sba_rdata,
                .st_halted, .st_haltreq, .st_sba_err, .st_reg_err, .tap_state);
  jtag_dm dm (.clk, .trst_n, .req_tog, .req_op, .req_ctl, .req_we, .req_sel, .req_data,
              .req_addr, .req_size, .ack_tog, .reg_rdata, .sba_rdata, .st_halted, .st_haltreq,
              .st_sba_err, .st_reg_err, .dbg_req, .dbg_step, .dbg_halted(dbg_req),
              .dbg_reg_sel, .dbg_reg_we, .dbg_reg_wdata, .dbg_reg_rdata({56'b0, dbg_reg_sel}),
              .dm_req, .dm_we, .dm_addr, .dm_size, .dm_wdata, .dm_ack, .dm_err(1'b0),
              .dm_rdata(dm_addr ^ 64'h5A5A_5A5A_5A5A_5A5A));
  /* verilator lint_off UNUSEDSIGNAL */
  wire unused = ^{dbg_step, dbg_reg_we, dbg_reg_wdata, dm_we, dm_wdata, dm_size, tap_state};
  /* verilator lint_on UNUSEDSIGNAL */
endmodule
