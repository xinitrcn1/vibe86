// v1_core: vibe86's first core. Not pipelined: it decodes one instruction a
// byte at a time, then runs its microcode routine one micro-op per cycle
// (memory micro-ops until the bus acknowledges). The microcode, the micro-op
// ISA and the decoder's tables are generated from the Intel SDM by
// tools/ucc (D-002, D-003); this file is the datapath that runs them.
//
//   decode   prefixes, REX, the opcode maps, ModRM/SIB/displacement and the
//            immediate, then the effective address (16-, 32- or 64-bit
//            addressing) and the routine's entry point (ucode_pkg::uentry);
//   execute  micro-ops against a working copy of the GPRs, RFLAGS and RIP
//            that END commits and a fault discards (D-005);
//   memory   segmentation (base, limit, type, canonical checks) and paging:
//            a TLB whose misses trap to the WALK microcode routine (D-008);
//   events   exceptions and interrupts enter the DELIVER routine.
//
// Reset state: Intel SDM Vol. 3A, 10.1.1 "Processor State After Reset"
// (Table 10-1): real mode, CS.base FFFF_0000H, IP FFF0H.
module v1_core
  import v1_uop_pkg::*;
  import v1_ucode_pkg::*;
(
  input  logic         clk,
  input  logic         rst_n,
  // core-uncore bus
  output logic         bus_req,
  output logic         bus_we,
  output logic         bus_io,
  output logic [63:0]  bus_addr,
  output logic [2:0]   bus_size,
  output logic [127:0] bus_wdata,
  input  logic         bus_ack,
  input  logic         bus_err,
  input  logic [127:0] bus_rdata,
  // events from the uncore
  input  logic         intr_valid,      // a maskable interrupt request (level), and its vector
  input  logic [7:0]   intr_vector,
  output logic         intr_ack,        // taken: the INTA cycle (one pulse)
  input  logic         nmi,
  input  logic         mce_valid,
  input  logic [1:0]   mce_type,
  input  logic [63:0]  mce_addr,
  // trace
  output logic         rt_valid,
  output logic [63:0]  rt_rip,
  output logic [3:0]   rt_len,
  output logic [127:0] rt_bytes,
  output logic [7:0]   rt_uops,
  output logic         ev_valid,
  output logic [7:0]   ev_vector,
  output logic         ev_has_error,
  output logic [31:0]  ev_error,
  output logic [63:0]  ev_rip,
  output logic         shutdown,
  output logic         halted,
  output logic [63:0]  shutdown_rip
);

  // ------------------------------------------------------------------ state

  typedef enum logic [2:0] {S_FETCH, S_EXEC, S_HALT, S_SHUTDOWN} state_e;
  typedef enum logic [3:0] {D_PFX, D_ESC, D_OPC3, D_OPC2, D_MODRM, D_SIB, D_DISP, D_IMM, D_FINISH,
                            D_BUS} dstate_e;

  state_e  st;
  dstate_e ds;

  logic [63:0] a_gpr [16];      // architectural (committed)
  logic [63:0] w_gpr [16];      // working
  logic [63:0] a_flags, w_flags;
  logic [63:0] rip, nrip;
  logic [63:0] tb [4][32];      // T0-T31: one bank per micro-call level (UCALL/URET)
  logic [63:0] tt [32];         // T32-T63: the micro-trap routines' (WALK)
  logic [63:0] ua [4];          // A0-A3: micro-subroutine arguments
  logic [1:0]  udepth;          // micro-call depth
  logic [63:0] hi_r, rem_r;
  logic        u_eq, u_ltu, u_lts;
  logic [63:0] sp [256];        // special registers (uop_pkg::SPECIAL)

  // decode
  logic [3:0]  idx;             // bytes consumed
  logic        p66, p67, plock;
  logic [1:0]  prep;            // 0 none, 1 F3, 2 F2
  logic [2:0]  pseg;            // segment override: 0-5, 7 none
  logic        rex_v;
  logic [3:0]  rex;             // W R X B
  logic [1:0]  dmap;
  logic [7:0]  dopc;
  logic [7:0]  modrm, sib;
  logic        has_modrm, has_sib;
  logic [63:0] disp;
  logic [2:0]  disp_n, disp_i;
  logic [63:0] imm_r, imm2_r;
  logic [3:0]  imm_n, imm_i;
  dattr_t      attr;
  logic [127:0] ibytes;         // the instruction's bytes (trace)
  logic [3:0]  dreg, drm;       // decoded register numbers
  logic [63:0] ea;
  logic [2:0]  easeg;
  logic [1:0]  dosz;            // 1 16, 2 32, 3 64
  logic [1:0]  dasz;            // 0 16, 1 32, 2 64

  // fetch line buffer (tagged by linear address)
  logic        lb_v;
  logic [59:0] lb_tag;
  logic [127:0] lb_data;

  // execute
  logic [UADDR_W-1:0] upc, upc_trap;
  logic [UADDR_W-1:0] ustack [4];
  logic        in_delivery, in_trap, trap_fetch;
  logic [7:0]  ucount;
  logic        halt_req;
  logic        intr_pend;
  logic [7:0]  intr_vec;
  logic        nmi_pend;
  logic [7:0]  deliv_vec;       // the event being delivered (for #DF)
  logic [3:0]  db_data;         // data breakpoints hit by this instruction (DR6.B0-B3)
  logic        flags_loaded;    // this instruction loaded RFLAGS (IRET, POPF): RF stays
  logic [63:0] rng;             // RDRAND/RDSEED: xorshift64 (Marsaglia, J. Stat. Softw. 8(14),
                                // 2003): NOT a cryptographic source
  logic        shadow_set;      // this instruction asked for an interrupt shadow
  logic        int_shadow;      // no interrupts at this boundary (MOV SS, POP SS, STI)

  // memory micro-op state
  typedef enum logic [2:0] {M_IDLE, M_BUS, M_BYTES} mstate_e;
  mstate_e     ms;
  logic [4:0]  mb_i;            // byte index for split accesses
  logic [127:0] mdata;

  // TLB
  localparam int NTLB = 64;
  logic        tlb_v   [NTLB];
  logic [51:0] tlb_vpn [NTLB];
  logic [1:0]  tlb_ps  [NTLB];  // 0 4K, 1 2M, 2 1G
  logic [51:0] tlb_ppn [NTLB];
  logic        tlb_w [NTLB], tlb_u [NTLB], tlb_nx [NTLB], tlb_g [NTLB], tlb_d [NTLB];
  logic [11:0] tlb_pcid [NTLB];
  logic [5:0]  tlb_next;

  // ------------------------------------------------------------------ modes

  wire cr0_pe = sp[SP_CR0[7:0]][0];
  wire cr0_pg = sp[SP_CR0[7:0]][31];
  wire cr0_wp = sp[SP_CR0[7:0]][16];
  wire efer_lma = sp[SP_EFER[7:0]][10];
  wire efer_nxe = sp[SP_EFER[7:0]][11];
  wire cr4_la57 = sp[SP_CR4[7:0]][12];
  wire cr4_pcide = sp[SP_CR4[7:0]][17];
  wire cr4_smep = sp[SP_CR4[7:0]][20];
  wire cr4_smap = sp[SP_CR4[7:0]][21];
  function automatic logic [7:0] segi(input int s, input int part);
    return 8'(64 + 4 * s + part);
  endfunction
  wire [63:0] cs_attr = sp[segi(1, 3)];
  wire [63:0] ss_attr = sp[segi(2, 3)];
  wire cs_l = cs_attr[9];
  wire cs_d = cs_attr[10];
  wire rf_vm = w_flags[17];
  logic [1:0] mode;   // 0 real, 1 16-bit protected / v86, 2 32-bit, 3 64-bit
  always_comb begin
    if (!cr0_pe) mode = 2'd0;
    else if (efer_lma && cs_l) mode = 2'd3;
    else if (rf_vm || !cs_d) mode = 2'd1;
    else mode = 2'd2;
  end
  wire ssz = (mode != 0) && ss_attr[10];
  wire [1:0] cpl = (mode == 0) ? 2'd0 : sp[SP_CPL[7:0]][1:0];

  // ------------------------------------------------------------------ helpers

  function automatic logic [63:0] szmask(input logic [1:0] s);
    case (s)
      2'd0: return 64'hFF;
      2'd1: return 64'hFFFF;
      2'd2: return 64'hFFFF_FFFF;
      default: return '1;
    endcase
  endfunction

  // x86 condition codes (Intel SDM Vol. 1, appendix B)
  function automatic logic cond(input logic [3:0] k, input logic [63:0] f);
    logic r;
    case (k[3:1])
      3'd0: r = f[11];
      3'd1: r = f[0];
      3'd2: r = f[6];
      3'd3: r = f[0] | f[6];
      3'd4: r = f[7];
      3'd5: r = f[2];
      3'd6: r = f[7] != f[11];
      default: r = f[6] | (f[7] != f[11]);
    endcase
    return r ^ k[0];
  endfunction

  // register reads with x86 partial-register rules
  function automatic logic [63:0] gpr_rd(input logic [3:0] i, input logic hi8);
    return hi8 ? {56'b0, w_gpr[i[1:0]][15:8]} : w_gpr[i];
  endfunction

  uop_t u;
  assign u = uop_t'(UROM[upc]);

  wire reg_hi8 = (u.sz == 2'd0) && !rex_v && dreg[3:2] == 2'b01;
  wire rm_hi8  = (u.sz == 2'd0) && !rex_v && drm[3:2] == 2'b01;

  function automatic logic [63:0] rd(input logic [6:0] s);
    if (s < 7'd16) return w_gpr[s[3:0]];
    if (s < 7'd20) return {56'b0, w_gpr[s[1:0]][15:8]};
    if (s < S_T32) return tb[udepth][5'(s - S_T0)];
    if (s <= S_T63) return tt[5'(s - S_T32)];
    if (s >= S_A0 && s <= S_A3) return ua[2'(s - S_A0)];
    case (s)
      S_REG:   return reg_hi8 ? {56'b0, w_gpr[dreg[1:0]][15:8]} : w_gpr[dreg];
      S_RM:    return rm_hi8 ? {56'b0, w_gpr[drm[1:0]][15:8]} : w_gpr[drm];
      S_IMM:   return imm_r;
      S_IMM2:  return imm2_r;
      S_EA:    return ea;
      S_NRIP:  return nrip;
      S_RIP:   return rip;
      S_FLAGS: return w_flags;
      S_K:     return {{32{u.imm[31]}}, u.imm};
      S_ZERO:  return '0;
      S_ONE:   return 64'd1;
      S_HI:    return hi_r;
      S_REM:   return rem_r;
      S_SEGREG: return sp[segi(int'(modrm[5:3]), 0)];
      S_TRAPVA: return sp[8'd250];
      S_TRAPX:  return sp[8'd251];
      default: return '0;
    endcase
  endfunction

  // ------------------------------------------------------------------ ALU

  logic [63:0] alu_res, alu_hi;
  logic        alu_hi_we, alu_rem_we, alu_de, alu_nowrite;
  logic [5:0]  alu_fl, alu_native;
  logic [63:0] va, vb, vc, vd;
  assign va = rd(u.a);
  assign vb = rd(u.b);
  assign vc = rd(u.c);
  assign vd = rd(u.dst);

  v1_alu alu (
    .op(u.op), .sz(u.sz), .a(va), .b(vb), .c(vc), .dst_old(vd), .hi_in(hi_r), .imm(u.imm),
    .cc(u.cc), .x(u.x), .flags_in(w_flags), .res(alu_res), .hi_out(alu_hi),
    .hi_we(alu_hi_we), .rem_we(alu_rem_we), .fl(alu_fl), .native(alu_native), .de(alu_de),
    .nowrite(alu_nowrite));

  // ------------------------------------------------------------------ segmentation and paging

  // the segment of a memory micro-op: explicit, or the instruction's operand segment
  function automatic logic [3:0] seg_of(input logic [3:0] s);
    if (s == SEG_EASEG) return {1'b0, easeg};
    return s;
  endfunction

  // linear address and segment checks (Intel SDM Vol. 3A, 5.3 "Limit Checking",
  // 5.4 "Type Checking", 3.3.7.1 "Canonical Addressing")
  typedef struct packed {
    logic [63:0] la;
    logic        fault;       // #GP(0) or #SS(0)
    logic        ss;
  } segchk_t;

  function automatic segchk_t seg_check(input logic [3:0] s, input logic [63:0] off,
                                        input int unsigned n, input logic wr);
    segchk_t r;
    logic [63:0] base, limit, attr;
    logic [63:0] last;
    r = '0;
    r.ss = (s == 4'd2);
    if (s >= 4'd6) begin
      r.la = off;
      return r;
    end
    base = sp[segi(int'(s), 1)];
    limit = sp[segi(int'(s), 2)];
    attr = sp[segi(int'(s), 3)];
    if (mode == 2'd3) begin
      r.la = (s == 4'd4 || s == 4'd5) ? base + off : off;
      // canonical: bits 63:47 (63:56 with 5-level paging) all equal
      if (cr4_la57 ? (r.la[63:56] != {8{r.la[55]}}) : (r.la[63:47] != {17{r.la[47]}}))
        r.fault = 1'b1;
      return r;
    end
    r.la = {32'b0, base[31:0] + off[31:0]};
    last = off + 64'(n) - 1;
    if (rf_vm) begin
      // virtual-8086 mode: real-mode segments, 64-KByte limits (Vol. 3A 21.1.1)
      if (last > 64'hFFFF) r.fault = 1'b1;
      return r;
    end
    if (mode != 2'd0) begin
      if (!attr[7]) r.fault = 1'b1;                       // not present / null selector
      else if (attr[4] && attr[3]) begin                   // code segment
        if (wr || !attr[1]) r.fault = 1'b1;               // not writable; execute-only
      end else if (attr[4] && wr && !attr[1]) r.fault = 1'b1;   // read-only data
    end
    if (attr[4] && !attr[3] && attr[2]) begin              // expand-down data
      if (off <= limit || last > (attr[10] ? 64'hFFFF_FFFF : 64'hFFFF)) r.fault = 1'b1;
    end else if (last > limit) r.fault = 1'b1;
    return r;
  endfunction

  // TLB lookup: a hit only for an access the entry allows; anything else
  // (a miss, a permission the entry doesn't grant, a clean page written)
  // traps to WALK, which checks the paging structures and installs the
  // translation or raises #PF (Intel SDM Vol. 3A, 4.6 "Access Rights")
  typedef struct packed {
    logic        hit;
    logic [63:0] pa;
  } tlbres_t;

  function automatic tlbres_t tlb_look(input logic [63:0] la, input logic wr, input logic fetch,
                                       input logic user);
    tlbres_t r;
    r = '0;
    for (int i = 0; i < NTLB; i++) begin
      logic match;
      case (tlb_ps[i])
        2'd0: match = la[63:12] == tlb_vpn[i];
        2'd1: match = la[63:21] == tlb_vpn[i][51:9];
        default: match = la[63:30] == tlb_vpn[i][51:18];
      endcase
      if (tlb_v[i] && match && (tlb_g[i] || !cr4_pcide || tlb_pcid[i] == sp[SP_CR3[7:0]][11:0])) begin
        logic ok;
        ok = 1'b1;
        if (user && !tlb_u[i]) ok = 1'b0;
        if (wr && !tlb_w[i] && (user || cr0_wp)) ok = 1'b0;
        if (wr && !tlb_d[i]) ok = 1'b0;
        if (fetch && tlb_nx[i] && efer_nxe) ok = 1'b0;
        if (fetch && !user && tlb_u[i] && cr4_smep) ok = 1'b0;
        if (!fetch && !user && tlb_u[i] && cr4_smap && !w_flags[18]) ok = 1'b0;
        if (ok) begin
          r.hit = 1'b1;
          case (tlb_ps[i])
            2'd0: r.pa = {tlb_ppn[i], la[11:0]};
            2'd1: r.pa = {tlb_ppn[i][51:9], la[20:0]};
            default: r.pa = {tlb_ppn[i][51:18], la[29:0]};
          endcase
        end
      end
    end
    return r;
  endfunction

  // ------------------------------------------------------------------ fetch / decode

  // linear address of the next instruction byte
  wire [63:0] ip_cur = rip + 64'(idx);
  wire [63:0] fetch_off = (mode == 2'd3) ? ip_cur : (mode == 2'd2 ? {32'b0, ip_cur[31:0]} : {48'b0, ip_cur[15:0]});
  wire [63:0] fetch_la = (mode == 2'd3) ? fetch_off : {32'b0, sp[segi(1, 1)][31:0] + fetch_off[31:0]};
  wire        fetch_hit = lb_v && lb_tag == fetch_la[63:4];
  wire [7:0]  fbyte = lb_data[{fetch_la[3:0], 3'b0} +: 8];
  wire        fetch_limit_bad = (mode != 2'd3) && (fetch_off > sp[segi(1, 2)]);

  wire        m64 = (mode == 2'd3);
  // instruction breakpoints matching this instruction's first byte
  logic [3:0] ibp_hit;
  always_comb
    for (int i = 0; i < 4; i++)
      ibp_hit[i] = sp[SP_DR7[7:0]][2 * i +: 2] != 0 && sp[SP_DR7[7:0]][16 + 4 * i +: 2] == 2'd0 &&
                   sp[8'(9 + i)] == fetch_la;
  // decode states that consume the next instruction byte
  wire need_byte = ds == D_PFX || ds == D_ESC || ds == D_OPC3 || ds == D_MODRM || ds == D_SIB ||
                   (ds == D_DISP && disp_i != disp_n) || (ds == D_IMM && imm_i != imm_n);

  // opcodes with a register in their low 3 bits (50+rd, B8+rd, ...) are in
  // the tables under their base opcode
  function automatic logic [7:0] plus_base(input logic [7:0] o);
    if (dec_attr(m64, dmap, o, 3'd0).valid || dec_modrm(m64, dmap, o)) return o;
    // only a +r opcode (50+r, B8+r, ...) folds onto its base: any other
    // opcode without a form stays itself and decodes as #UD (EA in 64-bit
    // mode is not E8 CALL rel32)
    if (dec_attr(m64, dmap, {o[7:3], 3'b0}, 3'd0).plus_r) return {o[7:3], 3'b0};
    return o;
  endfunction

  function automatic logic is_legacy_prefix(input logic [7:0] b);
    return b == 8'h66 || b == 8'h67 || b == 8'hF0 || b == 8'hF2 || b == 8'hF3 ||
           b == 8'h2E || b == 8'h36 || b == 8'h3E || b == 8'h26 || b == 8'h64 || b == 8'h65;
  endfunction

  // operand/address size of the instruction being decoded
  function automatic logic [1:0] calc_osz(input logic [1:0] cls);
    if (mode == 2'd3) begin
      if (cls == 2'd2) return 2'd3;
      if (rex_v && rex[3]) return 2'd3;
      if (cls == 2'd1) return p66 ? 2'd1 : 2'd3;
      return p66 ? 2'd1 : 2'd2;
    end
    if (mode == 2'd2) return p66 ? 2'd1 : 2'd2;
    return p66 ? 2'd2 : 2'd1;
  endfunction
  wire [1:0] asz_now = (mode == 2'd3) ? (p67 ? 2'd1 : 2'd2) :
                       (mode == 2'd2) ? (p67 ? 2'd0 : 2'd1) : (p67 ? 2'd1 : 2'd0);

  function automatic logic [3:0] imm_bytes(input logic [3:0] k, input logic [1:0] osz);
    case (k)
      4'd8: return (osz == 2'd1) ? 4'd4 : 4'd6;     // far pointer: offset, selector
      4'd9: return (asz_now == 2'd0) ? 4'd2 : (asz_now == 2'd1 ? 4'd4 : 4'd8);   // moffs
      4'd0: return 4'd0;
      4'd1: return 4'd1;
      4'd2: return 4'd2;
      4'd3: return 4'd3;
      4'd4: return 4'd4;
      4'd5: return (osz == 2'd1) ? 4'd2 : 4'd4;
      4'd6: return (osz == 2'd1) ? 4'd2 : (osz == 2'd2 ? 4'd4 : 4'd8);
      default: return 4'd8;
    endcase
  endfunction

  // effective address (Intel SDM Vol. 2A, 2.1.5, Tables 2-1 to 2-3; 2.2.1.6 RIP-relative)
  function automatic logic [63:0] calc_ea(input logic [63:0] next_rip, output logic [2:0] dseg);
    logic [63:0] base, index, e;
    logic [1:0] md;
    logic [2:0] rm;
    md = modrm[7:6];
    rm = modrm[2:0];
    dseg = 3'd3;   // DS
    if (dasz == 2'd0) begin
      // 16-bit addressing
      case (rm)
        3'd0: e = a_gpr[3] + a_gpr[6];
        3'd1: e = a_gpr[3] + a_gpr[7];
        3'd2: begin e = a_gpr[5] + a_gpr[6]; dseg = 3'd2; end
        3'd3: begin e = a_gpr[5] + a_gpr[7]; dseg = 3'd2; end
        3'd4: e = a_gpr[6];
        3'd5: e = a_gpr[7];
        3'd6: begin
          if (md == 2'd0) e = 0;
          else begin e = a_gpr[5]; dseg = 3'd2; end
        end
        default: e = a_gpr[3];
      endcase
      e = (e + disp) & 64'hFFFF;
      return e;
    end
    if (has_sib) begin
      logic [3:0] bi, ii;
      bi = {rex_v & rex[0], sib[2:0]};
      ii = {rex_v & rex[1], sib[5:3]};
      if (sib[2:0] == 3'd5 && md == 2'd0) base = 0;
      else begin
        base = a_gpr[bi];
        if (bi[2:0] == 3'd4 || bi[2:0] == 3'd5) dseg = 3'd2;
      end
      index = (ii == 4'd4) ? 64'd0 : (a_gpr[ii] << sib[7:6]);
      e = base + index + disp;
    end else if (md == 2'd0 && rm == 3'd5) begin
      e = (mode == 2'd3) ? next_rip + disp : disp;
    end else begin
      logic [3:0] bi;
      bi = {rex_v & rex[0], rm};
      if (bi[2:0] == 3'd5 || bi[2:0] == 3'd4) dseg = 3'd2;
      e = a_gpr[bi] + disp;
    end
    if (dasz == 2'd1) e = e & 64'hFFFF_FFFF;
    return e;
  endfunction

  // ------------------------------------------------------------------ data memory

  wire is_mem = (u.op == U_LD || u.op == U_ST);
  wire [3:0] mseg = seg_of(u.seg);
  wire [63:0] moff = va + {{32{u.imm[31]}}, u.imm};
  wire [4:0] mn = 5'(1) << u.sz;           // bytes
  segchk_t mchk;
  assign mchk = seg_check(mseg, moff, int'(mn), u.op == U_ST);
  wire paging = cr0_pg && mseg != 4'd7;
  wire maligned = (moff & 64'(mn - 1)) == 0;
  // the byte being accessed in a split (misaligned) access
  wire [63:0] mla_cur = ms == M_BYTES ? mchk.la + 64'(mb_i) : mchk.la;
  tlbres_t mtlb;
  assign mtlb = tlb_look(mla_cur, u.op == U_ST, 1'b0, cpl == 2'd3);
  tlbres_t ftlb;
  assign ftlb = tlb_look({fetch_la[63:4], 4'b0}, 1'b0, 1'b1, cpl == 2'd3);

  // ------------------------------------------------------------------ sequential

  task automatic commit();
    for (int i = 0; i < 16; i++) a_gpr[i] <= w_gpr[i];
    a_flags <= w_flags;
  endtask

  // exception classes for #DF (Intel SDM Vol. 3A, 7.15, Table 7-4)
  function automatic logic contributory(input logic [7:0] v);
    return v == 8'd0 || v == 8'd10 || v == 8'd11 || v == 8'd12 || v == 8'd13;
  endfunction

  // the exception/interrupt entry: roll back, record, enter DELIVER. An
  // exception while delivering another is #DF if both are contributory, or
  // the first is #PF and the second #PF or contributory (Table 7-5);
  // otherwise the second replaces the first. Anything while delivering #DF
  // shuts the processor down. keep: the instruction completed (a trap raised
  // at END): don't roll back.
  task automatic raise(input logic [7:0] vec, input logic has_err, input logic [31:0] err,
                       input logic trap, input logic swi, input logic ext,
                       input logic keep = 1'b0);
    logic [7:0] v;
    logic he;
    logic [31:0] ec;
    v = vec;
    he = has_err;
    ec = err;
    if (in_delivery && deliv_vec == 8'd8) begin
      st <= S_SHUTDOWN;
      shutdown_rip <= rip;
      return;
    end
    if (in_delivery && ((contributory(deliv_vec) && contributory(vec)) ||
                        (deliv_vec == 8'd14 && (vec == 8'd14 || contributory(vec))))) begin
      v = 8'd8;
      he = 1'b1;
      ec = 32'd0;
    end
    if (!keep) begin
      for (int i = 0; i < 16; i++) w_gpr[i] <= a_gpr[i];
      w_flags <= a_flags;
    end
    sp[SP_FAULT_VEC[7:0]] <= {53'b0, ext, swi, he, v};
    sp[SP_FAULT_ERR[7:0]] <= {32'b0, ec};
    sp[SP_FAULT_RIP[7:0]] <= trap ? nrip : rip;
    nrip <= trap ? nrip : rip;
    ev_valid <= 1'b1;
    ev_vector <= v;
    ev_has_error <= he;
    ev_error <= ec;
    ev_rip <= rip;
    upc <= UADDR_DELIVER;
    udepth <= '0;
    bus_req <= 1'b0;
    bus_io <= 1'b0;
    in_delivery <= 1'b1;
    deliv_vec <= v;
    in_trap <= 1'b0;
    shadow_set <= 1'b0;
    db_data <= '0;
    ms <= M_IDLE;
    st <= S_EXEC;
  endtask

  // a TLB miss or denied access: run WALK, then retry
  task automatic utrap(input logic [63:0] la, input logic [3:0] kind, input logic from_fetch);
    `ifdef V1_DEBUG $display("utrap la=%h kind=%h fetch=%0d upc=%0d", la, kind, from_fetch, upc); `endif
    sp[8'd250] <= la;
    sp[8'd251] <= {60'b0, kind};
    upc_trap <= upc;
    trap_fetch <= from_fetch;
    in_trap <= 1'b1;
    upc <= UADDR_WALK;
    ms <= M_IDLE;
    st <= S_EXEC;
  endtask

  task automatic wr(input logic [6:0] s, input logic [1:0] sz, input logic [63:0] v);
    logic [63:0] m;
    m = szmask(sz);
    if (s < 7'd16 || s == S_REG || s == S_RM) begin
      logic [3:0] i;
      logic hi8;
      i = (s == S_REG) ? dreg : (s == S_RM) ? drm : s[3:0];
      hi8 = (s == S_REG) ? reg_hi8 : (s == S_RM) ? rm_hi8 : 1'b0;
      if (hi8) w_gpr[i[1:0]][15:8] <= v[7:0];
      else case (sz)
        2'd0: w_gpr[i][7:0] <= v[7:0];
        2'd1: w_gpr[i][15:0] <= v[15:0];
        2'd2: w_gpr[i] <= {32'b0, v[31:0]};
        default: w_gpr[i] <= v;
      endcase
    end else if (s < 7'd20) begin
      w_gpr[s[1:0]][15:8] <= v[7:0];
    end else if (s < S_T32) begin
      tb[udepth][5'(s - S_T0)] <= v & m;
    end else if (s <= S_T63) begin
      tt[5'(s - S_T32)] <= v & m;
    end else if (s >= S_A0 && s <= S_A3) begin
      ua[2'(s - S_A0)] <= v;
    end else begin
      case (s)
        S_NRIP:  nrip <= v;
        S_FLAGS: begin
          w_flags <= (v & 64'h0000_0000_003F_7FD5) | 64'h2;
          flags_loaded <= 1'b1;
        end
        S_HI:    hi_r <= v & m;
        S_REM:   rem_r <= v & m;
        default: ;
      endcase
    end
  endtask

  task automatic wr_special(input logic [7:0] n, input logic [63:0] v);
    if (n == SP_INT_SHADOW[7:0]) begin
      shadow_set <= 1'b1;
      return;
    end
    sp[n] <= v;
    if (n == SP_CR0[7:0]) begin
      // IA-32e mode becomes active when paging is enabled with EFER.LME set
      // (Intel SDM Vol. 3A, 10.8.5 "Initializing IA-32e Mode")
      if (v[31] && sp[SP_EFER[7:0]][8]) sp[SP_EFER[7:0]][10] <= 1'b1;
      if (!v[31]) sp[SP_EFER[7:0]][10] <= 1'b0;
      for (int i = 0; i < NTLB; i++) tlb_v[i] <= 1'b0;
    end
    if (n == SP_EFER[7:0]) sp[n] <= {v[63:11], sp[n][10], v[9:0]};   // LMA is read-only
    if (n == SP_CR3[7:0] || n == SP_CR4[7:0])
      for (int i = 0; i < NTLB; i++) if (!tlb_g[i] || n == SP_CR4[7:0]) tlb_v[i] <= 1'b0;
    if (n == SP_CR3[7:0] || n == SP_CR0[7:0] || n == SP_CR4[7:0]) lb_v <= 1'b0;
  endtask

  task automatic reset_state();
    for (int i = 0; i < 16; i++) begin
      a_gpr[i] <= '0;
      w_gpr[i] <= '0;
    end
    a_gpr[2] <= 64'h0000_0601;  // EDX: the processor signature (CPUID.1:EAX)
    w_gpr[2] <= 64'h0000_0601;
    a_flags <= 64'h2;
    w_flags <= 64'h2;
    rip <= 64'hFFF0;
    nrip <= 64'hFFF0;
    for (int i = 0; i < 256; i++) sp[i] <= '0;
    sp[SP_CR0[7:0]] <= 64'h6000_0010;
    for (int s = 0; s < 6; s++) begin
      sp[segi(s, 0)] <= 64'h0;
      sp[segi(s, 1)] <= 64'h0;
      sp[segi(s, 2)] <= 64'hFFFF;
      sp[segi(s, 3)] <= 64'h93;           // present, read/write, accessed
    end
    sp[segi(1, 0)] <= 64'hF000;
    sp[segi(1, 1)] <= 64'hFFFF_0000;
    sp[segi(1, 3)] <= 64'h9B;             // present, execute/read, accessed
    sp[segi(6, 2)] <= 64'hFFFF;
    sp[segi(6, 3)] <= 64'h82;             // LDTR
    sp[segi(7, 2)] <= 64'hFFFF;
    sp[segi(7, 3)] <= 64'h8B;             // TR: busy 32-bit TSS
    sp[SP_GDTR_LIMIT[7:0]] <= 64'hFFFF;
    sp[SP_IDTR_LIMIT[7:0]] <= 64'hFFFF;
    sp[SP_MXCSR[7:0]] <= 64'h1F80;
    sp[SP_FCW[7:0]] <= 64'h037F;
    sp[SP_XCR0[7:0]] <= 64'h1;
    sp[SP_PAT[7:0]] <= 64'h0007_0406_0007_0406;
    sp[SP_APIC_BASE[7:0]] <= 64'hFEE0_0900;
    sp[SP_DR6[7:0]] <= 64'hFFFF_0FF0;
    rng <= 64'h9E37_79B9_7F4A_7C15;
    sp[SP_DR7[7:0]] <= 64'h400;
    udepth <= '0;
    db_data <= '0;
    deliv_vec <= '0;
    for (int i = 0; i < NTLB; i++) tlb_v[i] <= 1'b0;
    tlb_next <= '0;
    lb_v <= 1'b0;
    st <= S_FETCH;
    ds <= D_PFX;
    ms <= M_IDLE;
    idx <= '0;
    in_delivery <= 1'b0;
    in_trap <= 1'b0;
    halt_req <= 1'b0;
    intr_pend <= 1'b0;
    nmi_pend <= 1'b0;
    shadow_set <= 1'b0;
    int_shadow <= 1'b0;
    ucount <= '0;
    upc <= '0;
  endtask

  task automatic start_decode();
    idx <= '0;
    p66 <= 1'b0; p67 <= 1'b0; plock <= 1'b0; prep <= 2'd0; pseg <= 3'd7;
    rex_v <= 1'b0; rex <= '0;
    dmap <= '0;
    has_modrm <= 1'b0; has_sib <= 1'b0;
    disp <= '0; disp_n <= '0; disp_i <= '0;
    imm_r <= '0; imm2_r <= '0; imm_n <= '0; imm_i <= '0;
    ibytes <= '0;
    ucount <= '0;
    flags_loaded <= 1'b0;
    ds <= D_PFX;
  endtask

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      reset_state();
      bus_req <= 1'b0;
      rt_valid <= 1'b0;
      ev_valid <= 1'b0;
      intr_ack <= 1'b0;
      shutdown <= 1'b0;
      halted <= 1'b0;
      shutdown_rip <= '0;
      start_decode();
    end else begin
      rt_valid <= 1'b0;
      ev_valid <= 1'b0;
      intr_ack <= 1'b0;
      sp[SP_TSC[7:0]] <= sp[SP_TSC[7:0]] + 1;
      if (nmi) nmi_pend <= 1'b1;
      shutdown <= (st == S_SHUTDOWN);
      halted <= (st == S_HALT);

      case (st)
        // ------------------------------------------------------------ decode
        S_FETCH: begin
          if (ds == D_BUS) begin
            if (bus_ack) begin
              bus_req <= 1'b0;
              if (bus_err) raise(8'd13, 1'b1, 32'd0, 1'b0, 1'b0, 1'b0);
              else begin
                lb_v <= 1'b1;
                lb_data <= bus_rdata;
                ds <= dstate_e'(mdata[3:0]);   // resume
              end
            end
          end else if (ds == D_PFX && idx == 0 && !in_delivery && !w_flags[16] && ibp_hit != 0) begin
            sp[SP_DR6[7:0]] <= (sp[SP_DR6[7:0]] & ~64'hF) | {60'b0, ibp_hit};
            raise(8'd1, 1'b0, 32'd0, 1'b0, 1'b0, 1'b0);
          end else if (need_byte && !fetch_hit) begin
            `ifdef V1_DEBUG $display("fetch la=%h ds=%0d hit=%0d pa=%h", fetch_la, ds, ftlb.hit, ftlb.pa); `endif
            // fetch the 16-byte line holding the next byte
            if (idx == 4'd15 || fetch_limit_bad) raise(8'd13, 1'b1, 32'd0, 1'b0, 1'b0, 1'b0);
            else if (cr0_pg && !ftlb.hit) utrap({fetch_la[63:4], 4'b0}, 4'b0100 | {2'b0, cpl == 2'd3, 1'b0}, 1'b1);
            else begin
              bus_req <= 1'b1;
              bus_we <= 1'b0;
              bus_io <= 1'b0;
              bus_size <= 3'd4;
              bus_addr <= cr0_pg ? {ftlb.pa[63:4], 4'b0} : {fetch_la[63:4], 4'b0};
              lb_tag <= fetch_la[63:4];
              mdata[3:0] <= 4'(ds);
              ds <= D_BUS;
            end
          end else begin
            logic [7:0] b;
            b = fbyte;
            if (need_byte) ibytes[{idx, 3'b0} +: 8] <= b;
            case (ds)
              D_PFX: begin
                if (is_legacy_prefix(b)) begin
                  case (b)
                    8'h66: p66 <= 1'b1;
                    8'h67: p67 <= 1'b1;
                    8'hF0: plock <= 1'b1;
                    8'hF2: prep <= 2'd2;
                    8'hF3: prep <= 2'd1;
                    8'h26: pseg <= 3'd0;
                    8'h2E: pseg <= 3'd1;
                    8'h36: pseg <= 3'd2;
                    8'h3E: pseg <= 3'd3;
                    8'h64: pseg <= 3'd4;
                    default: pseg <= 3'd5;
                  endcase
                  rex_v <= 1'b0;      // a REX prefix followed by a legacy prefix is ignored
                  idx <= idx + 1;
                end else if (m64 && b[7:4] == 4'h4) begin
                  rex_v <= 1'b1;
                  rex <= b[3:0];
                  idx <= idx + 1;
                end else if (b == 8'h0F) begin
                  dmap <= 2'd1;
                  idx <= idx + 1;
                  ds <= D_ESC;
                end else begin
                  dmap <= 2'd0;
                  dopc <= b;
                  idx <= idx + 1;
                  ds <= D_OPC2;
                end
              end
              D_ESC: begin
                idx <= idx + 1;
                if (b == 8'h38) begin dmap <= 2'd2; ds <= D_OPC3; end
                else if (b == 8'h3A) begin dmap <= 2'd3; ds <= D_OPC3; end
                else begin dopc <= b; ds <= D_OPC2; end
              end
              D_OPC3: begin
                dopc <= b;
                idx <= idx + 1;
                ds <= D_OPC2;
              end
              D_OPC2: begin
                // the opcode is known: a ModRM byte, or straight to the immediate
                if (dec_modrm(m64, dmap, plus_base(dopc))) begin
                  has_modrm <= 1'b1;
                  ds <= D_MODRM;
                end else begin
                  dattr_t at;
                  at = dec_attr(m64, dmap, plus_base(dopc), 3'd0);
                  attr <= at;
                  dosz <= calc_osz(at.cls);
                  dasz <= asz_now;
                  imm_n <= imm_bytes(at.imm, calc_osz(at.cls));
                  ds <= D_IMM;
                end
              end
              D_MODRM: begin
                dattr_t at;
                logic [1:0] os;
                at = dec_attr(m64, dmap, plus_base(dopc), b[5:3]);
                os = calc_osz(at.cls);
                modrm <= b;
                idx <= idx + 1;
                attr <= at;
                dosz <= os;
                dasz <= asz_now;
                imm_n <= imm_bytes(at.imm, os);
                if (b[7:6] == 2'd3) ds <= D_IMM;
                else if (asz_now == 2'd0) begin
                  disp_n <= (b[7:6] == 2'd1) ? 3'd1 : (b[7:6] == 2'd2 || b[2:0] == 3'd6) ? 3'd2 : 3'd0;
                  ds <= D_DISP;
                end else if (b[2:0] == 3'd4) begin
                  has_sib <= 1'b1;
                  ds <= D_SIB;
                end else begin
                  disp_n <= (b[7:6] == 2'd1) ? 3'd1 : (b[7:6] == 2'd2 || b[2:0] == 3'd5) ? 3'd4 : 3'd0;
                  ds <= D_DISP;
                end
              end
              D_SIB: begin
                sib <= b;
                idx <= idx + 1;
                disp_n <= (modrm[7:6] == 2'd1) ? 3'd1 :
                          (modrm[7:6] == 2'd2 || (modrm[7:6] == 2'd0 && b[2:0] == 3'd5)) ? 3'd4 : 3'd0;
                ds <= D_DISP;
              end
              D_DISP: begin
                if (disp_i == disp_n) ds <= D_IMM;
                else begin
                  logic [63:0] d;
                  d = disp | (64'(b) << {disp_i, 3'b0});
                  // sign-extend the displacement as it completes
                  if (disp_i + 1 == disp_n) begin
                    case (disp_n)
                      3'd1: d = {{56{d[7]}}, d[7:0]};
                      3'd2: d = {{48{d[15]}}, d[15:0]};
                      default: d = {{32{d[31]}}, d[31:0]};
                    endcase
                  end
                  disp <= d;
                  disp_i <= disp_i + 1;
                  idx <= idx + 1;
                end
              end
              D_IMM: begin
                if (imm_i == imm_n) ds <= D_FINISH;
                else begin
                  logic [63:0] v;
                  v = imm_r | (64'(b) << {imm_i, 3'b0});
                  if (attr.imm == 4'd8 && imm_i + 2 >= imm_n) begin
                    // the far pointer's selector
                    imm2_r <= imm2_r | (64'(b) << {imm_i + 2 - imm_n, 3'b0});
                    v = imm_r;
                  end else if (attr.imm == 4'd8) begin
                    // the offset, zero-extended
                  end else if (attr.imm == 4'd9) begin
                    // moffs: an unsigned offset
                  end else if (imm_i + 1 == imm_n && !attr.immz) begin
                    case (imm_n)
                      4'd1: v = {{56{v[7]}}, v[7:0]};
                      4'd2: v = {{48{v[15]}}, v[15:0]};
                      4'd3: begin imm2_r <= {56'b0, v[23:16]}; v = {48'b0, v[15:0]}; end
                      4'd4: v = {{32{v[31]}}, v[31:0]};
                      default: ;
                    endcase
                  end
                  imm_r <= v;
                  imm_i <= imm_i + 1;
                  idx <= idx + 1;
                end
              end
              D_FINISH: begin
                logic [63:0] nr;
                logic [2:0] dseg;
                logic [63:0] e;
                logic [1:0] pk;
                logic [3:0] rr, rm;
                nr = rip + 64'(idx);
                if (mode != 2'd3) nr = (mode == 2'd2) ? {32'b0, nr[31:0]} : {48'b0, nr[15:0]};
                nrip <= nr;
                `ifdef V1_DEBUG $display("finish map=%0d opc=%h mode=%0d osz=%0d idx=%0d ds=%0d", dmap, dopc, mode, dosz, idx, ds); `endif
                e = calc_ea(nr, dseg);
                if (attr.moffs) begin
                  e = imm_r;
                  dseg = 3'd3;
                end
                ea <= e;
                easeg <= (pseg != 3'd7 && !(m64 && pseg < 3'd4)) ? pseg : dseg;
                rr = {rex_v & rex[2], modrm[5:3]};
                rm = attr.plus_r ? {rex_v & rex[0], dopc[2:0]} : {rex_v & rex[0], modrm[2:0]};
                dreg <= rr;
                drm <= rm;
                // F3/F2 select the form; a 66h selects it only where a form has
                // 66h as its mandatory prefix (ADCX, CLWB...), else it is osz
                pk = !attr.pfx_sens ? 2'd0 : (prep != 2'd0) ? prep : (attr.p66 && p66) ? 2'd3 : 2'd0;
                if (!attr.valid) upc <= UADDR_UD;
                else upc <= uentry(dmap == 2'd0 && dopc == 8'h90 && !(rex_v && rex[0]),
                                   mode, dmap, plus_base(dopc),
                                   modrm[5:3], pk, dosz, (has_modrm && modrm[7:6] != 2'd3) || attr.moffs,
                                   dasz, ssz);
                st <= S_EXEC;
              end
              default: ;
            endcase
          end
        end

        // ------------------------------------------------------------ execute
        S_EXEC: begin
          if (is_mem) begin
            // ---------------- memory micro-ops
            if (mchk.fault) raise(mchk.ss ? 8'd12 : 8'd13, 1'b1, 32'd0, 1'b0, 1'b0, 1'b0);
            else if (ms == M_IDLE || ms == M_BYTES) begin
              if (paging && !mtlb.hit)
                utrap(mla_cur, {2'b0, cpl == 2'd3, u.op == U_ST}, 1'b0);
              else begin
                logic [63:0] pa;
                pa = paging ? mtlb.pa : mla_cur;
                for (int i = 0; i < 4; i++) begin
                  logic [63:0] bp;
                  logic [1:0] rw, ln;
                  logic [63:0] blen;
                  bp = sp[8'(9 + i)];
                  rw = sp[SP_DR7[7:0]][16 + 4 * i +: 2];
                  ln = sp[SP_DR7[7:0]][18 + 4 * i +: 2];
                  blen = (ln == 2'd0) ? 1 : (ln == 2'd1) ? 2 : (ln == 2'd3) ? 4 : 8;
                  if (sp[SP_DR7[7:0]][2 * i +: 2] != 0 && (rw == 2'd3 || (rw == 2'd1 && u.op == U_ST)) &&
                      mla_cur < (bp & ~(blen - 1)) + blen && mla_cur + 64'(mn) > (bp & ~(blen - 1)))
                    db_data[i] <= 1'b1;
                end
                if (ms == M_IDLE && !maligned) begin
                  ms <= M_BYTES;
                  mb_i <= '0;
                  mdata <= {64'b0, vb};
                end else begin
                  bus_req <= 1'b1;
                  bus_we <= (u.op == U_ST);
                  bus_io <= 1'b0;
                  bus_addr <= pa;
                  bus_size <= (ms == M_BYTES) ? 3'd0 : 3'(u.sz);
                  bus_wdata <= (ms == M_BYTES) ? {120'b0, mdata[{mb_i, 3'b0} +: 8]} : {64'b0, vb};
                  if (ms == M_IDLE) mdata <= '0;
                  ms <= M_BUS;
                  if (u.op == U_ST) lb_v <= 1'b0;   // self-modifying code: refetch
                end
              end
            end else if (ms == M_BUS && bus_ack) begin
              bus_req <= 1'b0;
              if (bus_err) raise(8'd13, 1'b1, 32'd0, 1'b0, 1'b0, 1'b0);
              else if (maligned) begin
                if (u.op == U_LD) wr(u.dst, u.sz, bus_rdata[63:0] & szmask(u.sz));
                `ifdef V1_DEBUG if (in_trap) $display("  trap mem op=%0d addr=%h data=%h", u.op, bus_addr, bus_rdata[63:0]); `endif
                ms <= M_IDLE;
                upc <= upc + 1;
                ucount <= ucount + 1;
              end else begin
                logic [63:0] acc;
                acc = mdata[63:0];
                if (u.op == U_LD) acc[{mb_i, 3'b0} +: 8] = bus_rdata[7:0];
                mdata[63:0] <= acc;
                if (mb_i + 1 == mn) begin
                  if (u.op == U_LD) wr(u.dst, u.sz, acc);
                  ms <= M_IDLE;
                  upc <= upc + 1;
                  ucount <= ucount + 1;
                end else begin
                  mb_i <= mb_i + 1;
                  ms <= M_BYTES;
                end
              end
            end
          end else if (u.op == U_IN || u.op == U_OUT) begin
            if (ms == M_IDLE) begin
              bus_req <= 1'b1;
              bus_we <= (u.op == U_OUT);
              bus_io <= 1'b1;
              bus_addr <= {48'b0, va[15:0]};
              bus_size <= 3'(u.sz);
              bus_wdata <= {64'b0, vb};
              ms <= M_BUS;
            end else if (bus_ack) begin
              bus_req <= 1'b0;
              bus_io <= 1'b0;
              if (u.op == U_IN) wr(u.dst, u.sz, bus_rdata[63:0] & szmask(u.sz));
              ms <= M_IDLE;
              upc <= upc + 1;
              ucount <= ucount + 1;
            end
          end else begin
            // ---------------- one-cycle micro-ops
            logic [UADDR_W-1:0] next;
            logic hold;
            next = upc + 1;
            hold = 1'b0;
            `ifdef V1_DEBUG if (in_trap) $display("  trap upc=%0d op=%0d", upc, u.op); `endif
            ucount <= ucount + 1;
            unique case (u.op)
              U_NOP: ;
              U_JMP: next = u.imm[UADDR_W-1:0];
              U_JCC: if (cond(u.cc, w_flags)) next = u.imm[UADDR_W-1:0];
              U_JX:  if (va != 0) next = u.imm[UADDR_W-1:0];
              U_JU: begin
                logic c;
                case (u.cc)
                  4'd0: c = u_eq;
                  4'd1: c = !u_eq;
                  4'd2: c = u_ltu;
                  4'd3: c = !u_ltu;
                  4'd4: c = u_ltu || u_eq;
                  4'd5: c = !(u_ltu || u_eq);
                  4'd6: c = u_lts;
                  4'd7: c = !u_lts;
                  4'd8: c = u_lts || u_eq;
                  4'd9: c = !(u_lts || u_eq);
                  default: c = 1'b1;
                endcase
                if (c) next = u.imm[UADDR_W-1:0];
              end
              U_CMP: begin
                logic [63:0] am, bm, as, bs;
                am = va & szmask(u.sz);
                bm = vb & szmask(u.sz);
                as = (u.sz == 2'd3) ? va : (u.sz == 2'd2) ? {{32{va[31]}}, va[31:0]} :
                     (u.sz == 2'd1) ? {{48{va[15]}}, va[15:0]} : {{56{va[7]}}, va[7:0]};
                bs = (u.sz == 2'd3) ? vb : (u.sz == 2'd2) ? {{32{vb[31]}}, vb[31:0]} :
                     (u.sz == 2'd1) ? {{48{vb[15]}}, vb[15:0]} : {{56{vb[7]}}, vb[7:0]};
                u_eq <= am == bm;
                u_ltu <= am < bm;
                u_lts <= $signed(as) < $signed(bs);
              end
              U_UCALL: begin
                `ifdef V1_DEBUG $display("ucall %0d depth=%0d a0=%h a1=%h a2=%h", u.imm, udepth, ua[0], ua[1], ua[2]); `endif
                ustack[udepth] <= next;
                udepth <= udepth + 1;
                next = u.imm[UADDR_W-1:0];
              end
              U_URET: begin
                `ifdef V1_DEBUG $display("uret in_trap=%0d fetch=%0d upc_trap=%0d", in_trap, trap_fetch, upc_trap); `endif
                if (in_trap) begin
                  in_trap <= 1'b0;
                  if (trap_fetch) begin
                    st <= S_FETCH;
                    hold = 1'b1;
                  end else next = upc_trap;
                end else begin
                  next = ustack[udepth - 1];
                  udepth <= udepth - 1;
                end
              end
              U_END: begin
                hold = 1'b1;
                `ifdef V1_DEBUG $display("end upc=%0d rip=%h nrip=%h", upc, rip, nrip); `endif
                commit();
                rip <= nrip;
                if (!in_delivery) begin
                  rt_valid <= 1'b1;
                  rt_rip <= rip;
                  rt_len <= idx;
                  rt_bytes <= ibytes;
                  rt_uops <= ucount + 1;
                  sp[SP_UCOUNT[7:0]] <= sp[SP_UCOUNT[7:0]] + 64'(ucount) + 1;
                end
                in_delivery <= 1'b0;
                // RF is cleared when an instruction completes (Vol. 3A 19.3.1.1)
                if (!in_delivery && !flags_loaded) w_flags[16] <= 1'b0;
                // single-step (EFLAGS.TF at the start of the instruction) and data
                // breakpoints are traps after it (Vol. 3A 19.2.4, 19.3.1.5); not in
                // the shadow of MOV SS / POP SS
                if (!in_delivery && !shadow_set && (a_flags[8] || db_data != 0)) begin
                  sp[SP_DR6[7:0]] <= (sp[SP_DR6[7:0]] & ~64'h400F) | {49'b0, a_flags[8], 10'b0, db_data};
                  rt_valid <= 1'b1;
                  rt_rip <= rip;
                  rt_len <= idx;
                  rt_bytes <= ibytes;
                  rt_uops <= ucount + 1;
                  commit();
                  rip <= nrip;
                  raise(8'd1, 1'b0, 32'd0, 1'b1, 1'b0, 1'b0, 1'b1);
                  hold = 1'b1;
                end else begin
                // the shadow lasts until the next instruction completes (Intel SDM
                // Vol. 3A, 7.8.3 "Masking Exceptions and Interrupts When Switching Stacks")
                int_shadow <= shadow_set && !in_delivery;
                shadow_set <= 1'b0;
                start_decode();
                st <= halt_req ? S_HALT : S_FETCH;
                halt_req <= 1'b0;
                end
              end
              U_COMMIT: commit();
              U_RDS: wr(u.dst, 2'd3, (u.imm[7:0] == SP_MODE[7:0]) ?
                         {58'b0, cr0_pg, rf_vm, cs_d, cs_l, efer_lma, cr0_pe} :
                         (u.imm[7:0] == SP_CPL[7:0]) ? {62'b0, cpl} : sp[u.imm[7:0]]);
              U_WRS: begin
                `ifdef V1_DEBUG $display("  wrs %0d <= %h", u.imm[7:0], va); `endif
                wr_special(u.x == 4'd1 ? vb[7:0] : u.imm[7:0], va);
              end
              U_EXC: begin
                raise(u.imm[10] ? va[7:0] : u.imm[7:0], u.imm[8], va[31:0], u.imm[9],
                      u.imm[10] || u.imm[9], 1'b0);
                hold = 1'b1;
              end
              U_HLT: halt_req <= 1'b1;
              U_SHUTDOWN: begin
                st <= S_SHUTDOWN;
                shutdown_rip <= rip;
              end
              U_TLBW: begin
                tlb_v[tlb_next] <= 1'b1;
                tlb_vpn[tlb_next] <= va[63:12];
                tlb_ps[tlb_next] <= vc[1:0];
                tlb_ppn[tlb_next] <= {12'b0, vb[51:12]};
                tlb_w[tlb_next] <= vb[1];
                tlb_u[tlb_next] <= vb[2];
                tlb_d[tlb_next] <= vb[6];
                tlb_g[tlb_next] <= vb[8];
                tlb_nx[tlb_next] <= vb[63];
                tlb_pcid[tlb_next] <= sp[SP_CR3[7:0]][11:0];
                tlb_next <= tlb_next + 1;
              end
              U_TLBINV: begin
                for (int i = 0; i < NTLB; i++) begin
                  case (u.x)
                    4'd0: tlb_v[i] <= 1'b0;
                    4'd1: if (!tlb_g[i]) tlb_v[i] <= 1'b0;
                    4'd2: if (tlb_vpn[i][51:18] == va[63:30]) tlb_v[i] <= 1'b0;
                    4'd3: if (!tlb_g[i] && tlb_pcid[i] == vb[11:0]) tlb_v[i] <= 1'b0;
                    default: if (tlb_vpn[i][51:18] == va[63:30] && tlb_pcid[i] == vb[11:0]) tlb_v[i] <= 1'b0;
                  endcase
                end
                lb_v <= 1'b0;
              end
              U_LOCK, U_UNLOCK, U_CLFLUSH, U_SERIALIZE: ;
              U_RDRAND: begin
                logic [63:0] x;
                x = rng ^ (rng << 13);
                x = x ^ (x >> 7);
                x = x ^ (x << 17);
                rng <= x;
                wr(u.dst, u.sz, x & szmask(u.sz));
                w_flags[0] <= 1'b1;                 // CF: valid
                w_flags[2] <= 1'b0; w_flags[4] <= 1'b0; w_flags[6] <= 1'b0;
                w_flags[7] <= 1'b0; w_flags[11] <= 1'b0;
              end
              U_CRC32: wr(u.dst, 2'd2, alu_res);
              U_SETF: if (u.imm[5:0] != 6'd1) w_flags[u.imm[5:0]] <= va[0];
              U_DIVU, U_DIVS: begin
                if (alu_de) begin
                  raise(8'd0, 1'b0, 32'd0, 1'b0, 1'b0, 1'b0);
                  hold = 1'b1;
                end else begin
                  wr(u.dst, u.sz, alu_res);
                  rem_r <= alu_hi;
                end
              end
              default: begin
                if (u.op inside {U_ADD, U_ADC, U_SUB, U_SBB, U_AND, U_OR, U_XOR, U_ANDN, U_SHL,
                                 U_SHLD, U_SHRD,
                                 U_SHR, U_SAR, U_ROL, U_ROR, U_RCL, U_RCR, U_NEG, U_NOT, U_MOV,
                                 U_MULU, U_MULS, U_IMULT, U_ZEXT, U_SEXT, U_SETCC, U_BSF, U_BSR,
                                 U_TZCNT, U_LZCNT, U_POPCNT, U_BSWAP, U_SZP, U_GETF, U_SLICE,
                                 U_INSERT, U_KHI, U_SEL}) begin
                  logic [5:0] wm;
                  logic [63:0] f;
                  wm = u.fmask & alu_native;
                  f = w_flags;
                  if (wm[0]) f[0] = alu_fl[0];
                  if (wm[1]) f[2] = alu_fl[1];
                  if (wm[2]) f[4] = alu_fl[2];
                  if (wm[3]) f[6] = alu_fl[3];
                  if (wm[4]) f[7] = alu_fl[4];
                  if (wm[5]) f[11] = alu_fl[5];
                  w_flags <= f;
                  if (u.op != U_SZP && !alu_nowrite && u.dst != S_ZERO) begin
                    if (u.op == U_ZEXT || u.op == U_SEXT) wr(u.dst, u.x[1:0], alu_res);
                    else if (u.op inside {U_SLICE, U_INSERT, U_KHI}) wr(u.dst, 2'd3, alu_res);
                    else wr(u.dst, u.sz, alu_res);
                  end
                  if (alu_hi_we) hi_r <= alu_hi;
                end else begin
                  // a micro-op this core doesn't implement (vector/FP: later)
                  raise(8'd6, 1'b0, 32'd0, 1'b0, 1'b0, 1'b0);
                  hold = 1'b1;
                end
              end
            endcase
            if (!(hold || u.op == U_END || u.op == U_SHUTDOWN)) upc <= next;
          end
        end

        // ------------------------------------------------------------ halted
        S_HALT: begin
          if (nmi_pend || (intr_valid && w_flags[9])) st <= S_FETCH;
        end

        default: ;   // S_SHUTDOWN: until reset
      endcase

      // interrupts at an instruction boundary (Intel SDM Vol. 3A, 7.9 "Priority
      // Among Concurrent Exceptions and Interrupts")
      if (((st == S_FETCH && ds == D_PFX && idx == 0 && !in_delivery && !bus_req) || st == S_HALT)
          && !int_shadow) begin
        if (nmi_pend) begin
          nmi_pend <= 1'b0;
          raise(8'd2, 1'b0, 32'd0, 1'b1, 1'b0, 1'b1);
        end else if (intr_valid && w_flags[9]) begin
          intr_ack <= 1'b1;
          raise(intr_vector, 1'b0, 32'd0, 1'b1, 1'b0, 1'b1);
        end
      end
    end
  end
endmodule
