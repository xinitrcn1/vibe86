// v1_die: the v1 die — the v1 core and the uncore (rtl/uncore): the address
// fabric, the memory init engine, four DDR5 subchannel controllers and the
// I/O link endpoint. Its ports are the die interface (rtl/common/pins.toml,
// docs/die-interface.md, D-014). The core leaves reset when DRAM is ready.
module v1_die (
`include "vibe86_die_ports.svh"
);

  // ------------------------------------------------------------ memory init
  logic        dram_ready;
  logic [40:0] dram_size;
  logic        sub_in_reset, sub_start, sub_two_rank;
  logic [3:0]  sub_pop, sub_ready;
  logic        post_valid, post_ack;
  logic [7:0]  post_code;

  meminit u_meminit (
    .clk, .rst_n, .fast(meminit_fast),
    .scl_o(spd_scl_o), .sda_o(spd_sda_o), .scl_i(spd_scl_i), .sda_i(spd_sda_i),
    .ddr_reset_n({ddr1_reset_n, ddr0_reset_n}),
    .sub_in_reset, .sub_start, .sub_pop, .sub_two_rank, .sub_ready,
    .dram_ready, .dram_size,
    .post_valid, .post_code, .post_ack);

  // ------------------------------------------------------------ the core
  logic         core_rst_n;
  assign core_rst_n = rst_n && dram_ready;

  logic         c_req, c_we, c_io, c_smm, c_ack, c_err;
  logic [63:0]  c_addr;
  logic [2:0]   c_size;
  logic [127:0] c_wdata, c_rdata;
  logic         f_mce_ue;
  logic [63:0]  f_mce_addr;

  // debug module: JTAG (rtl/uncore/jtag_tap.sv, jtag_dm.sv) is integrated
  // from the die-jtag branch; until then the TAP is absent (TDO low)
  logic         dbg_halted;
  logic [63:0]  dbg_reg_rdata;
  logic         d_ack, d_err;
  logic [63:0]  d_rdata;
  assign tdo = 1'b0;
  /* verilator lint_off UNUSEDSIGNAL */
  wire jtag_unused = &{tck, tms, tdi, trst_n, dbg_halted, dbg_reg_rdata, d_ack, d_err, d_rdata,
                       ddr0_alert_n, ddr1_alert_n};
  /* verilator lint_on UNUSEDSIGNAL */

  v1_core u_core (
    .clk, .rst_n(core_rst_n),
    .bus_req(c_req), .bus_we(c_we), .bus_io(c_io), .bus_addr(c_addr), .bus_size(c_size),
    .bus_wdata(c_wdata), .bus_smm(c_smm), .bus_ack(c_ack), .bus_err(c_err), .bus_rdata(c_rdata),
    .intr_valid, .intr_vector, .intr_ack, .nmi, .smi,
    .mce_valid(mce_valid || f_mce_ue), .mce_type(f_mce_ue ? 2'd2 : mce_type),
    .mce_addr(f_mce_ue ? f_mce_addr : mce_addr),
    .dbg_req(1'b0), .dbg_step(1'b0), .dbg_halted, .dbg_reg_sel(8'd0), .dbg_reg_we(1'b0),
    .dbg_reg_wdata(64'd0), .dbg_reg_rdata,
    .rt_valid, .rt_rip, .rt_len, .rt_bytes, .rt_uops,
    .ev_valid, .ev_vector, .ev_has_error, .ev_error, .ev_rip,
    .shutdown, .halted, .shutdown_rip);

  // ------------------------------------------------------------ DRAM
  logic [3:0]   m_valid, m_ready, m_rsp, m_ue, m_ce;
  logic         m_we;
  logic [36:8]  m_off;
  logic [511:0] m_wdata;
  logic [63:0]  m_be;
  logic [511:0] m_rdata [4];
  logic [3:0]   cs_n [4];
  logic [13:0]  ca [4];
  logic [79:0]  dq_o [4], dq_i [4];
  logic [3:0]   dq_oe;
  logic [1:0]   dqs_o [4], dqs_i [4];
  /* verilator lint_off UNUSEDSIGNAL */
  logic [31:0]  n_ce [4], n_ue [4], n_strobe [4];   // statistics (read through JTAG later)
  /* verilator lint_on UNUSEDSIGNAL */

  for (genvar k = 0; k < 4; k++) begin : g_sub
    ddr5_sub u_sub (
      .clk, .rst_n,
      .in_reset(sub_in_reset), .start(sub_start), .fast(meminit_fast),
      .pop(sub_pop), .two_rank(sub_two_rank), .ready(sub_ready[k]),
      .req_valid(m_valid[k]), .req_ready(m_ready[k]), .req_we(m_we), .req_off(m_off),
      .req_wdata(m_wdata), .req_be(m_be),
      .rsp_valid(m_rsp[k]), .rsp_rdata(m_rdata[k]), .rsp_ue(m_ue[k]), .rsp_ce(m_ce[k]),
      .cs_n(cs_n[k]), .ca(ca[k]), .dq_o(dq_o[k]), .dq_oe(dq_oe[k]), .dqs_o(dqs_o[k]),
      .dq_i(dq_i[k]), .dqs_i(dqs_i[k]),
      .n_ce(n_ce[k]), .n_ue(n_ue[k]), .n_strobe_err(n_strobe[k]));
  end
  /* verilator lint_off UNUSEDSIGNAL */
  wire ce_unused = |m_ce;               // corrected errors: counted per subchannel
  /* verilator lint_on UNUSEDSIGNAL */

  // the pins of subchannels 0a, 0b, 1a, 1b
  assign ddr0a_cs_n = cs_n[0];  assign ddr0b_cs_n = cs_n[1];
  assign ddr1a_cs_n = cs_n[2];  assign ddr1b_cs_n = cs_n[3];
  assign ddr0a_ca = ca[0];      assign ddr0b_ca = ca[1];
  assign ddr1a_ca = ca[2];      assign ddr1b_ca = ca[3];
  assign ddr0a_dq_o = dq_o[0];  assign ddr0b_dq_o = dq_o[1];
  assign ddr1a_dq_o = dq_o[2];  assign ddr1b_dq_o = dq_o[3];
  assign ddr0a_dq_oe = dq_oe[0]; assign ddr0b_dq_oe = dq_oe[1];
  assign ddr1a_dq_oe = dq_oe[2]; assign ddr1b_dq_oe = dq_oe[3];
  assign ddr0a_dqs_o = dqs_o[0]; assign ddr0b_dqs_o = dqs_o[1];
  assign ddr1a_dqs_o = dqs_o[2]; assign ddr1b_dqs_o = dqs_o[3];
  assign dq_i[0] = ddr0a_dq_i;  assign dq_i[1] = ddr0b_dq_i;
  assign dq_i[2] = ddr1a_dq_i;  assign dq_i[3] = ddr1b_dq_i;
  assign dqs_i[0] = ddr0a_dqs_i; assign dqs_i[1] = ddr0b_dqs_i;
  assign dqs_i[2] = ddr1a_dqs_i; assign dqs_i[3] = ddr1b_dqs_i;

  // ------------------------------------------------------------ fabric
  fabric u_fabric (
    .clk, .rst_n, .dram_ready, .dram_size,
    .c_req, .c_we, .c_io, .c_addr, .c_size, .c_wdata, .c_smm, .c_ack, .c_err, .c_rdata,
    .d_req(1'b0), .d_we(1'b0), .d_addr(64'd0), .d_size(3'd0), .d_wdata(64'd0),
    .d_ack, .d_err, .d_rdata,
    .p_req(post_valid), .p_code(post_code), .p_ack(post_ack),
    .m_valid, .m_ready, .m_we, .m_off, .m_wdata, .m_be, .m_rsp, .m_rdata, .m_ue,
    .tx_valid(iol_tx_valid), .tx_flit(iol_tx_flit), .tx_ready(iol_tx_ready),
    .rx_valid(iol_rx_valid), .rx_flit(iol_rx_flit), .rx_ready(iol_rx_ready),
    .mce_ue(f_mce_ue), .mce_addr(f_mce_addr));

endmodule
