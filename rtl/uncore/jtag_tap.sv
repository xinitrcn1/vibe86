// jtag_tap: the die's JTAG test access port (IEEE Std 1149.1-2013), in the
// TCK domain, and the TCK side of the debug module's request handshake.
//
// The TAP controller is the standard 16-state machine (1149.1 clause 6.1,
// Figure 6-1) on the rising edge of TCK, reset asynchronously by TRST_n and
// synchronously by five TCK cycles with TMS high. TDO changes on the falling
// edge of TCK (clause 4.5) and is 0 outside Shift-IR/Shift-DR. Instruction
// and data registers are captured and shifted on the rising edge; their
// parallel outputs are updated on the falling edge in Update-IR/Update-DR
// (clause 7.2, 9.2).
//
// The instruction register is 5 bits; Capture-IR loads 00001b (the two
// least significant bits 01b, clause 7.1.1). Instructions (docs/jtag.md):
//   00001 IDCODE   32 bits, selected by Test-Logic-Reset (clause 8.13)
//   10000 DBGCTL   32 bits: halt/resume/step, status
//   10001 DBGREG   73 bits: core register access {we, sel[7:0], data[63:0]}
//   10010 SBAADDR  67 bits: system bus address and size {size[2:0], addr[63:0]}
//   10011 SBADATA  66 bits: system bus data and command/status {cmd/status[1:0], data[63:0]}
//   11111 BYPASS   1 bit (also every unassigned code, clause 8.4)
//
// Debug requests (an Update-DR of DBGCTL, DBGREG, or SBADATA with a command)
// cross into the core clock domain through a toggle handshake (the
// request/acknowledge "MCP formulation" of C. E. Cummings, "Clock Domain
// Crossing (CDC) Design & Verification Techniques Using SystemVerilog", SNUG
// Boston 2008): the payload registers below are stable from the request
// toggle until the acknowledge toggle comes back, so jtag_dm samples them
// without synchronizers; status bits from the core domain pass through two
// flip-flops on TCK. A request made while one is pending is dropped and
// reported in DBGCTL (sticky 'dropped').
module jtag_tap
  import vibe86_platform_pkg::*;
