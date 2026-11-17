// tb_fpu: IEEE 754 results and x86 exception flags of v1_fp_pkg.
// Expected values are what an x86 CPU (and IEEE 754-2019 round-to-nearest-
// even) gives; flags are {PE UE OE ZE DE IE} (bit 5..0).
module tb_fpu;
  import v1_fp_pkg::*;
  int errors = 0, checks = 0;

  task automatic chk(input string name, input fres_t r, input logic [79:0] want,
                     input logic [5:0] wantx, input logic [5:0] xmask = 6'h3F);
    checks++;
    if (r.v !== want || ((r.x ^ wantx) & xmask) != 0) begin
      errors++;
      $display("FAIL %s: %h flags %b (want %h flags %b)", name, r.v, r.x, want, wantx);
    end
  endtask

  // flag bits
  localparam logic [5:0] IE = 6'h01, DE = 6'h02, ZE = 6'h04, OE = 6'h08, UE = 6'h10, PE = 6'h20;

  // The test vectors give f16/f32/f64 operands and results as their own
  // width; the datapath takes them zero-extended in 80 bits (an f80 holds
  // the narrower formats in its low bits), which is what the widening does.
  /* verilator lint_off WIDTHEXPAND */
  initial begin
    fres_t r;
    logic [5:0] x;
    logic c;
    // f32 arithmetic
    chk("1+2", fp_arith(0, 32'h3f800000, 32'h40000000, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h40400000, 0);
    chk("0.1+0.2", fp_arith(0, 32'h3dcccccd, 32'h3e4ccccd, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h3e99999a, PE);
    chk("1-1", fp_arith(1, 32'h3f800000, 32'h3f800000, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h00000000, 0);
    chk("1-1 rd", fp_arith(1, 32'h3f800000, 32'h3f800000, 0, FMT_F32, 1, 0, 0, 24, 0), 32'h80000000, 0);
    chk("3*7", fp_arith(2, 32'h40400000, 32'h40e00000, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h41a80000, 0);
    chk("1/3 f32", fp_arith(3, 32'h3f800000, 32'h40400000, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h3eaaaaab, PE);
    chk("sqrt2 f32", fp_arith(4, 32'h40000000, 0, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h3fb504f3, PE);
    chk("sqrt4", fp_arith(4, 32'h40800000, 0, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h40000000, 0);
    // f64
    chk("1/3 f64", fp_arith(3, 64'h3ff0000000000000, 64'h4008000000000000, 0, FMT_F64, 0, 0, 0, 53, 0),
        64'h3fd5555555555555, PE);
    chk("sqrt2 f64", fp_arith(4, 64'h4000000000000000, 0, 0, FMT_F64, 0, 0, 0, 53, 0),
        64'h3ff6a09e667f3bcd, PE);
    chk("1e308*10", fp_arith(2, 64'h7fe1ccf385ebc8a0, 64'h4024000000000000, 0, FMT_F64, 0, 0, 0, 53, 0),
        64'h7ff0000000000000, OE | PE);
    // FMA: 0.1*10 - 1 exactly (not 0 as the rounded product would give)
    chk("fma", fp_arith(5, 64'h3fb999999999999a, 64'h4024000000000000, 64'hbff0000000000000, FMT_F64,
                        0, 0, 0, 53, 0), 64'h3c90000000000000, 0);
    // specials
    chk("0/0", fp_arith(3, 32'h0, 32'h0, 0, FMT_F32, 0, 0, 0, 24, 0), 32'hffc00000, IE);
    chk("1/0", fp_arith(3, 32'h3f800000, 32'h0, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h7f800000, ZE);
    chk("inf-inf", fp_arith(1, 32'h7f800000, 32'h7f800000, 0, FMT_F32, 0, 0, 0, 24, 0), 32'hffc00000, IE);
    chk("snan+1", fp_arith(0, 32'h7f800001, 32'h3f800000, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h7fc00001, IE);
    chk("qnan, snan", fp_arith(0, 32'h7fc00002, 32'hff800001, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h7fc00002, IE);
    chk("sqrt(-1)", fp_arith(4, 32'hbf800000, 0, 0, FMT_F32, 0, 0, 0, 24, 0), 32'hffc00000, IE);
    // subnormals
    chk("min sub / 2", fp_arith(3, 32'h00000001, 32'h40000000, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h00000000, UE | PE | DE);
    chk("sub + sub", fp_arith(0, 32'h00000001, 32'h00000001, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h00000002, DE);
    chk("min normal / 2", fp_arith(3, 32'h00800000, 32'h40000000, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h00400000, 0);
    chk("ftz", fp_arith(3, 32'h00800001, 32'h40000000, 0, FMT_F32, 0, 0, 1, 24, 0), 32'h00000000, UE | PE);
    // min / max (SSE: the second source if unordered or both zero)
    chk("min", fp_arith(6, 32'h3f800000, 32'h40000000, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h3f800000, 0);
    chk("max 0,-0", fp_arith(7, 32'h00000000, 32'h80000000, 0, FMT_F32, 0, 0, 0, 24, 0), 32'h80000000, 0);
    // conversions
    chk("f32->f64", fp_convert(32'h3fc00000, FMT_F32, FMT_F64, 0, 0, 0, 53), 64'h3ff8000000000000, 0);
    chk("f64->f32 1/3", fp_convert(64'h3fd5555555555555, FMT_F64, FMT_F32, 0, 0, 0, 24), 32'h3eaaaaab, PE);
    chk("f32->f16 1", fp_convert(32'h3f800000, FMT_F32, FMT_F16, 0, 0, 0, 11), 16'h3c00, 0);
    chk("f32->bf16 pi", fp_convert(32'h40490fdb, FMT_F32, FMT_BF16, 0, 0, 0, 8), 16'h4049, PE);
    chk("f64->f80 1", fp_convert(64'h3ff0000000000000, FMT_F64, FMT_F80, 0, 0, 0, 64),
        80'h3fff8000000000000000, 0);
    chk("f80->f64 1/3", fp_convert(80'h3ffdaaaaaaaaaaaaaaab, FMT_F80, FMT_F64, 0, 0, 0, 53),
        64'h3fd5555555555555, PE);
    chk("f16 1+1", fp_arith(0, 16'h3c00, 16'h3c00, 0, FMT_F16, 0, 0, 0, 11, 0), 16'h4000, 0);
    chk("int->f32 2^24+1", fp_from_int(64'd16777217, 32, 1, FMT_F32, 0, 24), 32'h4b800000, PE);
    chk("int->f64 -1", fp_from_int(64'hFFFFFFFF, 32, 1, FMT_F64, 0, 53), 64'hbff0000000000000, 0);
    chk("f32->int 2.5", fp_to_int(32'h40200000, FMT_F32, 32, 1, 0, 0), 32'd2, PE);
    chk("f32->int -2.5", fp_to_int(32'hc0200000, FMT_F32, 32, 1, 0, 0), 32'hfffffffe, PE);
    chk("f32->int trunc 2.7", fp_to_int(32'h402ccccd, FMT_F32, 32, 1, 3, 0), 32'd2, PE);
    chk("f32->int 3e9", fp_to_int(32'h4f32d05e, FMT_F32, 32, 1, 0, 0), 32'h80000000, IE);
    chk("f64->i64 -1", fp_to_int(64'hbff0000000000000, FMT_F64, 64, 1, 0, 0), 64'hffffffffffffffff, 0);
    chk("round 2.5 rne", fp_round_int(32'h40200000, FMT_F32, 0, 0, 0), 32'h40000000, PE);
    chk("round -2.5 up", fp_round_int(32'hc0200000, FMT_F32, 2, 0, 0), 32'hc0000000, PE);
    // comparisons
    c = fp_cmp(32'h3f800000, 32'h40000000, FMT_F32, 5'd1, 0, x);   // LT_OS
    checks++;
    if (!c || x != 0) begin errors++; $display("FAIL cmp lt"); end
    c = fp_cmp(32'h7fc00000, 32'h40000000, FMT_F32, 5'd0, 0, x);   // EQ_OQ with a QNaN
    checks++;
    if (c || x != 0) begin errors++; $display("FAIL cmp eq qnan"); end
    c = fp_cmp(32'h7fc00000, 32'h40000000, FMT_F32, 5'd1, 0, x);   // LT_OS signals
    checks++;
    if (c || x != IE) begin errors++; $display("FAIL cmp lt qnan"); end
    if (errors != 0) $fatal(1, "tb_fpu: %0d of %0d checks failed", errors, checks);
    $display("tb_fpu: %0d checks passed", checks);
    $finish;
  end
  /* verilator lint_on WIDTHEXPAND */
endmodule
