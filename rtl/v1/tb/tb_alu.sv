// tb_alu: directed tests of v1_alu's results and x86 flags.
//
// Expected values follow the Intel SDM Vol. 2 "Flags Affected" sections
// (and Vol. 1, 3.4.3.1 for CF/OF/AF/SF/ZF/PF); undefined flags aren't
// checked. Flags are compared as {OF, SF, ZF, AF, PF, CF} masked by what the
// operation defines.
module tb_alu;
  import v1_uop_pkg::*;

  uop_op_e     op;
  logic [1:0]  sz;
  logic [63:0] a, b, c, dst_old, hi_in, flags_in;
  logic [31:0] imm;
  logic [3:0]  cc, x;
  logic [63:0] res, hi_out;
  logic        hi_we, rem_we, de, nowrite;
  logic [5:0]  fl, native;
  int          errors = 0, checks = 0;

  v1_alu dut (.op, .sz, .a, .b, .c, .dst_old, .hi_in, .imm, .cc, .x, .flags_in,
              .res, .hi_out, .hi_we, .rem_we, .fl, .native, .de, .nowrite);

  // fl bit order: CF PF AF ZF SF OF (bit 0..5)
  function automatic logic [5:0] F(input logic of_, sf, zf, af, pf, cf);
    return {of_, sf, zf, af, pf, cf};
  endfunction

  task automatic check(input string name, input uop_op_e o, input logic [1:0] s,
                       input logic [63:0] va, vb, input logic [63:0] want,
                       input logic [5:0] wantf, input logic [5:0] mask,
                       input logic [63:0] fin = 64'h2);
    op = o; sz = s; a = va; b = vb; c = 0; dst_old = 0; hi_in = 0; imm = 0; cc = 0; x = 0;
    flags_in = fin;
    #1;
    checks++;
    if (res !== want || ((fl ^ wantf) & mask & native) != 0) begin
      errors++;
      $display("FAIL %s: res %h (want %h) flags %b (want %b, mask %b, native %b)",
               name, res, want, fl, wantf, mask, native);
    end
  endtask

  initial begin
    // ADD: carry, overflow, auxiliary carry, zero, parity
    check("add8 7f+1",  U_ADD, 2'd0, 64'h7F, 64'h1, 64'h80, F(1, 1, 0, 1, 0, 0), 6'h3F);
    check("add8 ff+1",  U_ADD, 2'd0, 64'hFF, 64'h1, 64'h00, F(0, 0, 1, 1, 1, 1), 6'h3F);
    check("add32 wrap", U_ADD, 2'd2, 64'hFFFF_FFFF, 64'h2, 64'h1, F(0, 0, 0, 1, 0, 1), 6'h3F);
    check("add64 of",   U_ADD, 2'd3, 64'h7FFF_FFFF_FFFF_FFFF, 64'h1, 64'h8000_0000_0000_0000,
          F(1, 1, 0, 1, 1, 0), 6'h3F);
    check("adc8",       U_ADC, 2'd0, 64'hFE, 64'h1, 64'h00, F(0, 0, 1, 1, 1, 1), 6'h3F, 64'h3);
    // SUB/CMP: borrow and signed overflow
    check("sub8 0-1",   U_SUB, 2'd0, 64'h0, 64'h1, 64'hFF, F(0, 1, 0, 1, 1, 1), 6'h3F);
    check("sub8 80-1",  U_SUB, 2'd0, 64'h80, 64'h1, 64'h7F, F(1, 0, 0, 1, 0, 0), 6'h3F);
    check("sub64 eq",   U_SUB, 2'd3, 64'h1234, 64'h1234, 64'h0, F(0, 0, 1, 0, 1, 0), 6'h3F);
    check("sbb16",      U_SBB, 2'd1, 64'h0, 64'h0, 64'hFFFF, F(0, 1, 0, 1, 1, 1), 6'h3F, 64'h3);
    check("neg32 min",  U_NEG, 2'd2, 64'h8000_0000, 64'h0, 64'h8000_0000, F(1, 1, 0, 0, 1, 1), 6'h3F);
    // logic: OF = CF = 0
    check("and16",      U_AND, 2'd1, 64'hF0F0, 64'h0FF0, 64'h00F0, F(0, 0, 0, 0, 1, 0), 6'h3B);
    check("xor64 zero", U_XOR, 2'd3, 64'hDEAD, 64'hDEAD, 64'h0, F(0, 0, 1, 0, 1, 0), 6'h3B);
    // shifts: CF = last bit out, OF for 1-bit shifts, count masked to 5/6 bits
    check("shl8 by 1",  U_SHL, 2'd0, 64'hC0, 64'd1, 64'h80, F(0, 1, 0, 0, 0, 1), 6'h3B);
    check("shr32 by 4", U_SHR, 2'd2, 64'h8000_00F1, 64'd4, 64'h0800_000F, F(0, 0, 0, 0, 1, 0), 6'h1B);
    check("sar16 by 15", U_SAR, 2'd1, 64'h8000, 64'd15, 64'hFFFF, F(0, 1, 0, 0, 1, 0), 6'h1B);
    check("shl32 by 32 (masked to 0)", U_SHL, 2'd2, 64'h1234, 64'd32, 64'h1234, 6'h0, 6'h0);
    check("rol8 by 1",  U_ROL, 2'd0, 64'h81, 64'd1, 64'h03, F(1, 0, 0, 0, 0, 1), 6'h21);
    check("ror64 by 4", U_ROR, 2'd3, 64'hF, 64'd4, 64'hF000_0000_0000_0000, F(0, 0, 0, 0, 0, 1), 6'h01);
    check("rcl8 by 1",  U_RCL, 2'd0, 64'h80, 64'd1, 64'h01, F(1, 0, 0, 0, 0, 1), 6'h21, 64'h3);
    // multiply: CF = OF = high half significant
    check("mulu32",     U_MULU, 2'd2, 64'hFFFF_FFFF, 64'h2, 64'hFFFF_FFFE, F(1, 0, 0, 0, 0, 1), 6'h21);
    check("imult16 fits", U_IMULT, 2'd1, 64'hFFFF, 64'h7, 64'hFFF9, F(0, 0, 0, 0, 0, 0), 6'h21);
    // bit scans
    check("bsf64",      U_BSF, 2'd3, 64'h8000, 64'h0, 64'd15, F(0, 0, 0, 0, 0, 0), 6'h08);
    check("bsr32",      U_BSR, 2'd2, 64'h0001_0000, 64'h0, 64'd16, F(0, 0, 0, 0, 0, 0), 6'h08);
    check("tzcnt16 0",  U_TZCNT, 2'd1, 64'h0, 64'h0, 64'd16, F(0, 0, 0, 0, 0, 1), 6'h09);
    check("lzcnt32",    U_LZCNT, 2'd2, 64'h0000_1000, 64'h0, 64'd19, F(0, 0, 0, 0, 0, 0), 6'h09);
    check("popcnt64",   U_POPCNT, 2'd3, 64'hFF00_FF00_0000_0001, 64'h0, 64'd17, F(0, 0, 0, 0, 0, 0), 6'h3F);
    check("bswap32",    U_BSWAP, 2'd2, 64'h1122_3344, 64'h0, 64'h4433_2211, 6'h0, 6'h0);

    // division: quotient, remainder, #DE
    op = U_DIVU; sz = 2'd1; a = 64'h0000; hi_in = 64'h0001; b = 64'h0010; c = 0; flags_in = 2; #1;
    checks++;
    if (res !== 64'h1000 || hi_out !== 64'h0 || de) begin
      errors++; $display("FAIL divu16: %h r %h de %b", res, hi_out, de);
    end
    op = U_DIVU; sz = 2'd0; a = 64'h00; hi_in = 64'h10; b = 64'h10; #1;     // 1000h / 10h = 100h: too big
    checks++;
    if (!de) begin errors++; $display("FAIL divu8 overflow: no #DE"); end
    op = U_DIVS; sz = 2'd2; a = 64'hFFFF_FFF9; hi_in = 64'hFFFF_FFFF; b = 64'h2; #1;  // -7 / 2
    checks++;
    if (res !== 64'hFFFF_FFFD || hi_out !== 64'hFFFF_FFFF || de) begin
      errors++; $display("FAIL divs32 -7/2: %h r %h de %b", res, hi_out, de);
    end
    op = U_DIVU; sz = 2'd3; a = 64'h5; hi_in = 0; b = 64'h0; #1;
    checks++;
    if (!de) begin errors++; $display("FAIL div by zero: no #DE"); end

    if (errors) $fatal(1, "tb_alu: %0d of %0d checks failed", errors, checks);
    $display("tb_alu: %0d checks passed", checks);
    $finish;
  end
endmodule