(
  input  logic        tck,
  input  logic        tms,
  input  logic        tdi,
  input  logic        trst_n,
  output logic        tdo,
  // to jtag_dm (core domain): request toggle and its payload
  output logic        req_tog,
  output logic [2:0]  req_op,       // 1 CTL, 2 REG, 3 SBA read, 4 SBA write
  output logic [3:0]  req_ctl,      // DBGCTL: [0] haltreq, [1] resume, [2] step, [3] clear errors
  output logic        req_we,       // DBGREG: write
  output logic [7:0]  req_sel,
  output logic [63:0] req_data,     // DBGREG / SBA write data
  output logic [63:0] req_addr,     // SBA address
  output logic [2:0]  req_size,     // SBA log2 bytes, 0..3
  // from jtag_dm: acknowledge toggle, results (stable when not busy), status
  input  logic        ack_tog,
  input  logic [63:0] reg_rdata,
  input  logic [63:0] sba_rdata,
  input  logic        st_halted,
  input  logic        st_haltreq,
  input  logic        st_sba_err,
  input  logic        st_reg_err,
  // for testbenches: the TAP state (1149.1 Table 6-3 names, encoding below)
  output logic [3:0]  tap_state
);
  localparam int IRW = 5;
  localparam logic [IRW-1:0] IR_IDCODE  = 5'b00001;
  localparam logic [IRW-1:0] IR_DBGCTL  = 5'b10000;
  localparam logic [IRW-1:0] IR_DBGREG  = 5'b10001;
  localparam logic [IRW-1:0] IR_SBAADDR = 5'b10010;
  localparam logic [IRW-1:0] IR_SBADATA = 5'b10011;
  localparam logic [IRW-1:0] IR_BYPASS  = 5'b11111;

  localparam logic [31:0] IDCODE = {PLAT_JTAG_IDCODE_VERSION[3:0], PLAT_JTAG_IDCODE_PART[15:0],
                                    PLAT_JTAG_IDCODE_MFR_BANK[3:0], PLAT_JTAG_IDCODE_MFR_CODE[6:0],
                                    1'b1};
  localparam logic [3:0] DBG_VERSION = 4'd1;

  typedef enum logic [3:0] {
    TLR = 4'h0, RTI = 4'h1,
    SEL_DR = 4'h2, CAP_DR = 4'h3, SH_DR = 4'h4, EX1_DR = 4'h5, PA_DR = 4'h6, EX2_DR = 4'h7, UPD_DR = 4'h8,
    SEL_IR = 4'h9, CAP_IR = 4'hA, SH_IR = 4'hB, EX1_IR = 4'hC, PA_IR = 4'hD, EX2_IR = 4'hE, UPD_IR = 4'hF
  } state_e;

  state_e st;
  assign tap_state = st;

  // the state transitions (1149.1 Figure 6-1)
  always_ff @(posedge tck or negedge trst_n) begin
    if (!trst_n) st <= TLR;
    else unique case (st)
      TLR:    st <= tms ? TLR : RTI;
      RTI:    st <= tms ? SEL_DR : RTI;
      SEL_DR: st <= tms ? SEL_IR : CAP_DR;
      CAP_DR: st <= tms ? EX1_DR : SH_DR;
      SH_DR:  st <= tms ? EX1_DR : SH_DR;
      EX1_DR: st <= tms ? UPD_DR : PA_DR;
      PA_DR:  st <= tms ? EX2_DR : PA_DR;
      EX2_DR: st <= tms ? UPD_DR : SH_DR;
      UPD_DR: st <= tms ? SEL_DR : RTI;
      SEL_IR: st <= tms ? TLR : CAP_IR;
      CAP_IR: st <= tms ? EX1_IR : SH_IR;
      SH_IR:  st <= tms ? EX1_IR : SH_IR;
      EX1_IR: st <= tms ? UPD_IR : PA_IR;
      PA_IR:  st <= tms ? EX2_IR : PA_IR;
      EX2_IR: st <= tms ? UPD_IR : SH_IR;
      UPD_IR: st <= tms ? SEL_DR : RTI;
    endcase
  end

  // ------------------------------------------------------------ instruction
  logic [IRW-1:0] ir_sr, ir;
  always_ff @(posedge tck or negedge trst_n) begin
    if (!trst_n) ir_sr <= IR_IDCODE;
    else if (st == CAP_IR) ir_sr <= 5'b00001;
    else if (st == SH_IR) ir_sr <= {tdi, ir_sr[IRW-1:1]};
  end
  // the instruction takes effect on the falling edge in Update-IR;
  // Test-Logic-Reset selects IDCODE (clause 7.2.1)
  always_ff @(negedge tck or negedge trst_n) begin
    if (!trst_n) ir <= IR_IDCODE;
    else if (st == TLR) ir <= IR_IDCODE;
    else if (st == UPD_IR) ir <= ir_sr;
  end

  // the selected data register's length; unassigned codes are BYPASS
  function automatic int unsigned dr_len(input logic [IRW-1:0] i);
    unique case (i)
      IR_IDCODE:  return 32;
      IR_DBGCTL:  return 32;
      IR_DBGREG:  return 73;
      IR_SBAADDR: return 67;
      IR_SBADATA: return 66;
      IR_BYPASS:  return 1;
      default:    return 1;
    endcase
  endfunction

  // ------------------------------------------------------------ status in
  // core-domain status: two flip-flops on TCK
  logic [3:0] st_s1, st_s2;
  logic       ack_s1, ack_s2;
  always_ff @(posedge tck or negedge trst_n) begin
    if (!trst_n) begin
      st_s1 <= '0;
      st_s2 <= '0;
      ack_s1 <= 1'b0;
      ack_s2 <= 1'b0;
    end else begin
      st_s1 <= {st_reg_err, st_sba_err, st_haltreq, st_halted};
      st_s2 <= st_s1;
      ack_s1 <= ack_tog;
      ack_s2 <= ack_s1;
    end
  end
  wire busy = req_tog != ack_s2;
  logic dropped;      // a request while one was pending (sticky; DBGCTL bit 3 clears)

  // ------------------------------------------------------------ data registers
  logic [72:0] dr;
  logic [72:0] cap;
  always_comb begin
    cap = '0;
    unique case (ir)
      IR_IDCODE:  cap[31:0] = IDCODE;
      IR_DBGCTL:  cap[31:0] = {DBG_VERSION, 22'b0, dropped, st_s2[3], st_s2[2], busy,
                               st_s2[1], st_s2[0]};
      IR_DBGREG:  cap = {1'b0, req_sel, reg_rdata};
      IR_SBAADDR: cap[66:0] = {req_size, req_addr};
      IR_SBADATA: cap[65:0] = {st_s2[2], busy, sba_rdata};
      default:    cap = '0;               // BYPASS captures 0 (clause 10.1.1)
    endcase
  end
  wire [6:0] len = 7'(dr_len(ir));
  always_ff @(posedge tck or negedge trst_n) begin
    if (!trst_n) dr <= '0;
    else if (st == CAP_DR) dr <= cap;
    else if (st == SH_DR) begin
      // shift toward bit 0 (TDO); TDI enters at the register's last bit
      for (int i = 0; i < 72; i++)
        dr[i] <= (7'(i) == len - 7'd1) ? tdi : dr[i + 1];
      dr[72] <= (len == 7'd73) ? tdi : 1'b0;
    end
  end

  // TDO on the falling edge (clause 4.5.1)
  always_ff @(negedge tck or negedge trst_n) begin
    if (!trst_n) tdo <= 1'b0;
    else if (st == SH_IR) tdo <= ir_sr[0];
    else if (st == SH_DR) tdo <= dr[0];
    else tdo <= 1'b0;
  end

  // ------------------------------------------------------------ update -> requests
  always_ff @(negedge tck or negedge trst_n) begin
    if (!trst_n) begin
      req_tog <= 1'b0;
      req_op <= '0;
      req_ctl <= '0;
      req_we <= 1'b0;
      req_sel <= '0;
      req_data <= '0;
      req_addr <= '0;
      req_size <= 3'd3;
      dropped <= 1'b0;
    end else if (st == UPD_DR) begin
      unique case (ir)
        IR_DBGCTL: begin
          if (dr[3]) dropped <= 1'b0;
          if (dr[3:0] != 4'b0) begin
            if (busy) dropped <= 1'b1;
            else begin
              req_op <= 3'd1;
              req_ctl <= dr[3:0];
              req_tog <= ~req_tog;
            end
          end
        end
        IR_DBGREG: begin
          if (busy) dropped <= 1'b1;
          else begin
            req_op <= 3'd2;
            req_we <= dr[72];
            req_sel <= dr[71:64];
            req_data <= dr[63:0];
            req_tog <= ~req_tog;
          end
        end
        IR_SBAADDR: begin
          if (busy) dropped <= 1'b1;
          else begin
            req_addr <= dr[63:0];
            req_size <= dr[66:64] > 3'd3 ? 3'd3 : dr[66:64];
          end
        end
        IR_SBADATA: begin
          if (dr[65:64] == 2'b01 || dr[65:64] == 2'b10) begin
            if (busy) dropped <= 1'b1;
            else begin
              req_op <= dr[65:64] == 2'b01 ? 3'd3 : 3'd4;
              req_data <= dr[63:0];
              req_tog <= ~req_tog;
            end
          end
        end
        default: ;
      endcase
    end
  end
endmodule
