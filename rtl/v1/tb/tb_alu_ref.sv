// tb_alu_ref: v1_alu against the host CPU (ref_x86.c, DPI-C) for the
// operations SeaBIOS and the differential tests found wrong or missing:
// CRC32 (CRC-32C) over random data at every width; SHLD/SHRD at 16/32/64
// bits for every count (16-bit: up to 16, beyond is undefined) with CF,
// SF, ZF, PF (and OF for count 1); ADCX/ADOX with the other flags left
// alone; MUL/IMUL high halves and CF/OF; DIV/IDIV quotients, remainders and
// #DE at the edges (zero divisors, MIN / -1, quotients one past the range).
module tb_alu_ref;
  import v1_uop_pkg::*;

  import "DPI-C" function int unsigned ref_crc32(input int unsigned crc, input longint unsigned d,
                                                 input int bytes);
  import "DPI-C" function longint unsigned ref_shxd(input int left, input int bits,
                                                    input longint unsigned dst,
                                                    input longint unsigned src, input int count,
                                                    input longint unsigned fin,
                                                    output longint unsigned fout);
  import "DPI-C" function longint unsigned ref_adx(input int ox, input int bits,
                                                   input longint unsigned a,
                                                   input longint unsigned b,
                                                   input longint unsigned fin,
                                                   output longint unsigned fout);

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

  // RFLAGS bits -> fl order {OF SF ZF AF PF CF}
  function automatic logic [5:0] fl_of(input logic [63:0] f);
    return {f[11], f[7], f[6], f[4], f[2], f[0]};
  endfunction

  function automatic logic [63:0] rnd64();
    case ($urandom_range(0, 5))
      0: return 0;
      1: return '1;
      2: return 64'h8000_0000_0000_0000 >> $urandom_range(0, 63);
      3: return {$urandom, $urandom} >> $urandom_range(0, 63);
      default: return {$urandom, $urandom};
    endcase
  endfunction

  task automatic fail(input string s);
    errors++;
    if (errors <= 40) $display("FAIL %s", s);
  endtask

  task automatic run(input uop_op_e o, input logic [1:0] s);
    op = o; sz = s; dst_old = 0; imm = 0; cc = 0;
    #1;
    checks++;
  endtask

  initial begin
    void'($urandom(7));
    x = 0; hi_in = 0; c = 0;
    // ---- CRC32
    for (int i = 0; i < 4000; i++) begin
      int sb;
      logic [63:0] m;
      sb = $urandom_range(0, 3);
      a = {32'b0, $urandom};
      b = rnd64();
      m = (sb == 3) ? '1 : (64'd1 << (8 << sb)) - 1;
      b = b & m;
      flags_in = 2;
      run(U_CRC32, 2'(sb));
      if (res[31:0] !== ref_crc32(a[31:0], b, 1 << sb))
        fail($sformatf("crc32 %0d bytes: %h, %h -> %h, host %h", 1 << sb, a[31:0], b,
                       res[31:0], ref_crc32(a[31:0], b, 1 << sb)));
    end
    // ---- SHLD / SHRD
    for (int i = 0; i < 6000; i++) begin
      int left, bits, cnt;
      logic [63:0] fo, want, m;
      logic [5:0] mask;
      left = $urandom_range(0, 1);
      bits = 16 << $urandom_range(0, 2);
      cnt = $urandom_range(0, 255);
      if (bits == 16 && (cnt & 31) > 16) cnt = cnt & 15;
      m = (bits == 64) ? '1 : (64'd1 << bits) - 1;
      a = rnd64() & m;
      b = rnd64() & m;
      c = 64'(cnt);
      flags_in = {52'b0, $urandom_range(0, 1) ? 12'h8D5 : 12'h000} | 2;
      want = ref_shxd(left, bits, a, b, cnt, flags_in, fo);
      run(left ? U_SHLD : U_SHRD, bits == 16 ? 2'd1 : bits == 32 ? 2'd2 : 2'd3);
      // the defined flags: none for a masked count of 0; CF SF ZF PF; OF for 1
      mask = 6'b011011;
      if ((cnt & (bits == 64 ? 63 : 31)) == 1) mask = 6'b111011;
      if ((cnt & (bits == 64 ? 63 : 31)) == 0) mask = 6'b111111;
      if ((cnt & (bits == 64 ? 63 : 31)) != 0 && ((fl ^ fl_of(fo)) & mask & native) != 0 ||
          ((cnt & (bits == 64 ? 63 : 31)) == 0 && native != 0 && ((fl ^ fl_of(flags_in)) & native) != 0) ||
          (res & m) !== want)
        fail($sformatf("sh%sd%0d %h, %h, %0d -> %h fl %b, host %h fl %b", left ? "l" : "r", bits,
                       a, b, cnt, res, fl, want, fl_of(fo)));
    end
    c = 0;
    // ---- ADCX / ADOX: x = 1 selects OF as the carry (ADC micro-op)
    for (int i = 0; i < 4000; i++) begin
      int ox, bits;
      logic [63:0] fo, want, m;
      ox = $urandom_range(0, 1);
      bits = $urandom_range(0, 1) ? 64 : 32;
      m = (bits == 64) ? '1 : 64'hFFFF_FFFF;
      a = rnd64() & m;
      b = rnd64() & m;
      flags_in = {52'b0, 12'($urandom) & 12'h8D5} | 2;
      want = ref_adx(ox, bits, a, b, flags_in, fo);
      x = 4'(ox);
      run(U_ADC, bits == 64 ? 2'd3 : 2'd2);
      x = 0;
      // ADCX writes CF only, ADOX OF only: compare that flag and that
      // nothing else is claimed (native)
      if ((res & m) !== want ||
          (ox ? (fl[5] !== fo[11] || native != 6'h20) : (fl[0] !== fo[0])))
        fail($sformatf("ad%sx%0d %h + %h (flags %h) -> %h fl %b native %b, host %h flags %h",
                       ox ? "o" : "c", bits, a, b, flags_in, res, fl, native, want, fo));
    end
    // ---- MUL / IMUL: high halves, CF = OF
    for (int i = 0; i < 4000; i++) begin
      int s;
      logic [63:0] m;
      logic [127:0] p;
      bit sgn;
      s = $urandom_range(0, 3);
      sgn = $urandom_range(0, 1);
      m = (s == 3) ? '1 : (64'd1 << (8 << s)) - 1;
      a = rnd64() & m;
      b = rnd64() & m;
      flags_in = 2;
      run(sgn ? U_MULS : U_MULU, 2'(s));
      if (!sgn) p = {64'b0, a} * {64'b0, b};
      else p = 128'($signed({{64{a[(8 << s) - 1]}}, a | (a[(8 << s) - 1] ? ~m : 0)}) *
                    $signed({{64{b[(8 << s) - 1]}}, b | (b[(8 << s) - 1] ? ~m : 0)}));
      if (res !== (p[63:0] & m) || hi_out !== ((p >> (8 << s)) & {64'b0, m}))
        fail($sformatf("mul%s%0d %h * %h -> %h:%h", sgn ? "s" : "u", 8 << s, a, b, hi_out, res));
    end
    // ---- DIV / IDIV at the edges
    for (int s = 0; s < 4; s++) begin
      logic [63:0] m, mn;
      m = (s == 3) ? '1 : (64'd1 << (8 << s)) - 1;
      mn = 64'd1 << ((8 << s) - 1);
      // MIN / -1 (sign-extended dividend): #DE
      a = mn; hi_in = m; b = m; flags_in = 2;
      run(U_DIVS, 2'(s));
      if (!de) fail($sformatf("idiv%0d MIN / -1: no #DE", 8 << s));
      // MIN / 1: fits
      a = mn; hi_in = m; b = 1;
      run(U_DIVS, 2'(s));
      if (de || res !== mn) fail($sformatf("idiv%0d MIN / 1: %h de %b", 8 << s, res, de));
      // a positive quotient of MAX + 1: #DE
      a = 0; hi_in = 1; b = 2;                      // 2^n / 2 = 2^(n-1) = MAX + 1
      run(U_DIVS, 2'(s));
      if (!de) fail($sformatf("idiv%0d 2^n / 2: no #DE", 8 << s));
      // unsigned: hi = divisor -> #DE; hi = divisor - 1 -> fits
      a = m; hi_in = 5; b = 5;
      run(U_DIVU, 2'(s));
      if (!de) fail($sformatf("div%0d hi = divisor: no #DE", 8 << s));
      a = m; hi_in = 4; b = 5;
      run(U_DIVU, 2'(s));
      if (de) fail($sformatf("div%0d hi < divisor: #DE", 8 << s));
      a = 7; hi_in = 0; b = 0;
      run(U_DIVU, 2'(s));
      if (!de) fail($sformatf("div%0d by 0: no #DE", 8 << s));
      run(U_DIVS, 2'(s));
      if (!de) fail($sformatf("idiv%0d by 0: no #DE", 8 << s));
      // -7 / 2 = -3 r -1; 7 / -2 = -3 r 1
      a = (-64'd7) & m; hi_in = m; b = 2;
      run(U_DIVS, 2'(s));
      if (de || res !== ((-64'd3) & m) || hi_out !== m)
        fail($sformatf("idiv%0d -7 / 2: %h r %h", 8 << s, res, hi_out));
      a = 7; hi_in = 0; b = (-64'd2) & m;
      run(U_DIVS, 2'(s));
      if (de || res !== ((-64'd3) & m) || hi_out !== 1)
        fail($sformatf("idiv%0d 7 / -2: %h r %h", 8 << s, res, hi_out));
    end
    if (errors) $fatal(1, "tb_alu_ref: %0d of %0d checks failed", errors, checks);
    $display("tb_alu_ref: %0d checks agree with the host / the edges", checks);
    $finish;
  end
endmodule
