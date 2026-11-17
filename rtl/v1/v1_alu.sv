// v1_alu: the scalar integer datapath of the v1 core.
//
// One micro-op per call: result, the x86 status flags it produces (CF PF AF
// ZF SF OF in fmask order, bit 0..5) and which of them the operation defines
// (`native`); the core writes RFLAGS bits in (uop.fmask & native). Flag
// definitions follow the Intel SDM Vol. 1, 3.4.3 "EFLAGS Register", and the
// Flags Affected sections of Vol. 2 for each instruction the op serves.
//
// Multiplication and division are single-cycle behavioural operators in v1
// (Verilator evaluates them on wide integers); a later version replaces them
// with an iterative divider (e.g. SRT, after Oberman & Flynn, "Division
// Algorithms and Implementations", IEEE Trans. Computers 46(8), 1997,
// doi:10.1109/12.609274) and a multiplier array.
module v1_alu
  import v1_uop_pkg::*;
(
  input  uop_op_e     op,
  input  logic [1:0]  sz,         // 0 8, 1 16, 2 32, 3 64
  input  logic [63:0] a,
  input  logic [63:0] b,
  input  logic [63:0] c,          // third source (SEL, SHLD/SHRD count)
  input  logic [63:0] dst_old,    // INSERT, KHI, TERNLOG
  input  logic [63:0] hi_in,      // the high half for DIV
  input  logic [31:0] imm,
  input  logic [3:0]  cc,
  input  logic [3:0]  x,
  input  logic [63:0] flags_in,   // RFLAGS (working)
  output logic [63:0] res,
  output logic [63:0] hi_out,     // MUL high half / DIV remainder
  output logic        hi_we,
  output logic        rem_we,
  output logic [5:0]  fl,         // CF PF AF ZF SF OF
  output logic [5:0]  native,
  output logic        de,         // divide error
  output logic        nowrite     // the result must not be written (BSF/BSR of 0)
);
  localparam int CF = 0, PF = 1, AF = 2, ZF = 3, SF = 4, OF = 5;

  function automatic logic [63:0] szmask(input logic [1:0] s);
    case (s)
      2'd0: return 64'hFF;
      2'd1: return 64'hFFFF;
      2'd2: return 64'hFFFF_FFFF;
      default: return '1;
    endcase
  endfunction
  function automatic int nbits(input logic [1:0] s);
    return 8 << s;
  endfunction
  function automatic logic msb(input logic [63:0] v, input logic [1:0] s);
    return v[nbits(s) - 1];
  endfunction
  function automatic logic [63:0] sext(input logic [63:0] v, input logic [1:0] s);
    case (s)
      2'd0: return {{56{v[7]}}, v[7:0]};
      2'd1: return {{48{v[15]}}, v[15:0]};
      2'd2: return {{32{v[31]}}, v[31:0]};
      default: return v;
    endcase
  endfunction
  function automatic logic parity(input logic [7:0] v);
    return ~^v;
  endfunction
  // x86 condition codes over RFLAGS (Intel SDM Vol. 1, appendix B)
  // RFLAGS is passed whole; a condition reads the arithmetic flags
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic cond(input logic [3:0] k, input logic [63:0] f);
  /* verilator lint_on UNUSEDSIGNAL */
    logic r;
    case (k[3:1])
      3'd0: r = f[11];                         // O
      3'd1: r = f[0];                          // B
      3'd2: r = f[6];                          // E
      3'd3: r = f[0] | f[6];                   // BE
      3'd4: r = f[7];                          // S
      3'd5: r = f[2];                          // P
      3'd6: r = f[7] != f[11];                 // L
      default: r = f[6] | (f[7] != f[11]);     // LE
    endcase
    return r ^ k[0];
  endfunction

  logic [63:0] m, am, bm;
  logic [64:0] wide;
  logic        cin;
  int unsigned n;
  logic [6:0]  cnt;
  logic [127:0] prod;
  logic [127:0] dvd;
  logic [127:0] q;
  logic [63:0]  r;               // a remainder is smaller than the 64-bit divisor

  always_comb begin
    m = szmask(sz);
    n = nbits(sz);
    am = a & m;
    bm = b & m;
    res = '0;
    fl = '0;
    native = '0;
    hi_out = '0;
    hi_we = 1'b0;
    rem_we = 1'b0;
    de = 1'b0;
    nowrite = 1'b0;
    cin = 1'b0;
    wide = '0;
    cnt = '0;
    prod = '0;
    dvd = '0;
    q = '0;
    r = '0;
    unique case (op)
      U_ADD, U_ADC: begin
        // x = 1 (ADOX): OF is the carry in and the carry out, nothing else
        cin = (op == U_ADC) ? (x == 4'd1 ? flags_in[11] : flags_in[0]) : 1'b0;
        wide = {1'b0, am} + {1'b0, bm} + {64'b0, cin};
        res = wide[63:0] & m;
        fl[CF] = (n == 64) ? wide[64] : wide[n];
        fl[OF] = (msb(am, sz) == msb(bm, sz)) && (msb(res, sz) != msb(am, sz));
        fl[AF] = (am[4] ^ bm[4] ^ res[4]);
        native = 6'h3F;
        if (op == U_ADC && x == 4'd1) begin
          fl[OF] = fl[CF];
          native = 6'h20;
        end
      end
      U_CRC32: begin
        // CRC-32C (Castagnoli, reflected polynomial 82F63B78h) of b's n bytes into a
        logic [31:0] crc;
        crc = a[31:0];
        for (int i = 0; i < 8; i++) begin
          if (i < n / 8) begin
            crc = crc ^ {24'b0, b[8 * i +: 8]};
            for (int k = 0; k < 8; k++)
              crc = crc[0] ? ((crc >> 1) ^ 32'h82F63B78) : (crc >> 1);
          end
        end
        res = {32'b0, crc};
      end
      U_SUB, U_SBB: begin
        cin = (op == U_SBB) ? flags_in[0] : 1'b0;
        wide = {1'b0, am} - {1'b0, bm} - {64'b0, cin};
        res = wide[63:0] & m;
        fl[CF] = ({1'b0, am} < ({1'b0, bm} + {64'b0, cin}));
        fl[OF] = (msb(am, sz) != msb(bm, sz)) && (msb(res, sz) != msb(am, sz));
        fl[AF] = (am[4] ^ bm[4] ^ res[4]);
        native = 6'h3F;
      end
      U_NEG: begin
        res = (64'd0 - am) & m;
        fl[CF] = am != 0;
        fl[OF] = am == ((64'd1 << (n - 1)) & m);
        fl[AF] = am[4] ^ res[4];
        native = 6'h3F;
      end
      U_AND, U_OR, U_XOR, U_ANDN: begin
        case (op)
          U_AND:  res = am & bm;
          U_OR:   res = am | bm;
          U_XOR:  res = am ^ bm;
          default: res = ~am & bm & m;
        endcase
        native = 6'h3F;            // OF = CF = 0 (AF: 0, the manual leaves it undefined)
      end
      U_NOT: res = ~am & m;
      // BMI1/BMI2 (Intel SDM Vol. 2: PDEP, PEXT, BEXTR, BZHI, BLSI, BLSMSK,
      // BLSR; flags per their Flags Affected sections; native: CF ZF SF OF)
      U_PDEP, U_PEXT: begin
        // PDEP: a's low bits go to b's set bit positions; PEXT: the bits of a
        // at b's set positions are packed into the low bits
        logic [6:0] k2;
        k2 = '0;
        for (int i = 0; i < 64; i++)
          if (i < int'(n) && bm[i]) begin
            if (op == U_PDEP) res[i] = am[k2[5:0]];
            else res[k2[5:0]] = am[i];
            k2 = k2 + 7'd1;
          end
      end
      U_BEXTR: begin
        // start b[7:0], length b[15:8]; ZF from the result, CF = OF = 0
        logic [7:0] st, ln;
        st = b[7:0];
        ln = b[15:8];
        res = (32'(st) < n) ? (am >> st) : 64'd0;
        if (32'(ln) < n) res = res & ((64'd1 << ln) - 64'd1);
        res = res & m;
        fl[ZF] = res == 0;
        native = 6'h29;
      end
      U_BZHI: begin
        // zero the bits from index b[7:0] up; CF = the index is >= the size
        logic [7:0] ix;
        ix = b[7:0];
        res = (32'(ix) < n) ? (am & ((64'd1 << ix) - 64'd1)) : am;
        fl[CF] = 32'(ix) > n - 1;
        fl[ZF] = res == 0;
        fl[SF] = msb(res, sz);
        native = 6'h39;
      end
      U_BLSI, U_BLSMSK, U_BLSR: begin
        case (op)
          U_BLSI:   res = (64'd0 - am) & am & m;
          U_BLSMSK: res = ((am - 64'd1) ^ am) & m;
          default:  res = (am - 64'd1) & am & m;
        endcase
        fl[CF] = (op == U_BLSI) ? am != 0 : am == 0;
        fl[ZF] = (op == U_BLSMSK) ? 1'b0 : res == 0;
        fl[SF] = msb(res, sz);
        native = 6'h39;
      end
      U_MOV: res = am;
      U_SHL, U_SHR, U_SAR: begin
        cnt = 7'(b & ((sz == 2'd3) ? 64'd63 : 64'd31));
        if (cnt == 0) begin
          res = am;
        end else begin
          native = 6'h3F;
          case (op)
            U_SHL: begin
              res = (am << cnt) & m;
              fl[CF] = (cnt <= 7'(n)) ? am[n - int'(cnt)] : 1'b0;
              fl[OF] = msb(res, sz) ^ fl[CF];
            end
            U_SHR: begin
              res = am >> cnt;
              fl[CF] = (cnt <= 7'(n)) ? am[cnt - 1] : 1'b0;
              fl[OF] = msb(am, sz);
            end
            default: begin
              res = 64'($signed(sext(am, sz)) >>> cnt) & m;
              fl[CF] = (cnt <= 7'(n)) ? am[cnt - 1] : msb(am, sz);
              fl[OF] = 1'b0;
            end
          endcase
        end
      end
      U_ROL, U_ROR: begin
        cnt = 7'(b & ((sz == 2'd3) ? 64'd63 : 64'd31));
        res = am;
        if (cnt != 0) begin
          logic [6:0] k;
          k = 7'(int'(cnt) % n);
          if (op == U_ROL) res = ((am << k) | (am >> (n - int'(k)))) & m;
          else             res = ((am >> k) | (am << (n - int'(k)))) & m;
          if (k == 0) res = am;
          native = (1 << CF) | (1 << OF);
          if (op == U_ROL) begin
            fl[CF] = res[0];
            fl[OF] = msb(res, sz) ^ res[0];
          end else begin
            fl[CF] = msb(res, sz);
            fl[OF] = msb(res, sz) ^ res[n - 2];
          end
        end
      end
      U_RCL, U_RCR: begin
        cnt = 7'(b & ((sz == 2'd3) ? 64'd63 : 64'd31));
        res = am;
        if (cnt != 0) begin
          logic c0, cx;
          logic [63:0] v;
          int k;
          k = int'(cnt) % (n + 1);
          v = am;
          c0 = flags_in[0];
          if (op == U_RCR) fl[OF] = msb(am, sz) ^ c0;
          for (int i = 0; i < 64; i++) begin
            if (i < k) begin
              if (op == U_RCL) begin
                cx = v[n - 1];
                v = ((v << 1) | {63'b0, c0}) & m;
                c0 = cx;
              end else begin
                cx = v[0];
                v = (v >> 1) | (64'(c0) << (n - 1));
                c0 = cx;
              end
            end
          end
          res = v;
          fl[CF] = c0;
          if (op == U_RCL) fl[OF] = msb(v, sz) ^ c0;
          native = (1 << CF) | (1 << OF);
        end
      end
      U_SHLD, U_SHRD: begin
        // a double shift: dst = a shifted, filled from b (count c, masked as
        // SHL's; a count above the operand size leaves the result undefined)
        cnt = 7'(c & ((sz == 2'd3) ? 64'd63 : 64'd31));
        res = am;
        if (cnt != 0) begin
          logic [127:0] w2;
          if (op == U_SHLD) begin
            w2 = ({64'b0, am} << n) | {64'b0, bm};       // a:b
            w2 = w2 << cnt;
            res = 64'(w2 >> n) & m;
            fl[CF] = (cnt <= 7'(n)) ? am[n - int'(cnt)] : 1'b0;
          end else begin
            w2 = ({64'b0, bm} << n) | {64'b0, am};       // b:a
            w2 = w2 >> cnt;
            res = w2[63:0] & m;
            fl[CF] = (cnt <= 7'(n)) ? am[cnt - 1] : 1'b0;
          end
          fl[OF] = msb(res, sz) != msb(am, sz);
          fl[SF] = msb(res, sz);
          fl[ZF] = res == 0;
          fl[PF] = parity(res[7:0]);
          native = (1 << CF) | (1 << OF) | (1 << SF) | (1 << ZF) | (1 << PF);
        end
      end
      U_MULU, U_MULS, U_IMULT: begin
        if (op == U_MULU) prod = {64'b0, am} * {64'b0, bm};
        else prod = 128'($signed({{64{sext(am, sz)[63]}}, sext(am, sz)}) *
                         $signed({{64{sext(bm, sz)[63]}}, sext(bm, sz)}));
        res = prod[63:0] & m;
        hi_out = 64'((prod >> n) & {64'b0, m});
        hi_we = (op != U_IMULT);
        if (op == U_MULU) fl[CF] = hi_out != 0;
        else fl[CF] = prod != {{64{sext(res, sz)[63]}}, sext(res, sz)};
        fl[OF] = fl[CF];
        native = (1 << CF) | (1 << OF);
      end
      U_DIVU, U_DIVS: begin
        // dividend HI:a at twice the operand size
        dvd = ({64'b0, hi_in & m} << n) | {64'b0, am};
        if (bm == 0) de = 1'b1;
        else if (op == U_DIVU) begin
          q = dvd / {64'b0, bm};
          r = 64'(dvd % {64'b0, bm});
          if ((q >> n) != 0) de = 1'b1;
        end else begin
          logic [127:0] sd, sv;
          if (n == 64) sd = dvd;
          else begin
            sd = dvd;
            for (int i = 0; i < 128; i++) if (i >= 2 * n) sd[i] = dvd[2 * n - 1];
          end
          sv = {{64{sext(bm, sz)[63]}}, sext(bm, sz)};
          if (n == 64 && dvd == {1'b1, 127'b0} && bm == '1) de = 1'b1;
          else begin
            q = 128'($signed(sd) / $signed(sv));
            r = 64'($signed(sd) % $signed(sv));
            // the quotient must fit in n signed bits
            if ($signed(q) > $signed(128'((128'd1 << (n - 1)) - 1)) ||
                $signed(q) < -$signed(128'(128'd1 << (n - 1)))) de = 1'b1;
          end
        end
        res = q[63:0] & m;
        hi_out = r & m;
        rem_we = 1'b1;
      end
      U_ZEXT: res = am;
      U_SEXT: res = sext(am, sz);
      U_SETCC: res = {63'b0, cond(cc, flags_in)};
      U_BSF, U_BSR, U_TZCNT, U_LZCNT, U_POPCNT: begin
        int idx;
        int pc;
        idx = 0;
        pc = 0;
        for (int i = 0; i < 64; i++) if (i < n && am[i]) pc++;
        native = 6'h3F;
        case (op)
          U_BSF: begin
            for (int i = 63; i >= 0; i--) if (am[i]) idx = i;
            res = 64'(idx);
            nowrite = am == 0;       // destination unchanged (as AMD documents; Intel: undefined)
            fl[ZF] = am == 0;
          end
          U_BSR: begin
            for (int i = 0; i < 64; i++) if (am[i]) idx = i;
            res = 64'(idx);
            nowrite = am == 0;
            fl[ZF] = am == 0;
          end
          U_TZCNT: begin
            idx = n;
            for (int i = 63; i >= 0; i--) if (i < n && am[i]) idx = i;
            res = 64'(idx);
            fl[CF] = am == 0;
            fl[ZF] = res == 0;
          end
          U_LZCNT: begin
            idx = n;
            for (int i = 0; i < 64; i++) if (i < n && am[i]) idx = n - 1 - i;
            res = 64'(idx);
            fl[CF] = am == 0;
            fl[ZF] = res == 0;
          end
          default: begin
            res = 64'(pc);
            fl[ZF] = am == 0;
          end
        endcase
      end
      U_BSWAP: begin
        if (sz == 2'd3) res = {a[7:0], a[15:8], a[23:16], a[31:24], a[39:32], a[47:40], a[55:48], a[63:56]};
        else res = {32'b0, a[7:0], a[15:8], a[23:16], a[31:24]};
      end
      U_SZP: begin
        res = am;
        native = (1 << SF) | (1 << ZF) | (1 << PF);
      end
      U_GETF: res = {63'b0, flags_in[imm[5:0]]};
      U_SLICE: begin
        logic [63:0] w;
        w = (imm[15:8] >= 8'd64) ? '1 : ((64'd1 << imm[15:8]) - 1);
        res = (a >> imm[7:0]) & w;
      end
      U_INSERT: begin
        logic [63:0] w;
        w = (imm[15:8] >= 8'd64) ? '1 : ((64'd1 << imm[15:8]) - 1);
        res = (dst_old & ~(w << imm[7:0])) | ((a & w) << imm[7:0]);
      end
      U_KHI: res = {imm, dst_old[31:0]};
      U_SEL: res = c[0] ? a : b;
      default: res = am;
    endcase
    // SF, ZF, PF of the result for the ops that define them "according to the result"
    if (op inside {U_ADD, U_ADC, U_SUB, U_SBB, U_NEG, U_AND, U_OR, U_XOR, U_ANDN, U_SHL, U_SHR,
                   U_SAR, U_SZP}) begin
      fl[SF] = msb(res, sz);
      fl[ZF] = (res & m) == 0;
      fl[PF] = parity(res[7:0]);
    end
    if (op == U_BSF || op == U_BSR || op == U_POPCNT) fl[PF] = 1'b0;
  end
endmodule
