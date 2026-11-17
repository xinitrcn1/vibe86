// lapic: a core's local APIC (D-017). Intel SDM Vol. 3A chapter 12
// "Advanced Programmable Interrupt Controller (APIC)" and AMD APM Vol. 2
// chapter 16 "Advanced Programmable Interrupt Controller (APIC)":
//
//  - xAPIC: registers in the 4 KiB page at IA32_APIC_BASE (the fabric sends
//    the core's accesses there: mmio_*); x2APIC (IA32_APIC_BASE.EXTD): the
//    same registers as MSRs 800h + offset/16 (msr_*, from RDMSR/WRMSR), with
//    the x2APIC rules (12.12): the ID read-only, LDR derived from the ID, no
//    DFR/APR, ICR one 64-bit MSR, SELF IPI (83Fh), #GP for reserved MSRs,
//    write-only and read-only ones and reserved bits;
//  - TPR/PPR/APR, ISR/TMR/IRR, EOI (with the EOI message to the I/O APIC for
//    level-triggered vectors unless SVR[12] suppresses it), SVR (software
//    enable), ESR (send/receive illegal vector, illegal register address);
//  - the LVT: CMCI, timer, thermal, performance counter, LINT0, LINT1,
//    error (version 15h, max LVT 6: rtl/common/platform.toml [lapic]);
//  - the timer: one-shot, periodic, TSC-deadline (IA32_TSC_DEADLINE, 6E0h),
//    the divide configuration; its input clock is the core clock (1:1, as
//    platform.toml's timer_hz = core_hz says);
//  - the ICR: IPIs with every delivery mode and shorthand through the die's
//    APIC bus (apic_bus.sv), which also brings MSIs;
//  - LINT0 in ExtINT mode passes the 8259's INTR and its vector to the core
//    and the core's acknowledge back as INTA (virtual-wire mode: MP
//    Specification 1.4, 3.6.2.2); LINT1 is the NMI pin;
//  - IA32_APIC_BASE.EN = 0: the APIC is out of the way, INTR and NMI go to
//    the core directly (12.4.3); EN going to 0 resets the APIC.
//
// Simplifications (documented in D-017): no remote read (RRD), no Remote
// IRR/delivery status for level-triggered LINT inputs, thermal, performance
// and CMCI LVTs exist but never fire, ExtINT messages are ignored.
module lapic
  import apic_pkg::*;
