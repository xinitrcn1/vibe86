// tb_fpu_ref: v1_fp_pkg's f32/f64 arithmetic against the host CPU's SSE
// (ref_x86.c, DPI-C) on random and edge-case operands: zeros, infinities,
// quiet and signalling NaNs with payloads, the smallest/largest subnormals
// and normals, values next to 1, operands whose results land on the
// overflow and underflow thresholds, and random bits; every op (add, sub,
// mul, div, sqrt, FMA, min, max), all four rounding modes, DAZ and FTZ.
// Results and the MXCSR exception flags must match bit for bit, except for
// which NaN an FMA with two or more NaN inputs returns (then: any NaN).
module tb_fpu_ref;
  import v1_fp_pkg::*;

  import "DPI-C" function void ref_f32(input int op, input int rm, input int daz, input int ftz,
                                       input int unsigned a, input int unsigned b,
                                       input int unsigned c, output int unsigned r,
                                       output int flags);
  import "DPI-C" function void ref_f64(input int op, input int rm, input int daz, input int ftz,
                                       input longint unsigned a, input longint unsigned b,
                                       input longint unsigned c, output longint unsigned r,
                                       output int flags);

  int errors = 0, checks = 0;
  int seed = 1;

  // an operand of format f (f32 if dbl = 0): a special class or random bits
  function automatic logic [63:0] operand(input bit dbl);
    int k;
    logic [63:0] v;
    int eb = dbl ? 11 : 8, fb = dbl ? 52 : 23;
    logic [63:0] fr, ex, s;
    k = $urandom_range(0, 15);
    fr = {$urandom, $urandom} & ((64'd1 << fb) - 1);
    s = 64'($urandom_range(0, 1)) << (eb + fb);
    case (k)
      0: ex = 0;                                    // zero or subnormal
      1: begin ex = 0; fr = 0; end                  // zero
      2: begin ex = (64'd1 << eb) - 1; fr = 0; end  // infinity
      3: begin ex = (64'd1 << eb) - 1; fr = fr | (64'd1 << (fb - 1)); end   // QNaN
      4: begin ex = (64'd1 << eb) - 1; fr = (fr & ~(64'd1 << (fb - 1))) | 1; end  // SNaN
      5: begin ex = 0; fr = 1; end                  // smallest subnormal
      6: begin ex = 0; fr = (64'd1 << fb) - 1; end  // largest subnormal
      7: ex = 1;                                    // smallest normals
      8: ex = (64'd1 << eb) - 2;                    // largest normals
      9: begin ex = (64'd1 << (eb - 1)) - 1; fr = fr & 64'h7; end  // next to 1
      10: ex = (64'd1 << (eb - 1)) - 1 + 64'($urandom_range(0, 3)) - 1;
      11: ex = 64'($urandom_range(1, 4));           // near the underflow threshold
      12: ex = (64'd1 << eb) - 2 - 64'($urandom_range(0, 3));   // near overflow
      13: ex = (64'd1 << (eb - 1)) - 1 + (64'd1 << (eb - 2)) - 64'($urandom_range(0, 2)); // squares overflow
      14: ex = (64'd1 << (eb - 2)) + 64'($urandom_range(0, 2));  // squares underflow
      default: ex = 64'($urandom_range(1, (1 << eb) - 2));
    endcase
    v = s | (ex << fb) | fr;
    return v;
  endfunction

  function automatic bit is_nan(input logic [63:0] v, input bit dbl);
    return dbl ? (v[62:52] == 11'h7FF && v[51:0] != 0) : (v[30:23] == 8'hFF && v[22:0] != 0);
  endfunction

  task automatic one(input bit dbl, input int op, input int rm, input int daz, input int ftz);
    logic [63:0] a, b, c, want;
    int wf;
    fres_t got;
    int nnan;
    a = operand(dbl);
    b = operand(dbl);
    c = operand(dbl);
    if (!dbl) begin
      int unsigned r32;
      a[63:32] = 0; b[63:32] = 0; c[63:32] = 0;
      ref_f32(op, rm, daz, ftz, a[31:0], b[31:0], c[31:0], r32, wf);
      want = {32'b0, r32};
      got = fp_arith(4'(op), 80'(a), 80'(b), 80'(c), FMT_F32, 2'(rm), daz[0], ftz[0], 24, 2'b0);
    end else begin
      longint unsigned r64;
      ref_f64(op, rm, daz, ftz, a, b, c, r64, wf);
      want = r64;
      got = fp_arith(4'(op), 80'(a), 80'(b), 80'(c), FMT_F64, 2'(rm), daz[0], ftz[0], 53, 2'b0);
    end
    checks++;
    nnan = int'(is_nan(a, dbl)) + int'(is_nan(b, dbl)) + int'(op == 5 && is_nan(c, dbl));
    if (op == 5 && nnan >= 2 && is_nan(want, dbl) && is_nan(got.v[63:0], dbl))
      want = got.v[63:0];
    if (got.v[63:0] !== want || 6'(wf) !== got.x) begin
      errors++;
      if (errors <= 60)
        $display("FAIL %s op %0d rm %0d daz %0d ftz %0d: a %h b %h c %h -> %h flags %b, host %h flags %b",
                 dbl ? "f64" : "f32", op, rm, daz, ftz, a, b, c, got.v[63:0], got.x, want, 6'(wf));
    end
  endtask

  initial begin
    void'($urandom(seed));
    for (int dbl = 0; dbl < 2; dbl++)
      for (int op = 0; op < 8; op++)
        for (int rm = 0; rm < 4; rm++)
          for (int mode = 0; mode < 3; mode++)        // plain, DAZ, FTZ
            for (int i = 0; i < 400; i++)
              one(dbl[0], op, rm, int'(mode == 1), int'(mode == 2));
    if (errors != 0) $fatal(1, "tb_fpu_ref: %0d of %0d checks differ from the host", errors, checks);
    $display("tb_fpu_ref: %0d checks agree with the host", checks);
    $finish;
  end
endmodule
