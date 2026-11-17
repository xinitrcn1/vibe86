// tb_x87_ref: v1's x87 unit (v1_x87_pkg) against the host's x87
// (ref_x87.c through DPI-C), on random and special operands under random
// rounding and precision control. Arithmetic, conversions, comparisons,
// FPREM/FPREM1, FSCALE, FXTRACT, FRNDINT, FXAM and the constants must match
// bit for bit (result, IE DE ZE OE UE PE, and the condition codes each
// instruction defines). The transcendentals must be within 1 ulp of the
// host's (Intel's are not correctly rounded either: SDM Vol. 1 8.3.10 and
// the instruction pages claim 1 ulp), with the same IE/ZE/DE and C2.
module tb_x87_ref;
  import v1_fp_pkg::*;
  import v1_x87_pkg::*;

  import "DPI-C" function void ref_x87(input int op, input int fcw, input longint unsigned alo,
                                       input int ahi, input longint unsigned blo, input int bhi,
                                       output longint unsigned rlo, output int rhi,
                                       output longint unsigned r2lo, output int r2hi,
                                       output int fsw);

  int errors, checks, ulp1;
  int operr [34];

  function automatic logic [63:0] rnd64();
    return {$urandom(), $urandom()};
  endfunction

  // an operand of a random class; dom: 0 any, 1 |x| <= 1, 2 |x| < 0.29,
  // 3 trigonometric (|x| < 2^64, near multiples of pi/2 too), 4 near 1
  function automatic logic [79:0] operand(input int dom);
    logic [79:0] v;
    logic [63:0] s;
    int c;
    logic sg;
    s = rnd64();
    sg = $urandom_range(0, 1) == 1;
    c = $urandom_range(0, 99);
    case (dom)
      1: begin
        if (c < 5) return {sg, 79'b0};
        return {sg, 15'(16383 - $urandom_range(1, 70)), 1'b1, s[62:0]};
      end
      2: begin
        if (c < 5) return {sg, 79'b0};
        return {sg, 15'(16383 - $urandom_range(2, 70)), 1'b1, s[62:0]};
      end
      3: begin
        if (c < 10) begin
          // a multiple of the 66-bit pi/2 (rounded), the hard case
          logic [255:0] k;
          k = 256'($urandom_range(1, 4000)) * 256'(K_PI66);
          v = round_pack(sg, 20'(127 - 65), 128'(k), 1'b0, FMT_F80, 2'd0, 1'b0, 64).v;
          return v;
        end
        if (c < 13) return {sg, 15'(16383 + 63 + $urandom_range(0, 3)), 1'b1, s[62:0]};
        return {sg, 15'(16383 + $urandom_range(0, 70) - 30), 1'b1, s[62:0]};
      end
      4: return {1'b0, 15'(16383 - $urandom_range(0, 1)), 1'b1, s[62:0]};
      default: ;
    endcase
    if (c < 45) v = {sg, 15'(16383 + $urandom_range(0, 80) - 40), 1'b1, s[62:0]};
    else if (c < 55) v = {sg, 15'($urandom_range(1, 32766)), 1'b1, s[62:0]};
    else if (c < 60) v = {sg, 15'h0, 1'b0, s[62:0] >> $urandom_range(0, 62)};   // denormal
    else if (c < 62) v = {sg, 15'h0, 1'b1, s[62:0]};                              // pseudo-denormal
    else if (c < 66) v = {sg, 79'b0};
    else if (c < 70) v = {sg, 15'h7FFF, 1'b1, 63'b0};
    else if (c < 73) v = {sg, 15'h7FFF, 2'b11, s[61:0]};                         // QNaN
    else if (c < 76) v = {sg, 15'h7FFF, 2'b10, s[61:0] | 62'd1};                 // SNaN
    else if (c < 78) v = {sg, 15'($urandom_range(1, 32766)), 1'b0, s[62:0]};     // unnormal
    else if (c < 90) v = fp_from_int(64'($signed(16'(s[15:0]))), 64, 1'b1, FMT_F80, 2'd0, 64).v;
    else v = {sg, 15'(16383 + $urandom_range(0, 8)), 1'b1, s[62:0] & 63'h7FFF_0000_0000_0000};  // 16 significant bits
    return v;
  endfunction

  function automatic logic [3:0] ccof(input int fsw);
    return {fsw[14], fsw[10], fsw[9], fsw[8]};
  endfunction

  // ulps between two finite f80 values of the same sign
  function automatic longint unsigned ulps(input logic [79:0] a, input logic [79:0] b);
    logic [78:0] x, y;
    x = a[78:0];
    y = b[78:0];
    if (a[79] != b[79]) return (x == 0 && y == 0) ? 0 : 64'hFFFF;
    // f80 magnitudes are monotonic in {exp, sig} for normals; close enough here
    return (x > y) ? 64'(x - y) : 64'(y - x);
  endfunction

  task automatic check(input int op, input logic [79:0] a, input logic [79:0] b,
                       input logic [15:0] fcw);
    xres_t m, m2;
    longint unsigned rlo, r2lo;
    int rhi, r2hi, fsw;
    logic [79:0] rv, rv2;
    logic [5:0] fx;
    logic [3:0] cmask;
    bit trans, ok;
    m2 = '0;
    ref_x87(op, int'(fcw), a[63:0], int'(a[79:64]), b[63:0], int'(b[79:64]), rlo, rhi, r2lo, r2hi, fsw);
    rv = {rhi[15:0], rlo};
    rv2 = {r2hi[15:0], r2lo};
    fx = fsw[5:0];
    trans = op >= 10 && op <= 16;
    cmask = 4'b0010;                       // C1 (rounding) by default
    case (op)
      0, 1, 2, 3, 4: m = x87_arith(3'(op), a, b, fcw);
      5: m = x87_rndint(a, fcw);
      6: m = x87_scale(a, b, fcw);
      7, 8: begin
        m = x87_prem(a, b, op == 8);
        cmask = m.cc[2] ? 4'b0100 : 4'b1111;  // a partial remainder leaves C0/C1/C3 undefined
      end
      9: begin
        m = x87_xtract(a, 1'b0);
        m2 = x87_xtract(a, 1'b1);
        cmask = 4'b0000;
      end
      10: m = x87_trans(3'd0, a, b, fcw);
      11: m = x87_trans(3'd1, a, b, fcw);
      12: m = x87_trans(3'd2, a, b, fcw);
      13: m = x87_trans(3'd3, a, b, fcw);
      14, 15, 16: begin
        m = x87_trans(3'(op - 10), a, b, fcw);
        cmask = 4'b0100;
        if (m.nowr) m.v = a;
      end
      17, 18: begin
        m = x87_compare(a, b, op == 18);
        cmask = 4'b1111;
        m.v = a;
      end
      19: begin
        m = '0;
        m.cc = x87_xam(a, 1'b0);
        m.v = a;
        cmask = 4'b1111;
      end
      20, 21: m = x87_load(3'(op - 20), a[63:0], 16'h0, fcw);
      22, 23, 24: m = x87_load(3'(op - 19), a[63:0], 16'h0, fcw);
      25: m = x87_load(3'd6, a[63:0], a[79:64], fcw);
      26, 27: begin
        m = x87_store(3'(op - 26), a, fcw, 1'b0, 1'b0);
        m.v = {16'b0, m.v[63:0]};
        rv = {16'b0, rlo};
      end
      28, 29, 30, 32: begin
        m = x87_store(op == 28 ? 3'd3 : op == 29 ? 3'd4 : 3'd5, a, fcw, op == 32, 1'b0);
        m.v = {16'b0, m.v[63:0]};
        rv = {16'b0, rlo};
      end
      31: begin
        m = x87_store(3'd6, a, fcw, 1'b0, 1'b0);
        m2 = x87_store(3'd6, a, fcw, 1'b0, 1'b1);
        m.v = {m2.v[15:0], m.v[63:0]};
        m2 = '0;
      end
      default: begin
        m = x87_load(3'd7, a[63:0], 16'h0, fcw);
        cmask = 4'b0000;
      end
    endcase
    if (op == 9) rv2 = rv2;
    checks++;
    ok = 1'b1;
    if (trans && !(x87_isnan(rv) || x87_isnan(m.v))) begin
      longint unsigned u;
      u = ulps(m.v, rv);
      if (u > 1) ok = 1'b0;
      else if (u == 1) ulp1++;
      if (m.x[XIE] != fx[XIE] || m.x[XZE] != fx[XZE] || m.x[XDE] != fx[XDE]) ok = 1'b0;
    end else begin
      if (m.v != rv) ok = 1'b0;
      if (op == 9 && m2.v != rv2) ok = 1'b0;
      if (m.x != fx) ok = 1'b0;
    end
    if (op == 16 && !m.nowr && !x87_isnan(rv) && rv2 != X87_ONE) ok = 1'b0;
    if ((m.cc & cmask) != (ccof(fsw) & cmask)) ok = 1'b0;
    if (!ok) begin
      errors++;
      operr[op]++;
      if (operr[op] <= 3)
        $display("MISMATCH op %0d fcw %h a %h b %h: v1 %h x %b cc %b%s | host %h x %b cc %b",
                 op, fcw, a, b, m.v, m.x, m.cc & cmask, op == 9 ? $sformatf(" exp %h", m2.v) : "",
                 rv, fx, ccof(fsw) & cmask);
    end
  endtask

  initial begin
    int n;
    errors = 0;
    checks = 0;
    ulp1 = 0;
    for (int op = 0; op <= 33; op++) operr[op] = 0;
    n = 3000;
    for (int op = 0; op <= 33; op++) begin
      for (int i = 0; i < n; i++) begin
        logic [79:0] a, b;
        logic [15:0] fcw;
        int dom;
        fcw = {4'b0000, 2'($urandom_range(0, 3)), (op <= 4) ? 2'($urandom_range(0, 3)) : 2'b11, 8'h7F};
        if (fcw[9:8] == 2'b01) fcw[9:8] = 2'b11;
        dom = (op == 10) ? 1 : (op == 12) ? 2 : (op >= 14 && op <= 16) ? 3 : (op == 11 && i % 3 == 0) ? 4 : 0;
        a = operand(dom);
        b = operand(op == 13 ? 0 : 0);
        if (op >= 20 && op <= 24) begin
          logic [63:0] s;
          s = rnd64();
          if (op == 20 && i % 4 == 0) s[30:23] = 8'h00;           // f32 denormals
          if (op == 20 && i % 7 == 0) s[30:23] = 8'hFF;           // inf/NaN
          if (op == 21 && i % 4 == 0) s[62:52] = 11'h000;
          if (op == 21 && i % 7 == 0) s[62:52] = 11'h7FF;
          a = {16'b0, s};
        end
        if (op == 25) begin
          logic [79:0] d;
          d = '0;
          for (int k = 0; k < 18; k++) d[4 * k +: 4] = 4'($urandom_range(0, 9));
          d[79] = $urandom_range(0, 1) == 1;
          a = d;
        end
        if (op == 31 && i % 2 == 0)
          a = fp_from_int(rnd64() >> $urandom_range(4, 63), 64, 1'b1, FMT_F80, 2'd0, 64).v;
        if (op == 33) a = {77'b0, 3'($urandom_range(0, 6))};
        check(op, a, b, fcw);
      end
    end
    for (int op = 0; op <= 33; op++) if (operr[op] != 0) $display("  op %0d: %0d errors", op, operr[op]);
    $display("tb_x87_ref: %0d checks, %0d errors, %0d transcendental results 1 ulp off",
             checks, errors, ulp1);
    if (errors != 0) $fatal(1, "tb_x87_ref: FAILED");
    $finish;
  end
endmodule
