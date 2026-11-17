// v1_fp_pkg: the floating-point datapath of the v1 core (D-009).
//
// One scalar operation per call, on f16, bf16, f32, f64 or x87's f80:
// unpack into sign / unbiased exponent / significand, compute exactly (or
// with enough extra bits plus a sticky bit), then round and pack once. This
// is the single-rounding structure of a fused multiply-add datapath
// (Montoye, Hokenek & Runyon, "Design of the IBM RISC System/6000
// floating-point execution unit", IBM J. R&D 34(1), 1990,
// doi:10.1147/rd.341.0059), with x86's rules from the Intel SDM Vol. 1:
//   4.8.3   encodings (QNaN floating-point indefinite: sign 1, quiet bit)
//   4.9     exceptions IE DE ZE OE UE PE; tininess is detected after
//           rounding, as on x86 (cf. SoftFloat's tininess_afterRounding)
//   10.2.3  MXCSR (RC, FZ, DAZ), 4.8.3.6 / 11.5 NaN operands: a signaling
//           NaN raises IE; with two NaNs the first source's is returned,
//           quieted (SSE); x87 returns the one with the larger significand
// Division and square root use the behavioural wide-integer operators in
// v1; a later version iterates them (Ercegovac & Lang, *Digital
// Arithmetic*, 2004, ch. 5-6).
package v1_fp_pkg;
  // formats (uop element types)
  localparam logic [3:0] FMT_F16 = 4'd8, FMT_BF16 = 4'd9, FMT_F32 = 4'd10, FMT_F64 = 4'd11,
                         FMT_F80 = 4'd12;
  // exception flags, in MXCSR/FSW order: IE DE ZE OE UE PE (bit 0..5)
  localparam int XIE = 0, XDE = 1, XZE = 2, XOE = 3, XUE = 4, XPE = 5;

  typedef struct packed {
    logic        sign;
    logic signed [17:0] exp;       // unbiased; the significand is 1.f at bit 63
    logic [63:0] sig;              // normalized for finite non-zero values
    logic        zero, inf, nan, snan, denorm;
    logic [63:0] payload;          // NaN payload (fraction, left-aligned at bit 62)
  } unpacked_t;

  function automatic int fbits(input logic [3:0] f);    // fraction bits
    case (f)
      FMT_F16: return 10;
      FMT_BF16: return 7;
      FMT_F32: return 23;
      FMT_F64: return 52;
      default: return 63;          // f80: 64-bit significand, explicit integer bit
    endcase
  endfunction
  function automatic int ebits(input logic [3:0] f);
    case (f)
      FMT_F16: return 5;
      FMT_BF16, FMT_F32: return 8;
      FMT_F64: return 11;
      default: return 15;
    endcase
  endfunction
  function automatic int bias(input logic [3:0] f);
    return (1 << (ebits(f) - 1)) - 1;
  endfunction
  function automatic int width(input logic [3:0] f);
    case (f)
      FMT_F16, FMT_BF16: return 16;
      FMT_F32: return 32;
      FMT_F64: return 64;
      default: return 80;
    endcase
  endfunction

  function automatic unpacked_t unpack(input logic [79:0] v, input logic [3:0] f, input logic daz);
    unpacked_t u;
    int fb, eb, b;
    logic [14:0] e;
    logic [63:0] frac;
    fb = fbits(f);
    eb = ebits(f);
    b = bias(f);
    u = '0;
    u.sign = v[width(f) - 1];
    e = 15'((v >> fb) & ((80'd1 << eb) - 1));
    if (f == FMT_F80) begin
      e = v[78:64];
      frac = v[63:0];
    end else
      frac = 64'(v & ((80'd1 << fb) - 1)) << (63 - fb);   // left-aligned below bit 63
    if (e == 15'((1 << eb) - 1)) begin
      logic [62:0] fr;
      fr = (f == FMT_F80) ? frac[62:0] : frac[62:0];
      if (fr == 0) u.inf = 1'b1;
      else begin
        u.nan = 1'b1;
        u.snan = !fr[62];
        u.payload = {1'b0, fr};
      end
    end else if (e == 0 && (f == FMT_F80 ? frac == 0 : frac == 0)) begin
      u.zero = 1'b1;
    end else if (e == 0 || (f == FMT_F80 && !frac[63])) begin
      // a subnormal (or an x87 pseudo-denormal): normalize
      if (daz && f != FMT_F80) u.zero = 1'b1;
      else begin
        // value = frac * 2^(1 - bias - 63) (frac: 0.f below bit 63; f80: the
        // significand with a clear integer bit): normalize so bit 63 is set
        logic [63:0] t;
        int k;
        u.denorm = 1'b1;
        t = frac;
        k = 0;
        for (int i = 0; i < 64; i++) if (t[63 - i] == 0 && k == i) k = i + 1;
        u.sig = t << k;
        u.exp = 18'(1 - b - k);
      end
    end else begin
      u.exp = 18'(int'(e) - b);
      u.sig = (f == FMT_F80) ? frac : (64'h8000_0000_0000_0000 | frac);
    end
    return u;
  endfunction

  // the default NaN (QNaN floating-point indefinite) and a quieted NaN
  function automatic logic [79:0] qnan_indef(input logic [3:0] f);
    int fb, eb;
    fb = fbits(f);
    eb = ebits(f);
    if (f == FMT_F80) return {1'b1, 15'h7FFF, 2'b11, 62'b0};
    return (80'd1 << (fb + eb)) | (((80'd1 << eb) - 1) << fb) | (80'd1 << (fb - 1));
  endfunction
  // operands are passed as whole unpacked values; this function reads some fields
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic [79:0] pack_nan(input unpacked_t u, input logic [3:0] f);
  /* verilator lint_on UNUSEDSIGNAL */
    int fb, eb;
    fb = fbits(f);
    eb = ebits(f);
    if (f == FMT_F80) return {u.sign, 15'h7FFF, 1'b1, 1'b1, u.payload[61:0]};
    return (80'(u.sign) << (fb + eb)) | (((80'd1 << eb) - 1) << fb) | (80'd1 << (fb - 1)) |
           ((80'(u.payload[61:0]) >> (62 - fb + 1)) & ((80'd1 << (fb - 1)) - 80'd1));
  endfunction

  typedef struct packed {
    logic [79:0] v;
    logic [5:0]  x;                // IE DE ZE OE UE PE
  } fres_t;

  // round a positive significand `sig` (leading 1 at or below bit 127, the
  // value being sig * 2^(exp - 127)) to format f, rounding mode rm
  // (0 nearest-even, 1 down, 2 up, 3 toward zero); sticky: bits already lost
  function automatic fres_t round_pack(input logic sign, input logic signed [19:0] exp,
                                       input logic [127:0] sig, input logic sticky,
                                       input logic [3:0] f, input logic [1:0] rm,
                                       input logic ftz, input int prec);
    fres_t r;
    int p, b, eb, fb, lz, sh;
    logic signed [19:0] emin, emax;
    logic signed [19:0] e;
    logic [127:0] s;
    logic [127:0] keep, half, rest;
    logic st, inexact, tiny, up;
    logic [127:0] m;
    r = '0;
    fb = fbits(f);
    eb = ebits(f);
    b = bias(f);
    p = (f == FMT_F80) ? prec : fb + 1;                  // precision in bits
    emin = 20'(1 - b);
    emax = 20'(b);
    if (sig == 0) begin
      r.v = 80'(sign) << (width(f) - 1);
      return r;
    end
    // normalize: leading 1 at bit 127
    lz = 0;
    for (int i = 0; i < 128; i++) if (sig[127 - i] == 0 && lz == i) lz = i + 1;
    s = sig << lz;
    e = exp - 20'(lz);
    st = sticky;
    // tininess after rounding (IEEE 754-2008 7.5; x86): the result rounded to
    // p bits *with an unbounded exponent* is below 2^emin. Only a value just
    // under 2^emin whose p-bit rounding carries out isn't tiny. (Rounding
    // at the subnormal precision instead, as v1 did, missed UE and FTZ for
    // results that round up to the smallest normal: rtl/v1/tb/tb_fpu_ref.)
    tiny = 1'b0;
    if (e < emin) begin
      logic [127:0] k0, r0, h0;
      logic u0;
      k0 = s >> (128 - p);
      r0 = s & ((128'd1 << (128 - p)) - 1);
      h0 = 128'd1 << (127 - p);
      case (rm)
        2'd0: u0 = r0 > h0 || (r0 == h0 && (st || k0[0]));
        2'd1: u0 = sign && (r0 != 0 || st);
        2'd2: u0 = !sign && (r0 != 0 || st);
        default: u0 = 1'b0;
      endcase
      tiny = !(e == emin - 20'sd1 && u0 && k0 == (128'd1 << p) - 1);
    end
    // subnormal: shift right so the exponent is emin
    if (e < emin) begin
      sh = int'(emin) - int'(e);
      if (sh > 127) begin
        st = st | (s != 0);
        s = 0;
      end else begin
        st = st | ((s & ((128'd1 << sh) - 1)) != 0);
        s = s >> sh;
      end
      e = emin;
    end
    // keep the top p bits
    keep = s >> (128 - p);
    rest = s & ((128'd1 << (128 - p)) - 1);
    half = 128'd1 << (127 - p);
    inexact = (rest != 0) || st;
    case (rm)
      2'd0: up = (rest > half) || (rest == half && (st || keep[0])) || (rest == half && st);
      2'd1: up = sign && inexact;
      2'd2: up = !sign && inexact;
      default: up = 1'b0;
    endcase
    if (rm == 2'd0) up = rest > half || (rest == half && !st && keep[0]) || (rest == half && st);
    m = keep + (up ? 128'd1 : 128'd0);
    if (m[p]) begin                                      // carried out: renormalize
      m = m >> 1;
      e = e + 1;
    end
    if (tiny && ftz) begin                               // flush to zero (MXCSR.FZ)
      r.v = 80'(sign) << (width(f) - 1);
      r.x[XUE] = 1'b1;
      r.x[XPE] = 1'b1;
      return r;
    end
    if (e > emax) begin                             // overflow
      r.x[XOE] = 1'b1;
      r.x[XPE] = 1'b1;
      if (rm == 2'd3 || (rm == 2'd1 && !sign) || (rm == 2'd2 && sign)) begin
        // the largest finite number
        // (x87 with FCW.PC < 64: the largest number of that precision)
        if (f == FMT_F80) r.v = {sign, 15'h7FFE, 64'hFFFF_FFFF_FFFF_FFFF << (64 - p)};
        else r.v = (80'(sign) << (fb + eb)) | (80'((1 << eb) - 2) << fb) | ((80'd1 << fb) - 1);
      end else begin
        if (f == FMT_F80) r.v = {sign, 15'h7FFF, 1'b1, 63'b0};
        else r.v = (80'(sign) << (fb + eb)) | (80'((1 << eb) - 1) << fb);
      end
      return r;
    end
    r.x[XPE] = inexact;
    r.x[XUE] = tiny && inexact;
    if (f == FMT_F80) begin
      logic [14:0] be;
      be = m[p - 1] ? 15'(int'(e) + b) : 15'd0;
      r.v = {sign, be, 64'(m << (64 - p))};
    end else begin
      logic [79:0] be;
      be = m[p - 1] ? 80'(int'(e) + b) : 80'd0;
      r.v = (80'(sign) << (fb + eb)) | (be << fb) | (80'(m) & ((80'd1 << fb) - 1));
    end
    return r;
  endfunction

  // NaN operands (SSE: the first NaN source, quieted; IE for any SNaN)
  function automatic logic nan_in(input unpacked_t a, input unpacked_t b, input unpacked_t c,
                                  input int n, input logic [3:0] f, output fres_t r);
    r = '0;
    if ((a.nan) || (n > 1 && b.nan) || (n > 2 && c.nan)) begin
      r.x[XIE] = a.snan || (n > 1 && b.snan) || (n > 2 && c.snan);
      r.v = a.nan ? pack_nan(a, f) : (n > 1 && b.nan) ? pack_nan(b, f) : pack_nan(c, f);
      return 1'b1;
    end
    return 1'b0;
  endfunction

  // op: 0 add, 1 sub, 2 mul, 3 div, 4 sqrt, 5 fma (a*b + c), 6 min, 7 max
  // the SIMD (SSE/AVX) exception precedence (Intel SDM Vol. 1 11.5.2,
  // "SIMD Floating-Point Exception Conditions"): an invalid operation or a
  // divide-by-zero hides a denormal operand (DE); found against the host's
  // SSE by rtl/v1/tb/tb_fpu_ref. (x87 reports #D ahead of #Z: F80 keeps it.)
  function automatic fres_t fp_arith(input logic [3:0] op, input logic [79:0] av,
                                     input logic [79:0] bv, input logic [79:0] cv,
                                     input logic [3:0] f, input logic [1:0] rm,
                                     input logic daz, input logic ftz, input int prec,
                                     input logic [1:0] neg);
    fres_t r;
    r = fp_arith_raw(op, av, bv, cv, f, rm, daz, ftz, prec, neg);
    if (f != FMT_F80 && (r.x[XIE] || r.x[XZE])) r.x[XDE] = 1'b0;
    return r;
  endfunction

  function automatic fres_t fp_arith_raw(input logic [3:0] op, input logic [79:0] av,
                                     input logic [79:0] bv, input logic [79:0] cv,
                                     input logic [3:0] f, input logic [1:0] rm,
                                     input logic daz, input logic ftz, input int prec,
                                     input logic [1:0] neg);
    fres_t r;
    unpacked_t a, b, c;
    int n;
    a = unpack(av, f, daz);
    b = unpack(bv, f, daz);
    c = unpack(cv, f, daz);
    if (neg[0]) a.sign = !a.sign;           // FMA variants: -(a*b) and -c
    if (neg[1]) c.sign = !c.sign;
    n = (op == 4'd4) ? 1 : (op == 4'd5) ? 3 : 2;
    r = '0;
    if (op == 4'd6 || op == 4'd7) begin
      // MINSS/MAXSS: if either is NaN, or both are zero, the second source
      logic lt;
      logic [79:0] ad, bd;
      // a denormal taken as zero (DAZ) comes back as that zero
      ad = a.zero ? 80'(a.sign) << (width(f) - 1) : av;
      bd = (b.zero && !b.nan) ? 80'(b.sign) << (width(f) - 1) : bv;
      if (a.nan || b.nan) begin
        r.v = bd;
        r.x[XIE] = 1'b1;
        return r;
      end
      lt = cmp_lt(a, b);
      r.v = (op == 4'd6) ? ((lt) ? ad : bd) : ((cmp_lt(b, a)) ? ad : bd);
      if (a.zero && b.zero) r.v = bd;
      r.x[XDE] = a.denorm || b.denorm;
      return r;
    end
    if (nan_in(a, b, c, n, f, r)) return r;
    r.x[XDE] = a.denorm || (n > 1 && b.denorm) || (n > 2 && c.denorm);
    case (op)
      4'd0, 4'd1: begin
        unpacked_t bb;
        bb = b;
        if (op == 4'd1) bb.sign = !bb.sign;
        r = merge_x(add_core(a, bb, f, rm, ftz, prec), r.x);
      end
      4'd2, 4'd5: begin
        // the product, exactly: 128 bits, exponent a.exp + b.exp
        logic [127:0] ps;
        logic psign;
        psign = a.sign ^ b.sign;
        if ((a.inf && b.zero) || (a.zero && b.inf)) begin
          r.v = qnan_indef(f);
          r.x[XIE] = 1'b1;
          return r;
        end
        if (op == 4'd2) begin
          if (a.inf || b.inf) begin
            r.v = pack_inf(psign, f);
            return r;
          end
          if (a.zero || b.zero) begin
            r.v = 80'(psign) << (width(f) - 1);
            return r;
          end
          ps = 128'(a.sig) * 128'(b.sig);
          r = merge_x(round_pack(psign, 20'(a.exp) + 20'(b.exp) + 1, ps, 1'b0, f, rm, ftz, prec), r.x);
        end else begin
          // fused: (a*b) + c with one rounding
          if (a.inf || b.inf) begin
            if (c.inf && c.sign != psign) begin
              r.v = qnan_indef(f);
              r.x[XIE] = 1'b1;
              return r;
            end
            r.v = pack_inf(psign, f);
            return r;
          end
          if (c.inf) begin
            r.v = pack_inf(c.sign, f);
            return r;
          end
          r = merge_x(fma_core(a, b, c, f, rm, ftz, prec), r.x);
        end
      end
      4'd3: begin
        logic qsign;
        qsign = a.sign ^ b.sign;
        if ((a.inf && b.inf) || (a.zero && b.zero)) begin
          r.v = qnan_indef(f);
          r.x[XIE] = 1'b1;
          return r;
        end
        if (a.inf || b.zero) begin
          r.v = pack_inf(qsign, f);
          if (b.zero && !a.inf) r.x[XZE] = 1'b1;
          return r;
        end
        if (a.zero || b.inf) begin
          r.v = 80'(qsign) << (width(f) - 1);
          return r;
        end
        begin
          logic [191:0] num;
          logic [127:0] q;
          logic rem;
          num = {1'b0, a.sig, 127'b0};          // a.sig / b.sig < 2: the quotient fits 128 bits
          q = 128'(num / 192'(b.sig));
          rem = (num % 192'(b.sig)) != 0;
          r = merge_x(round_pack(qsign, 20'(a.exp) - 20'(b.exp), q, rem, f, rm, ftz, prec), r.x);
        end
      end
      4'd4: begin
        if (a.zero) begin
          // +-0 (a denormal under DAZ too: not the input's bits)
          r.v = 80'(a.sign) << (width(f) - 1);
          return r;
        end
        if (a.sign) begin
          r.v = qnan_indef(f);
          r.x[XIE] = 1'b1;
          return r;
        end
        if (a.inf) begin
          r.v = av;
          return r;
        end
        begin
          // sqrt(sig * 2^(exp-63)): make the exponent even, take a 128-bit root
          // value = sig * 2^t, t = exp - 63; make t even, then
          // sqrt = isqrt(sig * 2^190) * 2^((t - 190) / 2)
          logic [255:0] rad;
          logic [127:0] root;
          logic signed [19:0] t;
          logic remz;
          t = 20'(a.exp) - 63;
          rad = {2'b0, a.sig, 190'b0};
          if (t[0]) begin
            rad = rad << 1;
            t = t - 1;
          end
          root = isqrt256(rad, remz);
          r = merge_x(round_pack(1'b0, (t >>> 1) + 32, root, !remz, f, rm, ftz, prec), r.x);
        end
      end
      default: ;
    endcase
    return r;
  endfunction

  function automatic fres_t merge_x(input fres_t r, input logic [5:0] x);
    r.x = r.x | x;
    return r;
  endfunction

  function automatic logic [79:0] pack_inf(input logic s, input logic [3:0] f);
    int fb, eb;
    fb = fbits(f);
    eb = ebits(f);
    if (f == FMT_F80) return {s, 15'h7FFF, 1'b1, 63'b0};
    return (80'(s) << (fb + eb)) | (80'((1 << eb) - 1) << fb);
  endfunction

  // a < b for ordered, non-NaN operands
  // operands are passed as whole unpacked values; this function reads some fields
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic cmp_lt(input unpacked_t a, input unpacked_t b);
  /* verilator lint_on UNUSEDSIGNAL */
    if (a.zero && b.zero) return 1'b0;
    if (a.sign != b.sign) return a.sign && !(a.zero && b.zero);
    if (a.inf || b.inf) begin
      if (a.inf && b.inf) return 1'b0;
      return a.sign ? a.inf : b.inf;
    end
    if (a.zero) return !b.sign;
    if (b.zero) return a.sign;
    if (a.exp != b.exp) return a.sign ? (a.exp > b.exp) : (a.exp < b.exp);
    return a.sign ? (a.sig > b.sig) : (a.sig < b.sig);
  endfunction

  // a + b (signs included), exactly enough, one rounding
  // operands are passed as whole unpacked values; this function reads some fields
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic fres_t add_core(input unpacked_t a, input unpacked_t b,
                                     input logic [3:0] f, input logic [1:0] rm, input logic ftz,
                                     input int prec);
  /* verilator lint_on UNUSEDSIGNAL */
    fres_t r;
    logic [255:0] x, y, z;
    logic signed [19:0] e;
    int d;
    logic sx, sy, sz, st;
    r = '0;
    if (a.inf || b.inf) begin
      if (a.inf && b.inf && a.sign != b.sign) begin
        r.v = qnan_indef(f);
        r.x[XIE] = 1'b1;
        return r;
      end
      r.v = pack_inf(a.inf ? a.sign : b.sign, f);
      return r;
    end
    if (a.zero && b.zero) begin
      // -0 + -0 = -0; otherwise +0, or -0 rounding down
      r.v = 80'((a.sign && b.sign) || (a.sign != b.sign && rm == 2'd1)) << (width(f) - 1);
      return r;
    end
    if (a.zero) return round_pack(b.sign, 20'(b.exp) + 64, {64'b0, b.sig}, 1'b0, f, rm, ftz, prec);
    if (b.zero) return round_pack(a.sign, 20'(a.exp) + 64, {64'b0, a.sig}, 1'b0, f, rm, ftz, prec);
    // align on the larger exponent; 192 guard bits below the significand
    if (a.exp >= b.exp) begin
      e = 20'(a.exp);
      d = int'(a.exp) - int'(b.exp);
      x = {a.sig, 192'b0};
      y = {b.sig, 192'b0};
      sx = a.sign;
      sy = b.sign;
    end else begin
      e = 20'(b.exp);
      d = int'(b.exp) - int'(a.exp);
      x = {b.sig, 192'b0};
      y = {a.sig, 192'b0};
      sx = b.sign;
      sy = a.sign;
    end
    st = 1'b0;
    if (d > 250) begin
      st = 1'b1;
      y = 0;
    end else begin
      st = (y & ((256'd1 << d) - 1)) != 0;
      y = y >> d;
    end
    // one guard bit at the top for the carry
    x = x >> 1;
    y = y >> 1;
    if (sx == sy) begin
      z = x + y;
      sz = sx;
    end else if (x >= y) begin
      z = x - y - (st ? 256'd1 : 256'd0);
      sz = sx;
    end else begin
      z = y - x;
      sz = sy;
    end
    if (z == 0 && !st) begin
      r.v = 80'(rm == 2'd1) << (width(f) - 1);
      return r;
    end
    // value = z * 2^(e + 1 - 255) (bit 254 holds the original leading 1)
    return round_pack(sz, e + 1 + 127 - 254 + 127, z[255:128], st || (z[127:0] != 0),
                      f, rm, ftz, prec);
  endfunction

  // (a*b) + c exactly, then round once
  // operands are passed as whole unpacked values; this function reads some fields
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic fres_t fma_core(input unpacked_t a, input unpacked_t b, input unpacked_t c,
                                     input logic [3:0] f, input logic [1:0] rm, input logic ftz,
                                     input int prec);
  /* verilator lint_on UNUSEDSIGNAL */
    logic [127:0] ps;
    fres_t r;
    if (a.zero || b.zero) begin
      unpacked_t z;
      z = '0;
      z.zero = 1'b1;
      z.sign = a.sign ^ b.sign;
      return add_core(z, c, f, rm, ftz, prec);
    end
    ps = 128'(a.sig) * 128'(b.sig);              // value ps * 2^(ea+eb-126)
    // as an unpacked value with a 128-bit significand: use add on the wide form
    begin
      logic [255:0] x, y, z;
      logic signed [19:0] ep, ec, e;
      int d;
      logic sx, sy, sz, st;
      // normalize the product so its leading 1 is at bit 255 of x
      ep = 20'(a.exp) + 20'(b.exp) + 1;          // ps has its leading 1 at bit 127 or 126
      x = {ps, 128'b0};
      if (!x[255]) begin
        x = x << 1;
        ep = ep - 1;
      end
      if (c.zero) return round_pack(a.sign ^ b.sign, ep, x[255:128], x[127:0] != 0,
                                    f, rm, ftz, prec);
      ec = 20'(c.exp);
      y = {c.sig, 192'b0};
      sx = a.sign ^ b.sign;
      sy = c.sign;
      if (ec > ep) begin
        logic [255:0] t;
        logic ts;
        t = x; x = y; y = t;
        ts = sx; sx = sy; sy = ts;
        e = ec;
        d = int'(ec) - int'(ep);
      end else begin
        e = ep;
        d = int'(ep) - int'(ec);
      end
      if (d > 250) begin
        st = y != 0;
        y = 0;
      end else begin
        st = (y & ((256'd1 << d) - 1)) != 0;
        y = y >> d;
      end
      x = x >> 1;
      y = y >> 1;
      if (sx == sy) begin
        z = x + y;
        sz = sx;
      end else if (x >= y) begin
        z = x - y - (st ? 256'd1 : 256'd0);
        sz = sx;
      end else begin
        z = y - x;
        sz = sy;
      end
      if (z == 0 && !st) begin
        r = '0;
        r.v = 80'(rm == 2'd1) << (width(f) - 1);
        return r;
      end
      return round_pack(sz, e + 1 + 127 - 254 + 127, z[255:128], st || (z[127:0] != 0),
                        f, rm, ftz, prec);
    end
  endfunction

  // integer square root of a 256-bit radicand (bit by bit); rem_zero: exact
  function automatic logic [127:0] isqrt256(input logic [255:0] rad, output logic rem_zero);
    logic [255:0] rem;
    logic [127:0] root;
    logic [257:0] trial;
    rem = 0;
    root = 0;
    for (int i = 127; i >= 0; i--) begin
      rem = (rem << 2) | 256'((rad >> (2 * i)) & 256'd3);
      trial = {128'b0, root, 2'b01};
      if (258'(rem) >= trial) begin
        rem = 256'(258'(rem) - trial);
        root = (root << 1) | 128'd1;
      end else root = root << 1;
    end
    rem_zero = rem == 0;
    return root;
  endfunction

  // format conversion (CVTSS2SD, CVTSD2SS, x87 loads/stores, F16C, BF16)
  function automatic fres_t fp_convert(input logic [79:0] av, input logic [3:0] from,
                                       input logic [3:0] to, input logic [1:0] rm,
                                       input logic daz, input logic ftz, input int prec);
    fres_t r;
    unpacked_t a;
    a = unpack(av, from, daz);
    r = '0;
    if (a.nan) begin
      r.x[XIE] = a.snan;
      r.v = pack_nan(a, to);
      return r;
    end
    if (a.inf) begin
      r.v = pack_inf(a.sign, to);
      return r;
    end
    if (a.zero) begin
      r.v = 80'(a.sign) << (width(to) - 1);
      return r;
    end
    r = round_pack(a.sign, 20'(a.exp) + 64, {64'b0, a.sig}, 1'b0, to, rm, ftz, prec);
    r.x[XDE] = r.x[XDE] | a.denorm;
    return r;
  endfunction

  // integer -> float (CVTSI2SS/SD, FILD, VCVTUDQ2PS...); width in bits, signedness
  function automatic fres_t fp_from_int(input logic [63:0] iv, input int iw, input logic is_signed,
                                        input logic [3:0] to, input logic [1:0] rm, input int prec);
    logic [63:0] m;
    logic s;
    logic [63:0] v;
    fres_t r;
    v = (iw == 64) ? iv : iv & ((64'd1 << iw) - 1);
    s = is_signed && v[iw - 1];
    m = s ? (64'd0 - (is_signed && iw < 64 ? (v | ~((64'd1 << iw) - 1)) : v)) : v;
    if (m == 0) begin
      r = '0;
      return r;
    end
    return round_pack(s, 20'd64 + 63, {64'b0, m}, 1'b0, to, rm, 1'b0, prec);
  endfunction

  // float -> integer (CVTSS2SI, CVTTSD2SI, FIST...): rm, or truncation; out of
  // range or NaN: the "integer indefinite" (the most negative value; for
  // unsigned conversions all ones) and IE
  function automatic fres_t fp_to_int(input logic [79:0] av, input logic [3:0] from,
                                      input int iw, input logic is_signed, input logic [1:0] rm,
                                      input logic daz);
    fres_t r;
    // the integer conversion reads sign, exponent, significand and the class bits
    /* verilator lint_off UNUSEDSIGNAL */
    unpacked_t a;
    /* verilator lint_on UNUSEDSIGNAL */
    logic [127:0] x;
    logic [127:0] keep, rest, half;
    logic up, inexact;
    int sh;
    logic [127:0] mag;
    a = unpack(av, from, daz);
    r = '0;
    if (a.nan || a.inf) begin
      r.v = is_signed ? (80'd1 << (iw - 1)) : ((80'd1 << iw) - 80'd1);
      if (iw == 64 && !is_signed) r.v = 80'(64'hFFFF_FFFF_FFFF_FFFF);
      r.x[XIE] = 1'b1;
      return r;
    end
    if (a.zero) return r;
    r.x[XDE] = a.denorm;
    // value = sig * 2^(exp - 63); integer part = sig >> (63 - exp)
    x = {a.sig, 64'b0};                       // binary point at bit 64
    if (a.exp > 63) begin
      r.v = is_signed ? (80'd1 << (iw - 1)) : ((80'd1 << iw) - 80'd1);
      if (iw == 64 && !is_signed) r.v = 80'(64'hFFFF_FFFF_FFFF_FFFF);
      r.x[XIE] = 1'b1;
      return r;
    end
    sh = 63 - int'(a.exp);
    if (sh > 127) x = {127'b0, x != 0};
    else x = x >> sh;
    keep = x >> 64;
    rest = x & ((128'd1 << 64) - 1);
    half = 128'd1 << 63;
    inexact = rest != 0;
    case (rm)
      2'd0: up = rest > half || (rest == half && keep[0]);
      2'd1: up = a.sign && inexact;
      2'd2: up = !a.sign && inexact;
      default: up = 1'b0;
    endcase
    mag = keep + (up ? 128'd1 : 128'd0);
    r.x[XPE] = inexact;
    if (is_signed) begin
      if ((!a.sign && mag > ((128'd1 << (iw - 1)) - 1)) || (a.sign && mag > (128'd1 << (iw - 1)))) begin
        r.v = (80'd1 << (iw - 1));
        r.x[XIE] = 1'b1;
        r.x[XPE] = 1'b0;
        return r;
      end
      begin
        logic [63:0] iv;
        iv = a.sign ? (64'd0 - mag[63:0]) : mag[63:0];
        r.v = {16'b0, iv};
        if (iw < 64) r.v = r.v & ((80'd1 << iw) - 1);
      end
    end else begin
      if (a.sign && mag != 0 || mag > ((128'd1 << iw) - 1)) begin
        r.v = (iw == 64) ? 80'(64'hFFFF_FFFF_FFFF_FFFF) : ((80'd1 << iw) - 80'd1);
        r.x[XIE] = 1'b1;
        r.x[XPE] = 1'b0;
        return r;
      end
      r.v = 80'(mag[63:0]);
    end
    return r;
  endfunction

  // comparisons: the 32 AVX predicates (VCMPPS imm8[4:0], Intel SDM Vol. 2A
  // Table 3-1), and the COMISS/UCOMISS flags; IE for SNaN (and QNaN for the
  // signaling predicates)
  function automatic logic fp_cmp(input logic [79:0] av, input logic [79:0] bv, input logic [3:0] f,
                                  input logic [4:0] pred, input logic daz, output logic [5:0] x);
    unpacked_t a, b;
    logic un, eq, lt, gt, res, sig;
    a = unpack(av, f, daz);
    b = unpack(bv, f, daz);
    un = a.nan || b.nan;
    eq = !un && ((a.zero && b.zero) || (a.sign == b.sign && a.exp == b.exp && a.sig == b.sig &&
                                         a.inf == b.inf && !a.zero && !b.zero) ||
                 (a.inf && b.inf && a.sign == b.sign));
    lt = !un && !eq && cmp_lt(a, b);
    gt = !un && !eq && !lt;
    // predicates 0-7 and their +8 / +16 / +24 variants
    case (pred[2:0])
      3'd0: res = eq;                 // EQ
      3'd1: res = lt;                 // LT
      3'd2: res = lt || eq;           // LE
      3'd3: res = un;                 // UNORD
      3'd4: res = !eq;                // NEQ (unordered: true)
      3'd5: res = !lt;                // NLT
      3'd6: res = !(lt || eq);        // NLE
      default: res = !un;             // ORD
    endcase
    case (pred[4:3])
      2'd1: begin
        // EQ_UQ, NGE, NGT, FALSE, NEQ_OQ, GE, GT, TRUE
        case (pred[2:0])
          3'd0: res = eq || un;
          3'd1: res = !(gt || eq);
          3'd2: res = !gt;
          3'd3: res = 1'b0;
          3'd4: res = !eq && !un;
          3'd5: res = gt || eq;
          3'd6: res = gt;
          default: res = 1'b1;
        endcase
      end
      default: ;
    endcase
    // signaling predicates: 1, 2, 5, 6, 9, 10, 13, 14 and 16-31 swap quiet/signaling
    sig = (pred[2:0] == 3'd1 || pred[2:0] == 3'd2 || pred[2:0] == 3'd5 || pred[2:0] == 3'd6);
    if (pred[4]) sig = !sig;
    x = '0;
    x[XIE] = a.snan || b.snan || (sig && un);
    x[XDE] = a.denorm || b.denorm;
    return res;
  endfunction

  // round to an integral value (ROUNDSS/SD, VRNDSCALE scale 0, FRNDINT)
  function automatic fres_t fp_round_int(input logic [79:0] av, input logic [3:0] f,
                                         input logic [1:0] rm, input logic suppress_pe,
                                         input logic daz);
    fres_t r;
    unpacked_t a;
    logic [127:0] x, keep, rest, half;
    logic up;
    int sh;
    a = unpack(av, f, daz);
    r = '0;
    if (a.nan) begin
      r.x[XIE] = a.snan;
      r.v = pack_nan(a, f);
      return r;
    end
    if (a.inf || a.zero || a.exp >= 63) begin
      r.v = av;
      return r;
    end
    x = {a.sig, 64'b0};
    sh = 63 - int'(a.exp);
    if (sh > 127) x = {127'b0, x != 0};
    else x = x >> sh;
    keep = x >> 64;
    rest = x & ((128'd1 << 64) - 1);
    half = 128'd1 << 63;
    case (rm)
      2'd0: up = rest > half || (rest == half && keep[0]);
      2'd1: up = a.sign && rest != 0;
      2'd2: up = !a.sign && rest != 0;
      default: up = 1'b0;
    endcase
    keep = keep + (up ? 128'd1 : 128'd0);
    if (keep == 0) r.v = 80'(a.sign) << (width(f) - 1);
    else r.v = round_pack(a.sign, 20'd127, keep, 1'b0, f, 2'd3, 1'b0, 64).v;
    r.x[XPE] = (rest != 0) && !suppress_pe;
    return r;
  endfunction
endpackage
