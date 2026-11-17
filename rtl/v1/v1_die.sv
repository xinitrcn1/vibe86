// v1_die: the v1 die — the v1 core and the uncore (rtl/uncore): the address
// fabric, the memory init engine, four DDR5 subchannel controllers (channels
// 0 and 1 of eight) and the I/O link endpoint. Its ports are the die
// interface (rtl/common/pins.toml, docs/die-interface.md, D-014) and the
// package's balls (docs/package.md, D-019). The core leaves reset when DRAM is ready.
module v1_die
  import apic_pkg::*;
(
`include "vibe86_die_ports.svh"
);
  // the die's cores (D-017: every interface below is per core; the second
  // core needs the fabric's N-master arbitration and LOCKed accesses, see
  // docs/decisions.md D-017)
  localparam int NCORE = 1;

  // the reserved balls (D-019): DDR5 channels 2-7, SPD bus 1, the
  // inter-socket links -- outputs idle, inputs unused
`include "vibe86_die_reserved.svh"

  // ------------------------------------------------------------ memory init
  logic        dram_ready, two_ch;
  logic [40:0] dram_size;
  logic        sub_in_reset, sub_start, sub_two_rank;
  logic [3:0]  sub_pop, sub_ready;
  logic        post_valid, post_ack;
  logic [7:0]  post_code;

  meminit u_meminit (
    .clk, .rst_n, .fast(meminit_fast),
    .scl_o(spd0_scl_o), .sda_o(spd0_sda_o), .scl_i(spd0_scl_i), .sda_i(spd0_sda_i),
    .ddr_reset_n({ddr1_reset_n, ddr0_reset_n}),
    .sub_in_reset, .sub_start, .sub_pop, .sub_two_rank, .sub_ready,
    .dram_ready, .two_ch, .dram_size,
    .post_valid, .post_code, .post_ack);

  // ------------------------------------------------------------ the core
  logic         core_rst_n;
  assign core_rst_n = rst_n && dram_ready;

  logic         c_req, c_we, c_io, c_smm, c_ack, c_err;
  logic [63:0]  c_addr;
  logic [2:0]   c_size;
  logic [127:0] c_wdata;
  logic [511:0] c_rdata;
  logic         c_ic_inv, c_ic_inv_core;   // the fabric's write snoop (D-018)
  logic [51:6]  c_ic_inv_line;
  logic         f_mce_ue;
  logic [63:0]  f_mce_addr;

  // ------------------------------------------------------------ JTAG
  // the TAP (TCK domain) and the debug module (core clock): docs/jtag.md
  logic         dbg_req, dbg_step, dbg_halted, dbg_reg_we;
  logic [7:0]   dbg_reg_sel;
  logic [63:0]  dbg_reg_wdata, dbg_reg_rdata;
  logic         d_req, d_we, d_ack, d_err;
  logic [63:0]  d_addr, d_wdata, d_rdata;
  logic [2:0]   d_size;
  logic         j_req_tog, j_ack_tog, j_req_we, j_halted, j_haltreq, j_sba_err, j_reg_err;
  logic [2:0]   j_req_op, j_req_size;
  logic [3:0]   j_req_ctl;
  logic [7:0]   j_req_sel;
  logic [63:0]  j_req_data, j_req_addr, j_reg_rdata, j_sba_rdata;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [3:0]   tap_state;          // for testbenches
  wire alert_unused = ddr0_alert_n & ddr1_alert_n;   // CRC/parity alerts: no CRC in use
  /* verilator lint_on UNUSEDSIGNAL */

  jtag_tap u_tap (
    .tck, .tms, .tdi, .trst_n, .tdo,
    .req_tog(j_req_tog), .req_op(j_req_op), .req_ctl(j_req_ctl), .req_we(j_req_we),
    .req_sel(j_req_sel), .req_data(j_req_data), .req_addr(j_req_addr), .req_size(j_req_size),
    .ack_tog(j_ack_tog), .reg_rdata(j_reg_rdata), .sba_rdata(j_sba_rdata),
    .st_halted(j_halted), .st_haltreq(j_haltreq), .st_sba_err(j_sba_err), .st_reg_err(j_reg_err),
    .tap_state);

  jtag_dm u_dm (
    .clk, .trst_n,
    .req_tog(j_req_tog), .req_op(j_req_op), .req_ctl(j_req_ctl), .req_we(j_req_we),
    .req_sel(j_req_sel), .req_data(j_req_data), .req_addr(j_req_addr), .req_size(j_req_size),
    .ack_tog(j_ack_tog), .reg_rdata(j_reg_rdata), .sba_rdata(j_sba_rdata),
    .st_halted(j_halted), .st_haltreq(j_haltreq), .st_sba_err(j_sba_err), .st_reg_err(j_reg_err),
    .dbg_req, .dbg_step, .dbg_halted, .dbg_reg_sel, .dbg_reg_we, .dbg_reg_wdata, .dbg_reg_rdata,
    .dm_req(d_req), .dm_we(d_we), .dm_addr(d_addr), .dm_size(d_size), .dm_wdata(d_wdata),
    .dm_ack(d_ack), .dm_err(d_err), .dm_rdata(d_rdata));

  v1_core u_core (
    .clk, .rst_n(core_rst_n),
    .bus_req(c_req), .bus_we(c_we), .bus_io(c_io), .bus_addr(c_addr), .bus_size(c_size),
    .bus_wdata(c_wdata), .bus_smm(c_smm), .bus_ack(c_ack), .bus_err(c_err), .bus_rdata(c_rdata),
    .ic_inv(c_ic_inv), .ic_inv_line(c_ic_inv_line), .ic_inv_core(c_ic_inv_core),
    .intr_valid(c_intr[0]), .intr_vector(c_vec[0]), .intr_ack(c_iack[0]),
    // with the APIC globally disabled (IA32_APIC_BASE.EN = 0) the NMI pin
    // reaches the core directly (Intel SDM Vol. 3A 12.4.3)
    .nmi(c_nmi[0] || (nmi && !c_apic_base[0][11])), .smi(smi || c_smi[0]),
    .is_bsp(1'b1), .apic_id(32'd0), .init(c_init[0]), .sipi(c_sipi[0]), .sipi_vec(c_sipi_vec[0]),
    .apic_base(c_apic_base[0]), .tsc(c_tsc[0]),
    .lapic_msr_addr(c_msr_addr[0]), .lapic_msr_wr(c_msr_wr[0]), .lapic_msr_wdata(c_msr_wdata[0]),
    .lapic_msr_rdata(c_msr_rdata[0]), .lapic_msr_rok(c_msr_rok[0]), .lapic_msr_wok(c_msr_wok[0]),
    .cr8_wr(c_cr8_wr[0]), .cr8_wdata(c_cr8_wdata[0]), .cr8_rdata(c_cr8_rdata[0]),
    .mce_valid(mce_valid || f_mce_ue), .mce_type(f_mce_ue ? 2'd2 : mce_type),
    .mce_addr(f_mce_ue ? f_mce_addr : mce_addr),
    .dbg_req, .dbg_step, .dbg_halted, .dbg_reg_sel, .dbg_reg_we, .dbg_reg_wdata, .dbg_reg_rdata,
    .rt_valid, .rt_rip, .rt_len, .rt_bytes, .rt_uops,
    .ev_valid, .ev_vector, .ev_has_error, .ev_error, .ev_rip,
    .shutdown, .halted, .shutdown_rip);

  // ------------------------------------------------------------ local APICs
  // one per core (APIC ID = core index) on the die's APIC bus, which also
  // carries the MSIs the fabric receives over the I/O link; the 8259's INTR
  // is every APIC's LINT0 (virtual wire through ExtINT, MP spec 1.4 3.6.2.2)
  // and the NMI pin their LINT1
  logic         c_intr [NCORE], c_iack [NCORE], c_nmi [NCORE], c_smi [NCORE];
  logic         c_init [NCORE], c_sipi [NCORE];
  logic [7:0]   c_vec [NCORE], c_sipi_vec [NCORE];
  logic [63:0]  c_apic_base [NCORE], c_tsc [NCORE];
  logic [31:0]  c_msr_addr [NCORE];
  logic         c_msr_wr [NCORE], c_msr_rok [NCORE], c_msr_wok [NCORE];
  logic [63:0]  c_msr_wdata [NCORE], c_msr_rdata [NCORE];
  logic         c_cr8_wr [NCORE];
  logic [3:0]   c_cr8_wdata [NCORE], c_cr8_rdata [NCORE];
  logic         lint0_ack [NCORE];
  logic [11:4]  la_off;
  logic [31:0]  la_rdata [NCORE];
  logic         la_wr;
  logic [31:0]  la_wdata;
  logic         ab_src_valid [NCORE], ab_src_ready [NCORE], ab_match [NCORE], ab_accept [NCORE];
  apic_msg_t    ab_src_msg [NCORE];
  logic [7:0]   ab_apr [NCORE];
  logic         ab_valid, msi_valid, msi_ready;
  apic_msg_t    ab_msg, msi_msg;
  logic         eoi_valid [NCORE], eoi_ack;
  logic [7:0]   eoi_vec [NCORE];

  for (genvar k = 0; k < NCORE; k++) begin : g_apic
    lapic #(.APIC_ID(32'(k))) u_lapic (
      .clk, .rst_n(core_rst_n),
      .apic_en(c_apic_base[k][11]), .apic_extd(c_apic_base[k][10]), .tsc(c_tsc[k]),
      .mmio_off(la_off), .mmio_rdata(la_rdata[k]), .mmio_wr(la_wr && k == 0), .mmio_wdata(la_wdata),
      .msr_addr(c_msr_addr[k]), .msr_rdata(c_msr_rdata[k]), .msr_rok(c_msr_rok[k]),
      .msr_wr(c_msr_wr[k]), .msr_wdata(c_msr_wdata[k]), .msr_wok(c_msr_wok[k]),
      .cr8_wr(c_cr8_wr[k]), .cr8_wdata(c_cr8_wdata[k]), .cr8_rdata(c_cr8_rdata[k]),
      .lint0(intr_valid), .lint0_vec(intr_vector), .lint0_ack(lint0_ack[k]), .lint1(nmi),
      .core_intr(c_intr[k]), .core_vec(c_vec[k]), .core_ack(c_iack[k]),
      .core_nmi(c_nmi[k]), .core_smi(c_smi[k]), .core_init(c_init[k]),
      .core_sipi(c_sipi[k]), .sipi_vec(c_sipi_vec[k]),
      .out_valid(ab_src_valid[k]), .out_msg(ab_src_msg[k]), .out_ready(ab_src_ready[k]),
      .in_valid(ab_valid), .in_msg(ab_msg), .in_match(ab_match[k]), .arb_pri(ab_apr[k]),
      .in_accept(ab_accept[k]),
      .eoi_valid(eoi_valid[k]), .eoi_vec(eoi_vec[k]), .eoi_ack(eoi_ack && k == 0));
  end

  apic_bus #(.N(NCORE)) u_apic_bus (
    .clk, .rst_n(core_rst_n),
    .src_valid(ab_src_valid), .src_msg(ab_src_msg), .src_ready(ab_src_ready),
    .msi_valid, .msi_msg, .msi_ready,
    .bus_valid(ab_valid), .bus_msg(ab_msg), .match(ab_match), .apr(ab_apr), .accept(ab_accept));

  // INTA to the 8259: the core that took an ExtINT
  always_comb begin
    intr_ack = 1'b0;
    for (int k = 0; k < NCORE; k++) intr_ack |= lint0_ack[k];
  end

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
    .clk, .rst_n, .dram_ready, .dram_size, .two_ch,
    .c_req, .c_we, .c_io, .c_addr, .c_size, .c_wdata, .c_smm, .c_ack, .c_err, .c_rdata,
    .ic_inv(c_ic_inv), .ic_inv_line(c_ic_inv_line), .ic_inv_core(c_ic_inv_core),
    .d_req, .d_we, .d_addr, .d_size, .d_wdata, .d_ack, .d_err, .d_rdata,
    .p_req(post_valid), .p_code(post_code), .p_ack(post_ack),
    .apic_base(c_apic_base[0][51:10]), .la_off, .la_rdata(la_rdata[0]), .la_wr, .la_wdata,
    .msi_valid, .msi_msg, .msi_ready, .eoi_valid(eoi_valid[0]), .eoi_vec(eoi_vec[0]), .eoi_ack,
    .m_valid, .m_ready, .m_we, .m_off, .m_wdata, .m_be, .m_rsp, .m_rdata, .m_ue,
    .tx_valid(iol_tx_valid), .tx_flit(iol_tx_flit), .tx_ready(iol_tx_ready),
    .rx_valid(iol_rx_valid), .rx_flit(iol_rx_flit), .rx_ready(iol_rx_ready),
    .mce_ue(f_mce_ue), .mce_addr(f_mce_addr));

endmodule
