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
// Reset state: rtl/common/reset.toml (vibe86_reset_pkg, generated), after
// Intel SDM Vol. 3A 10.1.1 (Table 10-1) and AMD APM Vol. 2 Table 14-1. The
// ports are the die interface, rtl/common/pins.toml (vibe86_die_ports.svh);
// v1 uses 128 of the bus's 512 data bits.
module v1_core
  import v1_uop_pkg::*;
  import v1_ucode_pkg::*;
  import vibe86_reset_pkg::*;
  import vibe86_cpuid_pkg::*;
  import vibe86_platform_pkg::*;
(
  // the core's side of the on-die fabric (rtl/uncore/fabric.sv): one
  // transaction at a time, held until acknowledged; physical addresses
  input  logic         clk,
  input  logic         rst_n,
  output logic         bus_req,
  output logic         bus_we,
  output logic         bus_io,          // I/O space (IN/OUT): bus_addr is the port
  output logic [63:0]  bus_addr,
  output logic [2:0]   bus_size,        // log2 bytes: 0..4, 6 (an instruction cache line fill)
  output logic [127:0] bus_wdata,
  output logic         bus_smm,         // SMIACT#: the fabric decodes SMRAM for the core
  input  logic         bus_ack,
  input  logic         bus_err,
  input  logic [511:0] bus_rdata,       // a line fill uses all 64 bytes, else [127:0]
  // the fabric's write snoop: a DRAM line some master wrote (the core, the
  // debug module, device DMA), for the instruction cache's coherence
  input  logic         ic_inv,
  input  logic [51:6]  ic_inv_line,
  input  logic         ic_inv_core,     // the write was the core's own (the data cache has it)
  // interrupts (the die's sideband pins)
  input  logic         intr_valid,
  input  logic [7:0]   intr_vector,
  output logic         intr_ack,
  input  logic         nmi,
  input  logic         smi,
  // the local APIC (rtl/uncore/lapic.sv, D-017) and the multiprocessor straps
  input  logic         is_bsp,          // strap: the bootstrap processor (IA32_APIC_BASE.BSP)
  input  logic [31:0]  apic_id,         // strap: this core's APIC ID (CPUID 1, 0Bh)
  input  logic         init,            // INIT: reset; wait for SIPI unless the BSP
  input  logic         sipi,            // start-up IPI (in wait-for-SIPI) and its vector
  input  logic [7:0]   sipi_vec,
  output logic [63:0]  apic_base,       // IA32_APIC_BASE (the fabric and the APIC decode it)
  output logic [63:0]  tsc,
  output logic [31:0]  lapic_msr_addr,  // RDMSR/WRMSR of 800h-8FFh and 6E0h (the APIC micro-op)
  output logic         lapic_msr_wr,
  output logic [63:0]  lapic_msr_wdata,
  input  logic [63:0]  lapic_msr_rdata,
  input  logic         lapic_msr_rok,
  input  logic         lapic_msr_wok,
  output logic         cr8_wr,          // CR8 is the APIC's TPR[7:4] (SDM Vol. 3A 12.8.6.5)
  output logic [3:0]   cr8_wdata,
  input  logic [3:0]   cr8_rdata,
  input  logic         mce_valid,
  input  logic [1:0]   mce_type,
  input  logic [63:0]  mce_addr,
  // debug (the JTAG debug module, rtl/uncore/jtag_dm.sv; docs/jtag.md)
  input  logic         dbg_req,         // halt at the next instruction boundary; stay halted while 1
  input  logic         dbg_step,        // pulse while halted: run one instruction, halt again
  output logic         dbg_halted,
  input  logic [7:0]   dbg_reg_sel,     // 0-15 GPRs, 16 RIP, 17 RFLAGS, 80h+n special register n
  input  logic         dbg_reg_we,      // write while halted
  input  logic [63:0]  dbg_reg_wdata,
  output logic [63:0]  dbg_reg_rdata,
  // trace port (the die's pins)
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

  typedef enum logic [2:0] {S_FETCH, S_EXEC, S_HALT, S_SHUTDOWN, S_DEBUG, S_WFS} state_e;
  typedef enum logic [3:0] {D_PFX, D_ESC, D_OPC3, D_OPC2, D_MODRM, D_SIB, D_DISP, D_IMM, D_FINISH,
                            D_BUS, D_VEX1, D_VEX2, D_IC} dstate_e;

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
  logic        p66, p67;        // (LOCK, F0h, is accepted and ignored: v1 is one core)
  logic [1:0]  prep;            // 0 none, 1 F3, 2 F2
  logic [2:0]  pseg;            // segment override: 0-5, 7 none
  logic        rex_v;
  logic [3:0]  rex;             // W R X B
  logic [2:0]  dmap;            // 0 one-byte, 1 0F, 2 0F38, 3 0F3A; VEX: 5 0F, 6 0F38, 7 0F3A
  // VEX (Intel SDM Vol. 2A 2.3 "Intel Advanced Vector Extensions (Intel AVX)"):
  // the minimal decoder the VEX-encoded GPR instructions (BMI1, BMI2) need;
  // C4h/C5h are VEX in protected and long mode (not virtual-8086), and in
  // 16/32-bit code only with ModRM.mod = 11b (else LES/LDS). VEX.R/X/B/W go
  // to the REX state, VEX.pp is the form's mandatory prefix (not osz).
  logic        vex, vl, vex_bad, plock_seen;
  logic [3:0]  vvvv;
  logic [1:0]  vpp;             // 0 none, 1 66, 2 F3, 3 F2
  logic [7:0]  dopc;
  logic [7:0]  modrm, sib;
  logic        has_modrm, has_sib;
  logic [63:0] disp;
  logic [2:0]  disp_n, disp_i;
  logic [63:0] imm_r, imm2_r;
  logic [3:0]  imm_n, imm_i;
  // modrm and cls are used from the table lookup at decode, not from this latch
  /* verilator lint_off UNUSEDSIGNAL */
  dattr_t      attr;
  /* verilator lint_on UNUSEDSIGNAL */
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

  // the instruction cache (D-018): 64 KiB, direct-mapped, 1024 lines of 64
  // bytes, physically indexed and tagged, behind the line buffer. The data
  // and tag arrays are synchronous RAMs (one-cycle registered read); the
  // valid bits are flops. Coherent with every DRAM write through the
  // fabric's snoop (ic_inv), as x86 requires for self-modifying code and
  // DMA (Intel SDM Vol. 3A 11.6).
  localparam int IC_LINES = 1024;
  logic [511:0] ic_data [IC_LINES];
  logic [51:16] ic_tag  [IC_LINES];
  logic         ic_v    [IC_LINES];
  logic [511:0] ic_rdata;             // the line at ic_raddr, one cycle later
  logic [51:16] ic_rtag;
  logic         ic_rbypass;           // that read raced a write of the same line
  logic [9:0]   ic_raddr;
  logic         ic_we;                // write the filled line (and set its valid bit)
  logic [9:0]   ic_widx;
  logic [51:16] ic_wtag;
  logic [511:0] ic_wdata;
  logic [63:4]  ic_pa;                // the 16 bytes being looked up or filled
  logic         ic_fill;              // a line fill is outstanding
  logic         ic_poison;            // ... and its line was written meanwhile

  // the data cache (D-018): 256 KiB, direct-mapped, 4096 lines of 64 bytes,
  // physically indexed and tagged; write-through (memory is always current,
  // so device DMA reads need no snoop), updated on a write hit, allocated
  // on a read miss only. Device and debug-module writes invalidate through
  // the fabric's snoop; the core's own writes update the line instead.
  localparam int DC_LINES = 4096;
  logic [511:0] dc_data [DC_LINES];
  logic [51:18] dc_tag  [DC_LINES];
  logic         dc_v    [DC_LINES];
  logic [511:0] dc_rdata;
  logic [51:18] dc_rtag;
  logic         dc_rbypass;
  logic [11:0]  dc_raddr;
  logic         dc_we, dc_wtag_en;     // write bytes (a fill also writes the tag, sets valid)
  logic [11:0]  dc_widx;
  logic [51:18] dc_wtag;
  logic [511:0] dc_wdata;
  logic [63:0]  dc_wbe;
  logic [63:0]  dc_pa;                 // the access being looked up or filled
  logic         dc_fill, dc_poison;

  // execute
  logic [UADDR_W-1:0] upc, upc_trap;
  logic [UADDR_W-1:0] ustack [4];
  logic        in_delivery, in_trap, trap_fetch;
  logic [7:0]  ucount;
  logic        halt_req;
  logic        nmi_pend;
  logic        smi_pend;
  logic        init_pend;       // INIT from the local APIC, taken at an instruction boundary
  logic        mce_pend;        // a machine check from the uncore (MCERR): Intel SDM Vol. 3A 17.10
  // debug (JTAG debug module): halted in S_DEBUG at an instruction boundary;
  // a step runs one instruction (dbg_stepping) and halts after it retires
  logic        dbg_stepping, dbg_stepped;
  logic        dbg_resume_halt;   // S_DEBUG was entered from HLT: resume into HLT        // SMI# latched until taken (outside SMM; AMD APM Vol. 2 10.3)
  wire         in_smm = sp[SP_SMM[7:0]][0];
  logic [7:0]  deliv_vec;       // the event being delivered (for #DF)
  logic [3:0]  db_data;         // data breakpoints hit by this instruction (DR6.B0-B3)
  logic        flags_loaded;    // this instruction loaded RFLAGS (IRET, POPF): RF stays
  logic [63:0] rng;             // RDRAND/RDSEED: xorshift64 (Marsaglia, J. Stat. Softw. 8(14),
                                // 2003): NOT a cryptographic source
  logic        shadow_set;      // this instruction asked for an interrupt shadow
  logic        int_shadow;      // no interrupts at this boundary (MOV SS, POP SS, STI)

  // memory micro-op state
  typedef enum logic [2:0] {M_IDLE, M_BUS, M_BYTES, M_DC} mstate_e;
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
  // descriptor-cache attributes {G, D/B, L, AVL, P, DPL, S, Type} (bits 11:0)
  wire cs_l = sp[segi(1, 3)][9];
  wire cs_d = sp[segi(1, 3)][10];
  wire rf_vm = w_flags[17];
  logic [1:0] mode;   // 0 real, 1 16-bit protected / v86, 2 32-bit, 3 64-bit
  always_comb begin
    if (!cr0_pe) mode = 2'd0;
    else if (efer_lma && cs_l) mode = 2'd3;
    else if (rf_vm || !cs_d) mode = 2'd1;
    else mode = 2'd2;
  end
  wire ssz = (mode != 0) && sp[segi(2, 3)][10];
  assign bus_smm = sp[SP_SMM[7:0]][0];   // SMIACT#
  assign dbg_halted = (st == S_DEBUG);
  assign apic_base = sp[SP_APIC_BASE[7:0]];
  assign tsc = sp[SP_TSC[7:0]];
  // the APIC micro-op: the MSR number in a, the value in b (lapic.sv applies
  // the write at the clock edge of the cycle that executes it)
  assign lapic_msr_addr = va[31:0];
  assign lapic_msr_wdata = vb;
  assign lapic_msr_wr = st == S_EXEC && u.op == U_APIC && u.x == 4'd1 && lapic_msr_wok;
  // debug register reads: the architectural (committed) state
  always_comb begin
    if (dbg_reg_sel < 8'd16) dbg_reg_rdata = a_gpr[dbg_reg_sel[3:0]];
    else if (dbg_reg_sel == 8'd16) dbg_reg_rdata = rip;
    else if (dbg_reg_sel == 8'd17) dbg_reg_rdata = a_flags;
    else if (dbg_reg_sel[7]) dbg_reg_rdata = (dbg_reg_sel[6:0] == SP_CPL[6:0]) ? {62'b0, cpl} : sp[{1'b0, dbg_reg_sel[6:0]}];
    else dbg_reg_rdata = '0;
  end
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
  // RFLAGS is passed whole; a condition reads the arithmetic flags
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic cond(input logic [3:0] k, input logic [63:0] f);
  /* verilator lint_on UNUSEDSIGNAL */
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
    return hi8 ? {56'b0, w_gpr[{2'b0, i[1:0]}][15:8]} : w_gpr[i];
  endfunction

  // micro-op fields v1 doesn't execute yet (the vector operand fields)
  /* verilator lint_off UNUSEDSIGNAL */
  uop_t u;
  /* verilator lint_on UNUSEDSIGNAL */
  // the ROM index: UROM_N entries; upc never points past them
  assign u = uop_t'(UROM[upc[$clog2(UROM_N)-1:0]]);

  wire reg_hi8 = (u.sz == 2'd0) && !rex_v && dreg[3:2] == 2'b01;
  wire rm_hi8  = (u.sz == 2'd0) && !rex_v && drm[3:2] == 2'b01;

  function automatic logic [63:0] rd(input logic [6:0] s);
    if (s < 7'd16) return w_gpr[s[3:0]];
    if (s < 7'd20) return {56'b0, w_gpr[{2'b0, s[1:0]}][15:8]};
    if (s < S_T32) return tb[udepth][5'(s - S_T0)];
    if (s <= S_T63) return tt[5'(s - S_T32)];
    if (s >= S_A0 && s <= S_A3) return ua[2'(s - S_A0)];
    case (s)
      S_REG:   return reg_hi8 ? {56'b0, w_gpr[{2'b0, dreg[1:0]}][15:8]} : w_gpr[dreg];
      S_RM:    return rm_hi8 ? {56'b0, w_gpr[{2'b0, drm[1:0]}][15:8]} : w_gpr[drm];
      S_VVVV:  return w_gpr[vvvv];
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

  // ------------------------------------------------------------------ vectors (D-015)

  // a vector selector to its register-file entry: the decoder-relative
  // names, and x87 ST(i) = R[(TOP + i) mod 8] (FSW bits 13:11)
  function automatic logic [6:0] vphys(input logic [6:0] s);
    case (s)
      V_VREG:  return {3'b0, dreg};
      V_VRM:   return {3'b0, drm};
      V_VVVV:  return 7'd0;                       // VEX/EVEX.vvvv: with the VEX decoder
      V_VIS4:  return {3'b0, imm_r[7:4]};
      V_KREG:  return 7'd56 + 7'(modrm[5:3]);
      V_KRM:   return 7'd56 + 7'(modrm[2:0]);
      V_KVVVV: return 7'd56;
      V_KAAA:  return 7'd56;
      default:
        if (s >= V_ST0 && s <= V_ST7)
          return V_ST0 + 7'(3'(s - V_ST0) + sp[SP_FSW[7:0]][13:11]);
        else return s;
    endcase
  endfunction

  logic [511:0] vrda, vwd, vwm;
  logic         vwe, vcommit, vrollback;
  logic [6:0]   vwa;
  v1_vrf vrf (.clk, .rst_n, .ra(vphys(u.op == U_VGET ? u.b : u.a)), .rda(vrda),
              .we(vwe), .wa(vwa), .wd(vwd), .wm(vwm),
              .commit(vcommit), .rollback(vrollback));

  // the write of this clock's vector micro-op (one clock: VSET, VZUP, VMOVR)
  always_comb begin
    logic [9:0] off;
    vwe = 1'b0;
    vwa = vphys(u.dst);
    vwd = '0;
    vwm = '0;
    off = 10'(vb + 64'(u.imm));
    if (st == S_EXEC && !is_mem) begin
      case (u.op)
        U_VSET: begin
          vwe = 1'b1;
          vwd = {448'b0, va} << off;
          vwm = {448'b0, szmask(u.sz)} << off;
        end
        U_VZUP: begin
          vwe = 1'b1;
          vwm = ~((512'd1 << u.imm[9:0]) - 512'd1);
          if (u.imm[9]) vwm = '0;                 // imm = 512: nothing above
        end
        U_VMOVR: begin
          vwe = 1'b1;
          vwd = vrda;
          vwm = '1;
        end
        default: ;
      endcase
    end
  end

  // ------------------------------------------------------------------ SSE/AVX floating point
  // one scalar operation per micro-op (lane-serial vectors, D-015) on
  // v1_fp_pkg; exceptions of the instruction accumulate in fpx until FPCHK
  logic [63:0] fp_res;
  logic [5:0]  fp_x;
  logic [5:0]  fpx;                               // this instruction's MXCSR exceptions
  logic [2:0]  comi_zpc;                          // (U)COMIS: ZF, PF, CF
  always_comb begin
    logic [1:0] rm;
    logic daz, ftz;
    v1_fp_pkg::fres_t r;
    logic [5:0] cx;
    logic c;
    logic [3:0] op;
    logic un, lt, eq;
    /* verilator lint_off UNUSEDSIGNAL */
    logic [5:0] dx;                               // the UNORD/EQ probes' flags: unused
    /* verilator lint_on UNUSEDSIGNAL */
    op = '0;
    c = 1'b0;
    un = 1'b0;
    lt = 1'b0;
    eq = 1'b0;
    dx = '0;
    rm = (u.rnd < 3'd4) ? u.rnd[1:0] : sp[SP_MXCSR[7:0]][14:13];
    daz = sp[SP_MXCSR[7:0]][6];
    ftz = sp[SP_MXCSR[7:0]][15];
    r = '0;
    cx = '0;
    comi_zpc = '0;
    fp_res = '0;
    fp_x = '0;
    if (u.op inside {U_FADD, U_FSUB, U_FMUL, U_FDIV, U_FSQRT, U_FMA, U_FMIN, U_FMAX}) begin
      case (u.op)
        U_FADD: op = 4'd0;
        U_FSUB: op = 4'd1;
        U_FMUL: op = 4'd2;
        U_FDIV: op = 4'd3;
        U_FSQRT: op = 4'd4;
        U_FMA: op = 4'd5;
        U_FMIN: op = 4'd6;
        default: op = 4'd7;
      endcase
      r = v1_fp_pkg::fp_arith(op, {16'b0, va}, {16'b0, vb}, {16'b0, vc}, u.et, rm, daz, ftz, 0,
                              u.x[1:0]);
      fp_res = r.v[63:0];
      fp_x = r.x;
    end else if (u.op == U_FCMP && u.x == 4'd0) begin
      c = v1_fp_pkg::fp_cmp({16'b0, va}, {16'b0, vb}, u.et, u.imm[4:0], daz, cx);
      fp_res = {63'b0, c};
      fp_x = cx;
    end else if (u.op == U_FCMP) begin
      // COMISS/COMISD (x = 1, signaling: QNaN raises IE) and UCOMISS/UCOMISD
      // (x = 2, quiet): unordered ZF PF CF = 111, < 001, = 100, > 000
      un = v1_fp_pkg::fp_cmp({16'b0, va}, {16'b0, vb}, u.et, 5'd3, daz, dx);
      eq = v1_fp_pkg::fp_cmp({16'b0, va}, {16'b0, vb}, u.et, 5'd0, daz, dx);
      lt = v1_fp_pkg::fp_cmp({16'b0, va}, {16'b0, vb}, u.et, u.x == 4'd1 ? 5'd1 : 5'd17, daz, cx);
      comi_zpc = un ? 3'b111 : lt ? 3'b001 : eq ? 3'b100 : 3'b000;
      fp_x = cx;
    end else if (u.op == U_FCVT) begin
      r = v1_fp_pkg::fp_convert({16'b0, va}, u.x, u.et, rm, daz, ftz, 0);
      fp_res = r.v[63:0];
      fp_x = r.x;
    end else if (u.op == U_FCVTI || u.op == U_FCVTIT) begin
      // float (et) -> integer of size sz; x[0]: unsigned
      r = v1_fp_pkg::fp_to_int({16'b0, va}, u.et, 8 << u.sz, !u.x[0],
                               u.op == U_FCVTIT ? 2'd3 : rm, daz);
      fp_res = r.v[63:0];
      fp_x = r.x;
    end else if (u.op == U_ICVTF) begin
      // integer of size sz (x[0]: unsigned) -> float (et)
      r = v1_fp_pkg::fp_from_int(va, 8 << u.sz, !u.x[0], u.et, rm, 0);
      fp_res = r.v[63:0];
      fp_x = r.x;
    end else if (u.op == U_FRND) begin
      // imm[2]: MXCSR.RC, else imm[1:0]; imm[3]: suppress precision (ROUNDPS)
      r = v1_fp_pkg::fp_round_int({16'b0, va}, u.et, u.imm[2] ? sp[SP_MXCSR[7:0]][14:13] : u.imm[1:0],
                                  u.imm[3], daz);
      fp_res = r.v[63:0];
      fp_x = r.x;
    end
  end

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
    // the checks read P, S, type and D/B; AVL, L, DPL and A are not limit/type checks
    /* verilator lint_off UNUSEDSIGNAL */
    logic [63:0] base, limit, sattr;
    /* verilator lint_on UNUSEDSIGNAL */
    logic [63:0] last;
    r = '0;
    r.ss = (s == 4'd2);
    if (s >= 4'd6) begin
      r.la = off;
      return r;
    end
    base = sp[segi(int'(s), 1)];
    limit = sp[segi(int'(s), 2)];
    sattr = sp[segi(int'(s), 3)];
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
      if (!sattr[7]) r.fault = 1'b1;                       // not present / null selector
      else if (sattr[4] && sattr[3]) begin                   // code segment
        if (wr || !sattr[1]) r.fault = 1'b1;               // not writable; execute-only
      end else if (sattr[4] && wr && !sattr[1]) r.fault = 1'b1;   // read-only data
    end
    if (sattr[4] && !sattr[3] && sattr[2]) begin              // expand-down data
      if (off <= limit || last > (sattr[10] ? 64'hFFFF_FFFF : 64'hFFFF)) r.fault = 1'b1;
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
  wire [63:0] fetch_pa = cr0_pg ? {ftlb.pa[63:4], 4'b0} : {fetch_la[63:4], 4'b0};
  // what the instruction cache keeps: DRAM below TOLM except the legacy
  // region (SMRAM and the PAM shadow change meaning under the core), DRAM
  // hoisted above 4 GiB up to the 64-bit PCI window, and the flash
  function automatic logic ic_cacheable(input logic [63:0] pa);
    return (pa < PLAT_MAP_TOLM && !(pa >= 64'hA0000 && pa < 64'h100000)) ||
           (pa >= PLAT_MAP_FLASH_BASE && pa < 64'h1_0000_0000) ||
           (pa >= PLAT_MAP_HOIST_BASE && pa < PLAT_MAP_PCI_MEM64_BASE);
  endfunction
  assign ic_raddr = fetch_pa[15:6];
  always_ff @(posedge clk) begin
    if (ic_we) begin
      ic_data[ic_widx] <= ic_wdata;
      ic_tag[ic_widx] <= ic_wtag;
    end
    ic_rdata <= ic_data[ic_raddr];
    ic_rtag <= ic_tag[ic_raddr];
    ic_rbypass <= ic_we && ic_widx == ic_raddr;
  end
  always_ff @(posedge clk) begin
    if (dc_we) begin
      for (int b = 0; b < 64; b++)
        if (dc_wbe[b]) dc_data[dc_widx][8 * b +: 8] <= dc_wdata[8 * b +: 8];
      if (dc_wtag_en) dc_tag[dc_widx] <= dc_wtag;
    end
    dc_rdata <= dc_data[dc_raddr];
    dc_rtag <= dc_tag[dc_raddr];
    dc_rbypass <= dc_we && dc_widx == dc_raddr;
  end

  wire        m64 = (mode == 2'd3);
  // instruction breakpoints matching this instruction's first byte
  logic [3:0] ibp_hit;
  always_comb
    for (int i = 0; i < 4; i++)
      ibp_hit[i] = sp[SP_DR7[7:0]][2 * i +: 2] != 0 && sp[SP_DR7[7:0]][16 + 4 * i +: 2] == 2'd0 &&
                   sp[8'(9 + i)] == fetch_la;
  // decode states that consume the next instruction byte
  wire need_byte = ds == D_PFX || ds == D_ESC || ds == D_OPC3 || ds == D_MODRM || ds == D_SIB ||
                   ds == D_VEX1 || ds == D_VEX2 ||
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
  // the data cache: looked up with this access's physical address, answered
  // in M_DC; a load's data from a hit, a line fill, or the bus
  assign dc_raddr = paging ? mtlb.pa[17:6] : mla_cur[17:6];
  wire        dc_hit = dc_v[dc_pa[17:6]] && !dc_rbypass && dc_rtag == dc_pa[51:18];
  wire [511:0] mem_line = (ms == M_DC) ? dc_rdata : bus_rdata;
  wire [63:0] mem_rd = (ms == M_DC || dc_fill) ? 64'(mem_line >> {dc_pa[5:0], 3'b0}) : bus_rdata[63:0];
  tlbres_t ftlb;
  assign ftlb = tlb_look({fetch_la[63:4], 4'b0}, 1'b0, 1'b1, cpl == 2'd3);

  // ------------------------------------------------------------------ sequential

  task automatic commit();
    for (int i = 0; i < 16; i++) a_gpr[i] <= w_gpr[i];
    a_flags <= w_flags;
    vcommit <= 1'b1;
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
      vrollback <= 1'b1;
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
    if (s < 7'd16 || s == S_REG || s == S_RM || s == S_VVVV) begin
      logic [3:0] i;
      logic hi8;
      i = (s == S_REG) ? dreg : (s == S_RM) ? drm : (s == S_VVVV) ? vvvv : s[3:0];
      hi8 = (s == S_REG) ? reg_hi8 : (s == S_RM) ? rm_hi8 : 1'b0;
      if (hi8) w_gpr[{2'b0, i[1:0]}][15:8] <= v[7:0];
      else case (sz)
        2'd0: w_gpr[i][7:0] <= v[7:0];
        2'd1: w_gpr[i][15:0] <= v[15:0];
        2'd2: w_gpr[i] <= {32'b0, v[31:0]};
        default: w_gpr[i] <= v;
      endcase
    end else if (s < 7'd20) begin
      w_gpr[{2'b0, s[1:0]}][15:8] <= v[7:0];
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
    if (n == SP_CR8[7:0]) begin
      cr8_wr <= 1'b1;
      cr8_wdata <= v[3:0];
    end
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
    // architectural state: rtl/common/reset.toml
    for (int i = 0; i < 16; i++) begin
      a_gpr[i] <= RESET_GPR[i];
      w_gpr[i] <= RESET_GPR[i];
    end
    a_flags <= RESET_RFLAGS;
    w_flags <= RESET_RFLAGS;
    rip <= RESET_RIP;
    nrip <= RESET_RIP;
    for (int i = 0; i < 256; i++) sp[i] <= '0;
`include "vibe86_reset_apply.svh"
    for (int s = 0; s < 8; s++) begin
      sp[segi(s, 0)] <= RESET_SEG_SEL[s];
      sp[segi(s, 1)] <= RESET_SEG_BASE[s];
      sp[segi(s, 2)] <= RESET_SEG_LIMIT[s];
      sp[segi(s, 3)] <= RESET_SEG_ATTR[s];
    end
    rng <= 64'h9E37_79B9_7F4A_7C15;
    udepth <= '0;
    db_data <= '0;
    deliv_vec <= '0;
    for (int i = 0; i < NTLB; i++) tlb_v[i] <= 1'b0;
    tlb_next <= '0;
    lb_v <= 1'b0;
    for (int i = 0; i < IC_LINES; i++) ic_v[i] <= 1'b0;
    ic_we <= 1'b0;
    ic_fill <= 1'b0;
    ic_poison <= 1'b0;
    for (int i = 0; i < DC_LINES; i++) dc_v[i] <= 1'b0;
    dc_we <= 1'b0;
    dc_fill <= 1'b0;
    dc_poison <= 1'b0;
    st <= S_FETCH;
    ds <= D_PFX;
    ms <= M_IDLE;
    idx <= '0;
    in_delivery <= 1'b0;
    in_trap <= 1'b0;
    halt_req <= 1'b0;
    nmi_pend <= 1'b0;
    smi_pend <= 1'b0;
    mce_pend <= 1'b0;
    shadow_set <= 1'b0;
    int_shadow <= 1'b0;
    ucount <= '0;
    upc <= '0;
  endtask

  task automatic start_decode();
    idx <= '0;
    fpx <= '0;
    p66 <= 1'b0; p67 <= 1'b0; prep <= 2'd0; pseg <= 3'd7;
    rex_v <= 1'b0; rex <= '0;
    dmap <= '0;
    vex <= 1'b0; vl <= 1'b0; vex_bad <= 1'b0; plock_seen <= 1'b0; vvvv <= '0; vpp <= '0;
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
      vcommit <= 1'b0;
      vrollback <= 1'b0;
      fpx <= '0;
      reset_state();
      bus_req <= 1'b0;
      rt_valid <= 1'b0;
      ev_valid <= 1'b0;
      intr_ack <= 1'b0;
      shutdown <= 1'b0;
      halted <= 1'b0;
      shutdown_rip <= '0;
      cr8_wr <= 1'b0;
      cr8_wdata <= '0;
      init_pend <= 1'b0;
      if (!is_bsp) begin
        // an application processor: not the BSP, and it waits for SIPI (SDM
        // Vol. 3A 10.4 "Multiple-Processor (MP) Initialization")
        sp[SP_APIC_BASE[7:0]][8] <= 1'b0;
        st <= S_WFS;
      end
      dbg_stepping <= 1'b0;
      dbg_stepped <= 1'b0;
      dbg_resume_halt <= 1'b0;
      start_decode();
    end else begin
      vcommit <= 1'b0;
      vrollback <= 1'b0;
      rt_valid <= 1'b0;
      ev_valid <= 1'b0;
      intr_ack <= 1'b0;
      sp[SP_TSC[7:0]] <= sp[SP_TSC[7:0]] + 1;
      if (nmi) nmi_pend <= 1'b1;
      if (smi) smi_pend <= 1'b1;
      if (init) init_pend <= 1'b1;
      cr8_wr <= 1'b0;
      if (mce_valid && mce_type == 2'd2) begin
        // an uncorrected error (type 2); corrected ones (1) are only counted
        // by the uncore. The address is kept for software (special MC_ADDR)
        mce_pend <= 1'b1;
        sp[SP_MC_ADDR[7:0]] <= mce_addr;
      end
      shutdown <= (st == S_SHUTDOWN);
      halted <= (st == S_HALT || st == S_WFS);   // an AP waiting for SIPI is idle too

      case (st)
        // ------------------------------------------------------------ decode
        S_FETCH: begin
          if (ds == D_BUS) begin
            if (bus_ack) begin
              bus_req <= 1'b0;
              ic_fill <= 1'b0;
              if (bus_err) raise(8'd13, 1'b1, 32'd0, 1'b0, 1'b0, 1'b0);
              else begin
                lb_v <= 1'b1;
                if (ic_fill) begin
                  // a line fill: keep it unless a write hit it meanwhile
                  lb_data <= bus_rdata[{ic_pa[5:4], 7'b0} +: 128];
                  if (!ic_poison) begin
                    ic_we <= 1'b1;
                    ic_widx <= ic_pa[15:6];
                    ic_wtag <= ic_pa[51:16];
                    ic_wdata <= bus_rdata;
                  end
                end else lb_data <= bus_rdata[127:0];
                ds <= dstate_e'(mdata[3:0]);   // resume
              end
            end
          end else if (ds == D_IC) begin
            // the cache's answer for ic_pa (read in the cycle before)
            if (ic_v[ic_pa[15:6]] && !ic_rbypass && ic_rtag == ic_pa[51:16]) begin
              lb_v <= 1'b1;
              lb_data <= ic_rdata[{ic_pa[5:4], 7'b0} +: 128];
              ds <= dstate_e'(mdata[3:0]);
            end else begin
              bus_req <= 1'b1;
              bus_we <= 1'b0;
              bus_io <= 1'b0;
              bus_size <= 3'd6;
              bus_addr <= {ic_pa[63:6], 6'b0};
              ic_fill <= 1'b1;
              ic_poison <= 1'b0;
              ds <= D_BUS;
            end
          end else if (ds == D_PFX && idx == 0 && !in_delivery && !w_flags[16] && ibp_hit != 0) begin
            sp[SP_DR6[7:0]] <= (sp[SP_DR6[7:0]] & ~64'hF) | {60'b0, ibp_hit};
            raise(8'd1, 1'b0, 32'd0, 1'b0, 1'b0, 1'b0);
          end else if (need_byte && !fetch_hit) begin
            `ifdef V1_DEBUG $display("fetch la=%h ds=%0d hit=%0d pa=%h", fetch_la, ds, ftlb.hit, ftlb.pa); `endif
            // fetch the 16-byte line holding the next byte
            if (idx == 4'd15 || fetch_limit_bad) raise(8'd13, 1'b1, 32'd0, 1'b0, 1'b0, 1'b0);
            else if (cr0_pg && !ftlb.hit) utrap({fetch_la[63:4], 4'b0}, 4'b0100 | {2'b0, cpl == 2'd3, 1'b0}, 1'b1);
            else if (ic_cacheable(fetch_pa)) begin
              // look the line up in the instruction cache (ic_raddr = fetch_pa)
              ic_pa <= fetch_pa[63:4];
              lb_tag <= fetch_la[63:4];
              mdata[3:0] <= 4'(ds);
              ds <= D_IC;
            end else begin
              bus_req <= 1'b1;
              bus_we <= 1'b0;
              bus_io <= 1'b0;
              bus_size <= 3'd4;
              bus_addr <= fetch_pa;
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
                    8'hF0: plock_seen <= 1'b1;
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
                end else if ((b == 8'hC4 || b == 8'hC5) && cr0_pe && !rf_vm) begin
                  // a VEX prefix, or LES/LDS (decided by the next byte)
                  dopc <= b;
                  idx <= idx + 1;
                  ds <= D_VEX1;
                end else if (b == 8'h0F) begin
                  dmap <= 3'd1;
                  idx <= idx + 1;
                  ds <= D_ESC;
                end else begin
                  dmap <= 3'd0;
                  dopc <= b;
                  idx <= idx + 1;
                  ds <= D_OPC2;
                end
              end
              D_ESC: begin
                idx <= idx + 1;
                if (b == 8'h38) begin dmap <= 3'd2; ds <= D_OPC3; end
                else if (b == 8'h3A) begin dmap <= 3'd3; ds <= D_OPC3; end
                else begin dopc <= b; ds <= D_OPC2; end
              end
              D_VEX1: begin
                if (!m64 && b[7:6] != 2'b11) begin
                  // LES/LDS: this byte is their ModRM (not consumed here)
                  dmap <= 3'd0;
                  ds <= D_OPC2;
                end else begin
                  // VEX with a 66h/F2h/F3h/REX/LOCK prefix before it is #UD
                  // (SDM Vol. 2A 2.3.2); R/X/B/vvvv are inverted
                  vex <= 1'b1;
                  vex_bad <= p66 || prep != 2'd0 || rex_v || plock_seen;
                  rex_v <= 1'b1;
                  idx <= idx + 1;
                  if (dopc == 8'hC5) begin
                    rex <= {1'b0, m64 & ~b[7], 2'b00};
                    vvvv <= ~b[6:3] & {m64, 3'b111};
                    vl <= b[2];
                    vpp <= b[1:0];
                    dmap <= 3'd5;
                    ds <= D_OPC3;
                  end else begin
                    rex <= {1'b0, m64 & ~b[7], m64 & ~b[6], m64 & ~b[5]};
                    if (b[4:0] == 5'd1) dmap <= 3'd5;
                    else if (b[4:0] == 5'd2) dmap <= 3'd6;
                    else if (b[4:0] == 5'd3) dmap <= 3'd7;
                    else vex_bad <= 1'b1;          // a reserved map
                    ds <= D_VEX2;
                  end
                end
              end
              D_VEX2: begin
                rex[3] <= m64 & b[7];              // VEX.W (ignored outside 64-bit mode)
                vvvv <= ~b[6:3] & {m64, 3'b111};
                vl <= b[2];
                vpp <= b[1:0];
                idx <= idx + 1;
                ds <= D_OPC3;
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
                    imm2_r <= imm2_r | (64'(b) << {4'(imm_i + 4'd2 - imm_n), 3'b0});
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
                logic [2:0] kdig;
                logic       crud;
                nr = rip + 64'(idx);
                if (mode != 2'd3) nr = (mode == 2'd2) ? {32'b0, nr[31:0]} : {48'b0, nr[15:0]};
                nrip <= nr;
                `ifdef V1_DEBUG $display("finish map=%0d opc=%h mode=%0d osz=%0d idx=%0d ds=%0d", dmap, dopc, mode, dosz, idx, ds); `endif
                e = calc_ea(nr, dseg);
                // INVD, WBINVD (and WBNOINVD), CLFLUSH(OPT): the instruction
                // cache is flushed as they are decoded (invisible if they fault)
                if (dmap == 3'd1 && (dopc == 8'h08 || dopc == 8'h09 ||
                    (dopc == 8'hAE && modrm[5:3] == 3'd7 && modrm[7:6] != 2'd3)))
                  begin
                    for (int i = 0; i < IC_LINES; i++) ic_v[i] <= 1'b0;
                    for (int i = 0; i < DC_LINES; i++) dc_v[i] <= 1'b0;
                  end
                // without a memory ModRM operand the default segment is DS
                // (string sources, XLAT: Intel SDM Vol. 1 3.7.5); modrm
                // still holds the previous instruction's byte, whose
                // BP/SP-based forms would select SS
                if (!has_modrm || modrm[7:6] == 2'd3) dseg = 3'd3;
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
                // VEX.pp is the mandatory prefix: 01 66h, 10 F3h, 11 F2h
                if (vex) pk = (vpp == 2'd1) ? 2'd3 : (vpp == 2'd2) ? 2'd1 : (vpp == 2'd3) ? 2'd2 : 2'd0;
                if (!attr.valid || vex_bad || (vex && attr.vvvv0 && vvvv != 4'd0)) upc <= UADDR_UD;
                // MOV CR/DR: REX.R extends ModRM.reg (Intel SDM Vol. 2B "MOV--
                // Move to/from Control Registers"): REX.R + reg 0 is CR8, keyed
                // as reg 1 (ucc's forms.py: CR1 doesn't exist); CR1, CR9-CR15
                // and DR8-DR15 are #UD
                kdig = modrm[5:3];
                crud = 1'b0;
                if (dmap == 2'd1 && (dopc == 8'h20 || dopc == 8'h22)) begin
                  if (rex_v && rex[2]) begin
                    if (modrm[5:3] == 3'd0) kdig = 3'd1;
                    else crud = 1'b1;
                  end else if (modrm[5:3] == 3'd1) crud = 1'b1;
                end
                else upc <= uentry(dmap == 3'd0 && dopc == 8'h90 && !(rex_v && rex[0]),
                                   mode, dmap, vl, plus_base(dopc),
                                   kdig, pk, dosz, (has_modrm && modrm[7:6] != 2'd3) || attr.moffs,
                                   dasz, ssz, modrm[2:0]);
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
                  // the request; a cacheable one asks the data cache first
                  // (dc_raddr is this access's index now, the answer in M_DC)
                  bus_req <= !ic_cacheable(pa);
                  bus_we <= (u.op == U_ST);
                  bus_io <= 1'b0;
                  bus_addr <= pa;
                  bus_size <= (ms == M_BYTES) ? 3'd0 : 3'(u.sz);
                  bus_wdata <= (ms == M_BYTES) ? {120'b0, mdata[7'({mb_i, 3'b0}) +: 8]} : {64'b0, vb};
                  if (ms == M_IDLE && u.op == U_LD) mdata <= '0;
                  dc_pa <= pa;
                  ms <= ic_cacheable(pa) ? M_DC : M_BUS;
                  if (u.op == U_ST) lb_v <= 1'b0;   // self-modifying code: refetch
                end
              end
            end else if (ms == M_DC && !(u.op == U_LD && dc_hit)) begin
              if (u.op == U_ST) begin
                // write through; a hit updates the line's bytes too
                if (dc_hit) begin
                  dc_we <= 1'b1;
                  dc_wtag_en <= 1'b0;
                  dc_widx <= dc_pa[17:6];
                  dc_wdata <= 512'(bus_wdata[63:0]) << {dc_pa[5:0], 3'b0};
                  dc_wbe <= (maligned ? (64'(1) << mn) - 64'd1 : 64'd1) << dc_pa[5:0];
                end
                bus_req <= 1'b1;
              end else begin
                // a read miss: fill the line
                bus_req <= 1'b1;
                bus_size <= 3'd6;
                bus_addr <= {dc_pa[63:6], 6'b0};
                dc_fill <= 1'b1;
                dc_poison <= 1'b0;
              end
              ms <= M_BUS;
            end else if ((ms == M_BUS && bus_ack) || ms == M_DC) begin
              bus_req <= 1'b0;
              if (dc_fill) begin
                dc_fill <= 1'b0;
                if (!bus_err && !dc_poison) begin
                  dc_we <= 1'b1;
                  dc_wtag_en <= 1'b1;
                  dc_widx <= dc_pa[17:6];
                  dc_wtag <= dc_pa[51:18];
                  dc_wdata <= bus_rdata;
                  dc_wbe <= '1;
                end
              end
              if (ms == M_BUS && bus_err) raise(8'd13, 1'b1, 32'd0, 1'b0, 1'b0, 1'b0);
              else if (maligned) begin
                if (u.op == U_LD) wr(u.dst, u.sz, mem_rd & szmask(u.sz));
                `ifdef V1_DEBUG if (in_trap) $display("  trap mem op=%0d addr=%h data=%h", u.op, bus_addr, bus_rdata[63:0]); `endif
                ms <= M_IDLE;
                upc <= upc + 1;
                ucount <= ucount + 1;
              end else begin
                logic [63:0] acc;
                acc = mdata[63:0];
                if (u.op == U_LD) acc[6'({mb_i, 3'b0}) +: 8] = mem_rd[7:0];
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
                if (dbg_stepping) dbg_stepped <= 1'b1;
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
                         (u.imm[7:0] == SP_CPL[7:0]) ? {62'b0, cpl} :
                         (u.imm[7:0] == SP_CR8[7:0]) ? {60'b0, cr8_rdata} : sp[u.imm[7:0]]);
              U_APIC: begin
                // the local APIC's MSRs (x2APIC 800h-8FFh, IA32_TSC_DEADLINE):
                // read (x = 0) into dst, or write b (x = 1, lapic_msr_wr);
                // an illegal access is #GP(0) (SDM Vol. 3A 12.12.1.2)
                if (u.x == 4'd0 ? !lapic_msr_rok : !lapic_msr_wok) begin
                  raise(8'd13, 1'b1, 32'd0, 1'b0, 1'b0, 1'b0);
                  hold = 1'b1;
                end else if (u.x == 4'd0) wr(u.dst, 2'd3, lapic_msr_rdata);
              end
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
              // ---- vectors (D-015): the writes go through the register file's port
              U_VGET: begin
                logic [63:0] x;
                x = 64'(vrda >> 10'(va + 64'(u.imm)));
                wr(u.dst, u.sz, x & szmask(u.sz));
              end
              U_VSET, U_VZUP, U_VMOVR: ;
              U_FADD, U_FSUB, U_FMUL, U_FDIV, U_FSQRT, U_FMA, U_FMIN, U_FMAX, U_FCVT, U_FCVTI,
              U_FCVTIT, U_ICVTF, U_FRND: begin
                wr(u.dst, u.sz, fp_res & szmask(u.sz));
                fpx <= fpx | fp_x;
              end
              U_FCMP: begin
                if (u.x == 4'd0) wr(u.dst, u.sz, fp_res);
                else begin
                  w_flags[6] <= comi_zpc[2];
                  w_flags[2] <= comi_zpc[1];
                  w_flags[0] <= comi_zpc[0];
                  w_flags[11] <= 1'b0; w_flags[7] <= 1'b0; w_flags[4] <= 1'b0;
                end
                fpx <= fpx | fp_x;
              end
              U_FPCHK: begin
                // the instruction's exceptions against MXCSR's masks (bits 12:7)
                // (the flags are set either way: Intel SDM Vol. 1 11.5.2)
                sp[SP_MXCSR[7:0]] <= sp[SP_MXCSR[7:0]] | {58'b0, fpx};
                if ((fpx & ~sp[SP_MXCSR[7:0]][12:7]) != 6'd0) begin
                  raise(sp[SP_CR4[7:0]][10] ? 8'd19 : 8'd6, 1'b0, 32'd0, 1'b0, 1'b0, 1'b0);
                  hold = 1'b1;
                end
                fpx <= '0;
              end
              U_CPUID: begin
                // rtl/common/cpuid.toml (vibe86_cpuid_pkg) plus the dynamic fields
                logic [127:0] r;
                r = cpuid_v1(w_gpr[0][31:0], w_gpr[1][31:0]);
                if (w_gpr[0][31:0] == 32'd1) begin
                  r[63:56] = apic_id[7:0];                  // EBX[31:24]: initial APIC ID
                  r[64 + 27] = sp[SP_CR4[7:0]][18];         // ECX[27]: OSXSAVE
                end
                if (w_gpr[0][31:0] == 32'h0B) r[127:96] = apic_id;   // EDX: x2APIC ID
                w_gpr[0] <= {32'b0, r[31:0]};
                w_gpr[3] <= {32'b0, r[63:32]};
                w_gpr[1] <= {32'b0, r[95:64]};
                w_gpr[2] <= {32'b0, r[127:96]};
              end
              U_SETF: if (u.imm[5:0] != 6'd1) w_flags[u.imm[5:0]] <= va[0];
              U_DIVU, U_DIVS: begin
                if (alu_de) begin
                  raise(8'd0, 1'b0, 32'd0, 1'b0, 1'b0, 1'b0);
                  hold = 1'b1;
                end else begin
                  wr(u.dst, u.sz, alu_res);
                  if (alu_rem_we) rem_r <= alu_hi;
                end
              end
              default: begin
                if (u.op inside {U_ADD, U_ADC, U_SUB, U_SBB, U_AND, U_OR, U_XOR, U_ANDN, U_SHL,
                                 U_SHLD, U_SHRD,
                                 U_SHR, U_SAR, U_ROL, U_ROR, U_RCL, U_RCR, U_NEG, U_NOT, U_MOV,
                                 U_MULU, U_MULS, U_IMULT, U_ZEXT, U_SEXT, U_SETCC, U_BSF, U_BSR,
                                 U_TZCNT, U_LZCNT, U_POPCNT, U_BSWAP, U_SZP, U_GETF, U_SLICE,
                                 U_PDEP, U_PEXT, U_BEXTR, U_BZHI, U_BLSI, U_BLSMSK, U_BLSR,
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
                if (dmap == 2'd1 && (dopc == 8'h21 || dopc == 8'h23) && rex_v && rex[2]) crud = 1'b1;
                if (!attr.valid || crud) upc <= UADDR_UD;
              end
            endcase
            if (!(hold || u.op == U_END || u.op == U_SHUTDOWN)) upc <= next;
          end
        end

        // ------------------------------------------------------------ halted
        S_HALT: begin
          if ((smi_pend && !in_smm) || (nmi_pend && !in_smm) || (intr_valid && w_flags[9]))
            st <= S_FETCH;
        end

        // ------------------------------------------------------------ wait for SIPI
        // an AP after reset or INIT: only INIT and SIPI matter; SIPI starts it
        // in real mode at vector * 4 KiB (CS = vector << 8, IP = 0; SDM Vol. 3A
        // 10.4.4.1 "Typical BSP Initialization Sequence", 12.6.1)
        S_WFS: if (sipi) begin
          sp[segi(1, 0)] <= {48'b0, sipi_vec, 8'h00};
          sp[segi(1, 1)] <= {44'b0, sipi_vec, 12'h000};
          rip <= '0;
          nrip <= '0;
          start_decode();
          st <= S_FETCH;
        end

        // ------------------------------------------------------------ debug
        // halted for the debug module (docs/jtag.md): registers are read and
        // written through dbg_reg_*; dbg_step runs one instruction, dropping
        // dbg_req resumes (into HLT if the core was halted when it stopped)
        S_DEBUG: begin
          if (dbg_reg_we) begin
            if (dbg_reg_sel < 8'd16) begin
              a_gpr[dbg_reg_sel[3:0]] <= dbg_reg_wdata;
              w_gpr[dbg_reg_sel[3:0]] <= dbg_reg_wdata;
            end else if (dbg_reg_sel == 8'd16) begin
              rip <= dbg_reg_wdata;
              nrip <= dbg_reg_wdata;
            end else if (dbg_reg_sel == 8'd17) begin
              a_flags <= (dbg_reg_wdata & 64'h0000_0000_003F_7FD5) | 64'h2;
              w_flags <= (dbg_reg_wdata & 64'h0000_0000_003F_7FD5) | 64'h2;
            end else if (dbg_reg_sel[7]) begin
              wr_special(dbg_reg_sel & 8'h7F, dbg_reg_wdata);
            end
          end
          if (dbg_step) begin
            dbg_stepping <= 1'b1;
            dbg_stepped <= 1'b0;
            dbg_resume_halt <= 1'b0;
            start_decode();
            st <= S_FETCH;
          end else if (!dbg_req) begin
            start_decode();
            st <= dbg_resume_halt ? S_HALT : S_FETCH;
            dbg_resume_halt <= 1'b0;
          end
        end

        default: ;   // S_SHUTDOWN: until reset
      endcase

      // interrupts at an instruction boundary (Intel SDM Vol. 3A, 7.9 "Priority
      // Among Concurrent Exceptions and Interrupts")
      if (((st == S_FETCH && ds == D_PFX && idx == 0 && !in_delivery && !bus_req) ||
           st == S_HALT || st == S_WFS) && !int_shadow) begin
        if (st == S_WFS) begin
          // waiting for SIPI: another INIT starts the wait over; nothing else
          if (init_pend) init_pend <= 1'b0;
        end else if (mce_pend) begin
          // machine check: an abort, above every other event. #MC (vector 18)
          // with CR4.MCE set, else the processor shuts down (Intel SDM Vol. 3A
          // 17.10 "Machine-Check Initialization", Table 7-2 priority 1)
          mce_pend <= 1'b0;
          if (sp[SP_CR4[7:0]][6]) raise(8'd18, 1'b0, 32'd0, 1'b0, 1'b0, 1'b1);
          else begin
            st <= S_SHUTDOWN;
            shutdown_rip <= rip;
          end
        end else if (dbg_req && (!dbg_stepping || dbg_stepped)) begin
          // the debug module's halt: above every other event; a step stops
          // after its one instruction (events it took are delivered first)
          dbg_resume_halt <= (st == S_HALT);
          dbg_stepping <= 1'b0;
          dbg_stepped <= 1'b0;
          st <= S_DEBUG;
        end else if (smi_pend && !in_smm) begin
          // SMI: above NMI and INTR (Intel SDM Vol. 3A Table 7-2: "external hardware
          // interventions"); SMIACT# on, then the SMI routine saves the state at
          // SMBASE + FE00h (AMD APM Vol. 2 10.3). bit 1: the processor was halted.
          smi_pend <= 1'b0;
          sp[SP_SMM[7:0]] <= {62'b0, st == S_HALT, 1'b1};
          sp[SP_FAULT_RIP[7:0]] <= rip;
          nrip <= rip;
          upc <= UADDR_SMI;
          udepth <= '0;
          in_delivery <= 1'b1;
          deliv_vec <= 8'hFF;
          ev_valid <= 1'b1;               // traced as event "vector" FFh (not an IDT vector)
          ev_vector <= 8'hFF;
          ev_has_error <= 1'b0;
          ev_error <= '0;
          ev_rip <= rip;
          shadow_set <= 1'b0;
          ms <= M_IDLE;
          st <= S_EXEC;
        end else if (init_pend && !in_smm) begin
          // INIT (SDM Vol. 3A Table 10-1, INIT column): the reset state, but
          // IA32_APIC_BASE, the TSC and SMBASE are kept; the BSP restarts at
          // the reset vector, an AP waits for SIPI. INIT is held in SMM.
          init_pend <= 1'b0;
          reset_state();
          sp[SP_APIC_BASE[7:0]] <= sp[SP_APIC_BASE[7:0]];
          sp[SP_TSC[7:0]] <= sp[SP_TSC[7:0]];
          sp[SP_SMBASE[7:0]] <= sp[SP_SMBASE[7:0]];
          bus_req <= 1'b0;
          start_decode();
          st <= sp[SP_APIC_BASE[7:0]][8] ? S_FETCH : S_WFS;
        end else if (nmi_pend && !in_smm) begin   // NMIs are held in SMM until RSM
          nmi_pend <= 1'b0;
          raise(8'd2, 1'b0, 32'd0, 1'b1, 1'b0, 1'b1);
        end else if (intr_valid && w_flags[9]) begin
          intr_ack <= 1'b1;
          raise(intr_vector, 1'b0, 32'd0, 1'b1, 1'b0, 1'b1);
        end
      end
      // the instruction cache: a filled line becomes valid as the data
      // array writes it; a snooped write invalidates its line (and the
      // line buffer, and an outstanding fill of it) -- last, so it wins
      if (ic_we) begin
        ic_we <= 1'b0;
        ic_v[ic_widx] <= 1'b1;
      end
      if (ic_inv) begin
        ic_v[ic_inv_line[15:6]] <= 1'b0;
        if (ic_fill && ic_pa[51:6] == ic_inv_line) ic_poison <= 1'b1;
        lb_v <= 1'b0;
      end
      if (dc_we) begin
        dc_we <= 1'b0;
        if (dc_wtag_en) dc_v[dc_widx] <= 1'b1;
      end
      if (ic_inv && !ic_inv_core) begin
        dc_v[ic_inv_line[17:6]] <= 1'b0;
        if (dc_fill && dc_pa[51:6] == ic_inv_line) dc_poison <= 1'b1;
      end
    end
  end
endmodule