#(
  parameter logic [31:0] APIC_ID = 32'd0,
  parameter logic [31:0] VERSION = 32'h0006_0015
) (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        apic_en,         // the core's IA32_APIC_BASE.EN (bit 11)
  input  logic        apic_extd,       // IA32_APIC_BASE.EXTD (bit 10): x2APIC mode
  input  logic [63:0] tsc,
  // xAPIC MMIO (the fabric): reads are combinational, a write is one pulse
  input  logic [11:4] mmio_off,        // the register (offset / 16)
  output logic [31:0] mmio_rdata,
  input  logic        mmio_wr,
  input  logic [31:0] mmio_wdata,
  // x2APIC MSRs and IA32_TSC_DEADLINE (the core's APIC micro-op)
  input  logic [31:0] msr_addr,
  output logic [63:0] msr_rdata,
  output logic        msr_rok,         // the MSR can be read
  input  logic        msr_wr,
  input  logic [63:0] msr_wdata,
  output logic        msr_wok,         // the write is legal (else #GP)
  // CR8 is TPR[7:4] (SDM 12.8.6.5? "Task Priority in IA-32e Mode", 10.8.6)
  input  logic        cr8_wr,
  input  logic [3:0]  cr8_wdata,
  output logic [3:0]  cr8_rdata,
  // the pins: the 8259's INTR (level) and its vector; the NMI pin (pulse)
  input  logic        lint0,
  input  logic [7:0]  lint0_vec,
  output logic        lint0_ack,       // INTA to the 8259
  input  logic        lint1,
  // to the core
  output logic        core_intr,
  output logic [7:0]  core_vec,
  input  logic        core_ack,
  output logic        core_nmi,
  output logic        core_smi,
  output logic        core_init,
  output logic        core_sipi,
  output logic [7:0]  sipi_vec,
  // the APIC bus: our IPIs out, every message in (we say whether it's for us)
  output logic        out_valid,
  output apic_msg_t   out_msg,
  input  logic        out_ready,
  input  logic        in_valid,
  input  apic_msg_t   in_msg,
  output logic        in_match,        // in_msg is addressed to us
  output logic [7:0]  arb_pri,         // APR, for lowest-priority arbitration
  input  logic        in_accept,       // deliver it (the bus picked us)
  // the EOI message for a level-triggered vector (to the I/O APIC)
  output logic        eoi_valid,
  output logic [7:0]  eoi_vec,
  input  logic        eoi_ack
);

  wire en = apic_en;
  wire x2 = apic_en & apic_extd;

  // ------------------------------------------------------------ state
  logic [7:0]   id_x;                // xAPIC ID[31:24] (writable)
  logic [7:0]   tpr;
  logic [31:0]  ldr, dfr, svr;
  logic [255:0] isr, tmr, irr;
  logic [31:0]  esr, esr_acc;
  logic [31:0]  lvt [7];             // 0 CMCI, 1 timer, 2 thermal, 3 perf, 4 LINT0, 5 LINT1, 6 error
  logic [31:0]  icr_lo, icr_hi;
  logic [31:0]  init_cnt, cur_cnt, dcr;
  logic [63:0]  deadline;
  logic [7:0]   divctr;
  logic         lint0_q, lint1_q;
  logic         eoi_pend;
  logic [31:0]  err_seen;            // ESR bits seen last clock (the error LVT fires on new ones)

  localparam int L_CMCI = 0, L_TIMER = 1, L_THERM = 2, L_PERF = 3, L_LINT0 = 4, L_LINT1 = 5, L_ERR = 6;

  wire sw_en = svr[8];

  function automatic logic [8:0] highest(input logic [255:0] v);   // {any, vector}
    for (int i = 255; i >= 0; i--)
      if (v[i]) return {1'b1, 8'(i)};
    return 9'd0;
  endfunction

  logic [8:0] isrv, irrv;
  logic [7:0] ppr, apr;
  logic [3:0] m;
  always_comb begin
    isrv = highest(isr);
    irrv = highest(irr);
    // PPR (12.8.3.1): TPR if TPR[7:4] >= ISRV[7:4], else ISRV[7:4]:0
    ppr = (tpr[7:4] >= isrv[7:4]) ? tpr : {isrv[7:4], 4'h0};
    // APR (12.6.2.4, Figure 12-?? "Arbitration Priority"): TPR when it is at
    // least the highest pending and above the highest in service, else the
    // highest of the three priority classes
    m = tpr[7:4];
    if (tpr[7:4] >= irrv[7:4] && tpr[7:4] > isrv[7:4]) apr = tpr;
    else begin
      if (isrv[7:4] > m) m = isrv[7:4];
      if (irrv[7:4] > m) m = irrv[7:4];
      apr = {m, 4'h0};
    end
  end

  // x2APIC: the logical ID from the ID (12.12.10.2): cluster ID[19:4] in
  // LDR[31:16], bit ID[3:0] set in LDR[15:0]
  wire [31:0] ldr_x2 = {APIC_ID[19:4], 16'(1) << APIC_ID[3:0]};
  wire [31:0] my_id  = x2 ? APIC_ID : {24'b0, id_x};

  // ------------------------------------------------------------ registers
  // read: {ok, value} of register index r (offset/16) in the current mode
  function automatic logic [64:0] reg_read(input logic [7:0] r, input logic xm);
    logic [63:0] v;
    logic ok;
    ok = 1'b1;
    v = '0;
    case (r)
      8'h02: v = xm ? 64'(APIC_ID) : {32'b0, id_x, 24'b0};
      8'h03: v = 64'(VERSION);
      8'h08: v = 64'(tpr);
      8'h09: if (xm) ok = 1'b0; else v = 64'(apr);
      8'h0A: v = 64'(ppr);
      8'h0B: if (xm) ok = 1'b0;                    // EOI is write-only
      8'h0C: ok = !xm;                             // RRD: not supported (reads 0)
      8'h0D: v = xm ? 64'(ldr_x2) : 64'(ldr);
      8'h0E: if (xm) ok = 1'b0; else v = 64'(dfr);
      8'h0F: v = 64'(svr);
      8'h28: v = 64'(esr);
      8'h2F: v = 64'(lvt[L_CMCI]);
      8'h30: v = xm ? {icr_hi, icr_lo} : 64'(icr_lo);
      8'h31: if (xm) ok = 1'b0; else v = 64'(icr_hi);
      8'h32: v = 64'(lvt[L_TIMER]);
      8'h33: v = 64'(lvt[L_THERM]);
      8'h34: v = 64'(lvt[L_PERF]);
      8'h35: v = 64'(lvt[L_LINT0]);
      8'h36: v = 64'(lvt[L_LINT1]);
      8'h37: v = 64'(lvt[L_ERR]);
      8'h38: v = 64'(init_cnt);
      8'h39: v = 64'(cur_cnt);
      8'h3E: v = 64'(dcr);
      8'h3F: ok = 1'b0;                            // SELF IPI: write-only (x2APIC only)
      default:
        if (r >= 8'h10 && r <= 8'h17) v = 64'(isr[32 * (r - 8'h10) +: 32]);
        else if (r >= 8'h18 && r <= 8'h1F) v = 64'(tmr[32 * (r - 8'h18) +: 32]);
        else if (r >= 8'h20 && r <= 8'h27) v = 64'(irr[32 * (r - 8'h20) +: 32]);
        else ok = 1'b0;
    endcase
    return {ok, v};
  endfunction

  // is a write of v to register r legal in x2APIC mode (12.12.1.2: #GP for
  // read-only registers and reserved bits)
  function automatic logic x2_write_ok(input logic [7:0] r, input logic [63:0] v);
    case (r)
      8'h08: return v[63:8] == 0;
      8'h0B: return v == 0;                         // EOI: a write of 0
      8'h0F: return v[63:13] == 0 && v[11:10] == 0;
      8'h28: return v == 0;
      8'h2F, 8'h33, 8'h34: return v[63:17] == 0;
      8'h30: return v[17:16] == 0 && v[13] == 0;
      8'h32: return v[63:19] == 0;
      8'h35, 8'h36: return v[63:17] == 0;
      8'h37: return v[63:17] == 0;
      8'h38: return v[63:32] == 0;
      8'h3E: return v[63:4] == 0 && v[2] == 0;
      8'h3F: return v[63:8] == 0;
      default: return 1'b0;                         // read-only or reserved
    endcase
  endfunction

  // the MSR port: 800h-8FFh in x2APIC mode, 6E0h (TSC deadline) whenever
  // the APIC is enabled
  always_comb begin
    logic [64:0] rr;
    msr_rdata = '0;
    msr_rok = 1'b0;
    msr_wok = 1'b0;
    if (msr_addr == 32'h6E0) begin
      msr_rok = en;
      msr_wok = en;
      msr_rdata = deadline;
    end else if (x2 && msr_addr[31:8] == 24'h000008) begin
      rr = reg_read(msr_addr[7:0], 1'b1);
      msr_rok = rr[64];
      msr_rdata = rr[63:0];
      msr_wok = x2_write_ok(msr_addr[7:0], msr_wdata);
    end
    rr = reg_read(mmio_off, 1'b0);
    mmio_rdata = rr[31:0];
  end

  assign cr8_rdata = tpr[7:4];
  assign arb_pri = apr;

  // ------------------------------------------------------------ delivery
  // a message for us? (12.6.2: physical and logical destinations, the
  // shorthands; x2APIC logical cluster mode)
  always_comb begin
    in_match = 1'b0;
    if (en && in_valid) begin
      case (in_msg.sh)
        2'd1: in_match = in_msg.src == my_id;
        2'd2: in_match = 1'b1;
        2'd3: in_match = in_msg.src != my_id;
        default:
          if (!in_msg.dm) begin
            if (in_msg.x2) in_match = in_msg.dest == 32'hFFFF_FFFF || in_msg.dest == my_id;
            else in_match = in_msg.dest[7:0] == 8'hFF || in_msg.dest[7:0] == my_id[7:0];
          end else if (x2) begin
            in_match = in_msg.dest == 32'hFFFF_FFFF ||
                       (in_msg.dest[31:16] == ldr_x2[31:16] && (in_msg.dest[15:0] & ldr_x2[15:0]) != 0);
          end else if (dfr[31:28] == 4'hF) begin   // flat model
            in_match = (in_msg.dest[7:0] & ldr[31:24]) != 0;
          end else begin                           // cluster model
            in_match = in_msg.dest[7:0] == 8'hFF ||
                       (in_msg.dest[7:4] == ldr[31:28] && (in_msg.dest[3:0] & ldr[27:24]) != 0);
          end
      endcase
    end
  end

  // the interrupt the core is offered: the highest pending vector above
  // PPR's class (12.8.3), else LINT0's ExtINT (the 8259)
  wire irr_ok = en && sw_en && irrv[8] && irrv[7:4] > ppr[7:4];
  wire extint = lvt[L_LINT0][10:8] == 3'b111 && !lvt[L_LINT0][16] && lint0;
  always_comb begin
    if (!en) begin                                 // globally disabled: the pins go to the core
      core_intr = lint0;
      core_vec = lint0_vec;
    end else if (irr_ok) begin
      core_intr = 1'b1;
      core_vec = irrv[7:0];
    end else begin
      core_intr = extint;
      core_vec = lint0_vec;
    end
  end
  assign lint0_ack = core_ack && (!en || !irr_ok);

  // ------------------------------------------------------------ the timer
  function automatic logic [7:0] divisor(input logic [2:0] d);   // {DCR[3], DCR[1:0]}
    case (d)
      3'b000: return 8'd2;
      3'b001: return 8'd4;
      3'b010: return 8'd8;
      3'b011: return 8'd16;
      3'b100: return 8'd32;
      3'b101: return 8'd64;
      3'b110: return 8'd128;
      default: return 8'd1;
    endcase
  endfunction

  // ------------------------------------------------------------ sequential
  // accept a fixed interrupt into IRR (illegal vectors 0-15 set ESR bit 6)
  task automatic post(input logic [7:0] v, input logic level);
    if (v < 8'd16) esr_acc[6] <= 1'b1;
    else begin
      irr[v] <= 1'b1;
      tmr[v] <= level;
    end
  endtask

  // power-up / reset state (12.4.7.1), also after the APIC is globally
  // disabled (IA32_APIC_BASE.EN = 0)
  task automatic reset_regs();
      id_x <= APIC_ID[7:0];
    tpr <= '0;
    ldr <= '0;
    dfr <= 32'hFFFF_FFFF;
    svr <= 32'h0000_00FF;
    isr <= '0;
    tmr <= '0;
    irr <= '0;
    esr <= '0;
    esr_acc <= '0;
    for (int i = 0; i < 7; i++) lvt[i] <= 32'h0001_0000;
    icr_lo <= '0;
    icr_hi <= '0;
    init_cnt <= '0;
    cur_cnt <= '0;
    dcr <= '0;
    deadline <= '0;
    divctr <= '0;
    lint0_q <= 1'b0;
    lint1_q <= 1'b0;
    out_valid <= 1'b0;
    out_msg <= '0;
    eoi_pend <= 1'b0;
    eoi_vec <= '0;
    core_nmi <= 1'b0;
    core_smi <= 1'b0;
    core_init <= 1'b0;
    core_sipi <= 1'b0;
    sipi_vec <= '0;
  endtask

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) reset_regs();
    else if (!en) reset_regs();
    else begin
      logic        wr;
      logic [7:0]  r;
      logic [63:0] v;
      core_nmi <= 1'b0;
      core_smi <= 1'b0;
      core_init <= 1'b0;
      core_sipi <= 1'b0;
      lint0_q <= lint0;
      lint1_q <= lint1;
      if (out_valid && out_ready) out_valid <= 1'b0;
      if (eoi_pend && eoi_ack) eoi_pend <= 1'b0;

      // the core takes the offered interrupt: IRR -> ISR (ExtINT goes to the 8259)
      if (core_ack && irr_ok) begin
        irr[irrv[7:0]] <= 1'b0;
        isr[irrv[7:0]] <= 1'b1;
      end

      // LINT0 (not ExtINT) and LINT1: their LVT modes (12.5.1)
      if (!lvt[L_LINT0][16] && lvt[L_LINT0][10:8] != 3'b111 && lint0 && !lint0_q) begin
        if (lvt[L_LINT0][10:8] == 3'b100) core_nmi <= 1'b1;
        else if (lvt[L_LINT0][10:8] == 3'b000 && sw_en) post(lvt[L_LINT0][7:0], 1'b0);
      end
      if (!lvt[L_LINT1][16] && lint1 && !lint1_q) begin
        if (lvt[L_LINT1][10:8] == 3'b100) core_nmi <= 1'b1;
        else if (lvt[L_LINT1][10:8] == 3'b000 && sw_en) post(lvt[L_LINT1][7:0], 1'b0);
      end

      // the timer: count in the divided clock; TSC-deadline mode compares the TSC
      if (lvt[L_TIMER][18:17] == 2'b10) begin
        if (deadline != 0 && tsc >= deadline) begin
          deadline <= '0;
          if (!lvt[L_TIMER][16] && sw_en) post(lvt[L_TIMER][7:0], 1'b0);
        end
      end else if (cur_cnt != 0) begin
        if (divctr + 8'd1 >= divisor({dcr[3], dcr[1:0]})) begin
          divctr <= '0;
          if (cur_cnt == 32'd1) begin
            if (!lvt[L_TIMER][16] && sw_en) post(lvt[L_TIMER][7:0], 1'b0);
            cur_cnt <= (lvt[L_TIMER][18:17] == 2'b01) ? init_cnt : 32'd0;
          end else cur_cnt <= cur_cnt - 32'd1;
        end else divctr <= divctr + 8'd1;
      end

      // a message from the APIC bus addressed to us
      if (in_valid && in_accept) begin
        case (in_msg.dlv)
          DLV_FIXED, DLV_LOWEST: if (sw_en) post(in_msg.vec, in_msg.trig);
          DLV_SMI: core_smi <= 1'b1;
          DLV_NMI: core_nmi <= 1'b1;
          DLV_INIT: if (!(in_msg.trig && !in_msg.level)) core_init <= 1'b1;   // not INIT level de-assert
          DLV_SIPI: begin
            core_sipi <= 1'b1;
            sipi_vec <= in_msg.vec;
          end
          DLV_EXTINT: ;                              // ExtINT messages: ignored
          default: ;
        endcase
      end

      // register writes: xAPIC MMIO or x2APIC MSRs
      wr = 1'b0;
      r = '0;
      v = '0;
      if (mmio_wr && !x2) begin
        wr = 1'b1;
        r = mmio_off;
        v = {32'b0, mmio_wdata};
        // an access to a reserved offset sets ESR bit 7 (illegal register address)
        if (!reg_read(r, 1'b0)[64] && r != 8'h0B && r != 8'h0C && r != 8'h3F) esr_acc[7] <= 1'b1;
      end else if (msr_wr && msr_addr == 32'h6E0) begin
        if (lvt[L_TIMER][18:17] == 2'b10) deadline <= msr_wdata;
      end else if (msr_wr && x2 && msr_addr[31:8] == 24'h000008 && msr_wok) begin
        wr = 1'b1;
        r = msr_addr[7:0];
        v = msr_wdata;
      end
      if (cr8_wr) tpr <= {cr8_wdata, 4'h0};
      if (wr) begin
        case (r)
          8'h02: if (!x2) id_x <= v[31:24];
          8'h08: tpr <= v[7:0];
          8'h0B: begin                               // EOI: the highest in service
            if (isrv[8]) begin
              isr[isrv[7:0]] <= 1'b0;
              if (tmr[isrv[7:0]] && !svr[12]) begin
                eoi_pend <= 1'b1;
                eoi_vec <= isrv[7:0];
              end
            end
          end
          8'h0D: if (!x2) ldr <= {v[31:24], 24'b0};
          8'h0E: if (!x2) dfr <= {v[31:28], 28'hFFF_FFFF};
          8'h0F: begin
            svr <= {19'b0, v[12], 3'b0, v[8:0]};
            if (!v[8]) for (int i = 0; i < 7; i++) lvt[i][16] <= 1'b1;   // software disable masks the LVT
          end
          8'h28: begin                               // ESR: latch the errors seen since the last write
            esr <= esr_acc;
            esr_acc <= '0;
          end
          8'h2F, 8'h33, 8'h34: lvt[r == 8'h2F ? L_CMCI : r == 8'h33 ? L_THERM : L_PERF] <=
              {15'b0, v[16] | !sw_en, 5'b0, v[10:8], v[7:0]};
          8'h30: begin
            // ICR: send (12.6.1); x2APIC writes it whole, xAPIC writes the
            // low half last (the high half holds the destination)
            logic [31:0] dst;
            icr_lo <= {12'b0, v[19:18], 2'b0, v[15:14], 2'b0, v[11:0]};
            if (x2) icr_hi <= v[63:32];
            dst = x2 ? v[63:32] : {24'b0, icr_hi[31:24]};
            if ((v[10:8] == DLV_FIXED || v[10:8] == DLV_LOWEST) && v[7:0] < 8'd16) esr_acc[5] <= 1'b1;
            else if (!out_valid) begin
              out_valid <= 1'b1;
              out_msg <= '{dest: dst, x2: x2, dm: v[11], sh: v[19:18], dlv: v[10:8],
                           level: v[14], trig: v[15], vec: v[7:0], src: my_id};
            end
          end
          8'h31: if (!x2) icr_hi <= {v[31:24], 24'b0};
          8'h32: begin
            if (v[18:17] != lvt[L_TIMER][18:17]) begin   // a mode change disarms the timer
              cur_cnt <= '0;
              deadline <= '0;
            end
            lvt[L_TIMER] <= {13'b0, v[18:17], v[16] | !sw_en, 8'b0, v[7:0]};
          end
          8'h35, 8'h36: lvt[r == 8'h35 ? L_LINT0 : L_LINT1] <=
              {15'b0, v[16] | !sw_en, v[15], 1'b0, v[13], 2'b0, v[10:8], v[7:0]};
          8'h37: lvt[L_ERR] <= {15'b0, v[16] | !sw_en, 8'b0, v[7:0]};
          8'h38: if (lvt[L_TIMER][18:17] != 2'b10) begin
            init_cnt <= v[31:0];
            cur_cnt <= v[31:0];
            divctr <= '0;
          end
          8'h3E: dcr <= {28'b0, v[3], 1'b0, v[1:0]};
          8'h3F: if (!out_valid) begin                // SELF IPI (x2APIC)
            if (v[7:0] < 8'd16) esr_acc[5] <= 1'b1;
            else begin
              out_valid <= 1'b1;
              out_msg <= '{dest: APIC_ID, x2: 1'b1, dm: 1'b0, sh: 2'd1, dlv: DLV_FIXED,
                           level: 1'b1, trig: 1'b0, vec: v[7:0], src: my_id};
            end
          end
          default: ;
        endcase
      end

      // the error LVT: one interrupt when a new error bit appears
      if ((esr_acc & ~err_seen) != 0 && !lvt[L_ERR][16] && sw_en) post(lvt[L_ERR][7:0], 1'b0);
      err_seen <= esr_acc;
    end
  end


  assign eoi_valid = eoi_pend;

endmodule
