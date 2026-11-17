// jtag_dm: the debug module, in the core clock domain. It carries out the
// requests jtag_tap makes from TCK: halting, resuming and single-stepping
// the core (the core's dbg_* ports), reading and writing its registers while
// halted, and system bus accesses (physical addresses, through the fabric's
// debug master port dm_*). docs/jtag.md.
//
// Clock-domain crossing (C. E. Cummings, "Clock Domain Crossing (CDC) Design
// & Verification Techniques Using SystemVerilog", SNUG Boston 2008, 5.8.2
// "MCP formulation ... with acknowledge feedback"): only the request toggle
// passes through a two-flip-flop synchronizer here; the payload (req_op,
// req_* ) is held stable by jtag_tap from the toggle until the acknowledge
// toggle returns, so it is sampled directly once the synchronized toggle
// changes. Results (reg_rdata, sba_rdata) are written before the acknowledge
// toggles and held until the next request, so jtag_tap may capture them
// whenever it doesn't see a request pending.
//
// Reset: by TRST_n (synchronized: asserted asynchronously, released on clk),
// not by the system reset: a halt request survives RESET_L, so a debugger
// can halt the core at the reset vector.
module jtag_dm (
  input  logic        clk,
  input  logic        trst_n,
  // from jtag_tap (TCK domain)
  input  logic        req_tog,
  input  logic [2:0]  req_op,       // 1 CTL, 2 REG, 3 SBA read, 4 SBA write
  input  logic [3:0]  req_ctl,      // [0] haltreq, [1] resume, [2] step, [3] clear errors
  input  logic        req_we,
  input  logic [7:0]  req_sel,
  input  logic [63:0] req_data,
  input  logic [63:0] req_addr,
  input  logic [2:0]  req_size,
  // to jtag_tap
  output logic        ack_tog,
  output logic [63:0] reg_rdata,
  output logic [63:0] sba_rdata,
  output logic        st_halted,
  output logic        st_haltreq,
  output logic        st_sba_err,
  output logic        st_reg_err,
  // the core's debug ports
  output logic        dbg_req,      // halt at the next instruction boundary; stay halted while 1
  output logic        dbg_step,     // one-cycle pulse while halted: one instruction, then halt
  input  logic        dbg_halted,
  output logic [7:0]  dbg_reg_sel,
  output logic        dbg_reg_we,
  output logic [63:0] dbg_reg_wdata,
  input  logic [63:0] dbg_reg_rdata,
  // the fabric's debug master port: held until dm_ack (one cycle)
  output logic        dm_req,
  output logic        dm_we,
  output logic [63:0] dm_addr,
  output logic [2:0]  dm_size,      // log2 bytes, 0..3
  output logic [63:0] dm_wdata,
  input  logic        dm_ack,
  input  logic        dm_err,
  input  logic [63:0] dm_rdata
);
  // TRST_n: asynchronous assertion, synchronous release
  logic rst_s1, rst_n;
  always_ff @(posedge clk or negedge trst_n) begin
    if (!trst_n) begin
      rst_s1 <= 1'b0;
      rst_n <= 1'b0;
    end else begin
      rst_s1 <= 1'b1;
      rst_n <= rst_s1;
    end
  end

  // the request toggle: two flip-flops, then edge detection against 'seen'
  logic req_s1, req_s2, seen;
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      req_s1 <= 1'b0;
      req_s2 <= 1'b0;
    end else begin
      req_s1 <= req_tog;
      req_s2 <= req_s1;
    end
  end

  typedef enum logic [1:0] {IDLE, REG_RD, SBA, DONE} state_e;
  state_e st;

  assign st_halted = dbg_halted;
  assign st_haltreq = dbg_req;
  assign dm_size = req_size;
  assign dm_addr = req_addr;
  assign dm_wdata = req_data;
  assign dbg_reg_wdata = req_data;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      st <= IDLE;
      seen <= 1'b0;
      ack_tog <= 1'b0;
      reg_rdata <= '0;
      sba_rdata <= '0;
      st_sba_err <= 1'b0;
      st_reg_err <= 1'b0;
      dbg_req <= 1'b0;
      dbg_step <= 1'b0;
      dbg_reg_sel <= '0;
      dbg_reg_we <= 1'b0;
      dm_req <= 1'b0;
      dm_we <= 1'b0;
    end else begin
      dbg_step <= 1'b0;
      dbg_reg_we <= 1'b0;
      unique case (st)
        IDLE:
          if (req_s2 != seen) begin
            seen <= req_s2;
            unique case (req_op)
              3'd1: begin                     // DBGCTL
                if (req_ctl[3]) begin
                  st_sba_err <= 1'b0;
                  st_reg_err <= 1'b0;
                end
                if (req_ctl[0]) dbg_req <= 1'b1;
                if (req_ctl[1]) dbg_req <= 1'b0;
                if (req_ctl[2] && dbg_halted) dbg_step <= 1'b1;
                st <= DONE;
              end
              3'd2: begin                     // DBGREG: only while halted
                if (!dbg_halted) begin
                  st_reg_err <= 1'b1;
                  st <= DONE;
                end else begin
                  dbg_reg_sel <= req_sel;
                  if (req_we) begin
                    dbg_reg_we <= 1'b1;
                    st <= DONE;
                  end else
                    st <= REG_RD;             // the read data follows the select
                end
              end
              3'd3, 3'd4: begin               // SBA read / write
                dm_req <= 1'b1;
                dm_we <= req_op == 3'd4;
                st <= SBA;
              end
              default: st <= DONE;
            endcase
          end
        REG_RD: begin
          reg_rdata <= dbg_reg_rdata;
          st <= DONE;
        end
        SBA:
          if (dm_ack) begin
            dm_req <= 1'b0;
            if (dm_err) st_sba_err <= 1'b1;
            else if (!dm_we) sba_rdata <= dm_rdata;
            st <= DONE;
          end
        DONE: begin
          ack_tog <= seen;                    // results are in place: acknowledge
          st <= IDLE;
        end
      endcase
    end
  end
endmodule
