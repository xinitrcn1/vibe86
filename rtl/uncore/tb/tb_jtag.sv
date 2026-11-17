// tb_jtag: jtag_tap + jtag_dm against stub core and fabric models.
//   the TAP state machine: TRST_n, a walk through all 16 states, the
//   five-TMS-high reset from any state; IDCODE selected by Test-Logic-Reset;
//   the IR capture pattern 00001b; BYPASS (and an unassigned code) as a
//   one-bit register; debug halt / register write and read / step / resume;
//   register access while running (reg_err); system bus write and read,
//   a bus error and clearing it; a request while one is pending (dropped);
//   all of it at TCK periods below, near and above the core clock's
//   (34, 14 and 6 ns against 10 ns), to exercise the clock-domain crossing.
module tb_jtag;
  logic clk = 1'b0;
  always #5 clk = ~clk;

  logic tck = 1'b0, tms = 1'b1, tdi = 1'b0, trst_n = 1'b0;
  wire  tdo;
  wire [3:0] tap_state;
  int unsigned half = 17;     // TCK half period (ns)
  int unsigned half_list [3] = '{17, 7, 3};

  // ------------------------------------------------------------ DUT
  wire        req_tog, req_we, ack_tog;
  wire [2:0]  req_op, req_size;
  wire [3:0]  req_ctl;
  wire [7:0]  req_sel;
  wire [63:0] req_data, req_addr, reg_rdata, sba_rdata;
  wire        st_halted, st_haltreq, st_sba_err, st_reg_err;
  wire        dbg_req, dbg_step, dbg_reg_we;
  logic       dbg_halted = 1'b0;
  wire [7:0]  dbg_reg_sel;
  wire [63:0] dbg_reg_wdata;
  logic [63:0] dbg_reg_rdata;
  wire        dm_req, dm_we;
  wire [63:0] dm_addr, dm_wdata;
  wire [2:0]  dm_size;
  logic       dm_ack = 1'b0, dm_err = 1'b0;
  logic [63:0] dm_rdata = '0;

  jtag_tap tap (.tck, .tms, .tdi, .trst_n, .tdo, .req_tog, .req_op, .req_ctl, .req_we,
                .req_sel, .req_data, .req_addr, .req_size, .ack_tog, .reg_rdata, .sba_rdata,
                .st_halted, .st_haltreq, .st_sba_err, .st_reg_err, .tap_state);
  jtag_dm dm (.clk, .trst_n, .req_tog, .req_op, .req_ctl, .req_we, .req_sel, .req_data,
              .req_addr, .req_size, .ack_tog, .reg_rdata, .sba_rdata, .st_halted, .st_haltreq,
              .st_sba_err, .st_reg_err, .dbg_req, .dbg_step, .dbg_halted, .dbg_reg_sel,
              .dbg_reg_we, .dbg_reg_wdata, .dbg_reg_rdata, .dm_req, .dm_we, .dm_addr,
              .dm_size, .dm_wdata, .dm_ack, .dm_err, .dm_rdata);

  // ------------------------------------------------------------ stub core
  // halts 3 cycles after dbg_req, resumes 2 cycles after it drops; a step
  // runs for 4 cycles and advances "RIP" (register 16)
  logic [63:0] regs [256];
  int unsigned hcnt = 0, scnt = 0;
  assign dbg_reg_rdata = regs[dbg_reg_sel];
  initial begin
    for (int i = 0; i < 256; i++) regs[i] = 64'hA5A5_0000_0000_0000 | 64'(i);
    regs[16] = 64'h100;
  end
  always_ff @(posedge clk) begin
    if (dbg_reg_we) begin
      if (!dbg_halted) $fatal(1, "register write while running");
      regs[dbg_reg_sel] <= dbg_reg_wdata;
    end
    if (scnt != 0) begin
      scnt <= scnt - 1;
      if (scnt == 1) begin
        dbg_halted <= 1'b1;
        regs[16] <= regs[16] + 1;
      end
    end else if (dbg_step && dbg_halted) begin
      dbg_halted <= 1'b0;
      scnt <= 4;
    end else if (dbg_req && !dbg_halted) begin
      hcnt <= hcnt + 1;
      if (hcnt == 2) dbg_halted <= 1'b1;
    end else if (!dbg_req && dbg_halted) begin
      hcnt <= hcnt + 1;
      if (hcnt == 1) dbg_halted <= 1'b0;
    end else hcnt <= 0;
  end

  // ------------------------------------------------------------ stub fabric
  // 512 bytes at 1000h; anything else is a bus error; latency 1 + addr[6:3]
  // cycles (addr 1FF8h: 2000 cycles, for the 'dropped' test)
  logic [7:0] mem [512];
  int unsigned lat = 0;
  always_ff @(posedge clk) begin
    dm_ack <= 1'b0;
    if (dm_req && !dm_ack) begin
      if (lat == 0) lat <= (dm_addr == 64'h1FF8) ? 2000 : 1 + 32'(dm_addr[6:3]);
      else if (lat == 1) begin
        lat <= 0;
        dm_ack <= 1'b1;
        dm_err <= !(dm_addr >= 64'h1000 && dm_addr < 64'h1200) && dm_addr != 64'h1FF8;
        if (dm_addr >= 64'h1000 && dm_addr < 64'h1200) begin
          for (int i = 0; i < 8; i++)
            if (i < (1 << dm_size)) begin
              if (dm_we) mem[9'(dm_addr - 64'h1000) + 9'(i)] <= dm_wdata[8 * i +: 8];
              else dm_rdata[8 * i +: 8] <= mem[9'(dm_addr - 64'h1000) + 9'(i)];
            end
        end
      end else lat <= lat - 1;
    end
  end

  // ------------------------------------------------------------ JTAG host
  int errors = 0;
  task automatic check(input string what, input logic [127:0] got, input logic [127:0] want);
    if (got !== want) begin
      $display("FAIL %s: got %h want %h (TCK half %0d)", what, got, want, half);
      errors++;
    end
  endtask

  // one TCK cycle: set TMS/TDI, sample TDO before the rising edge
  task automatic clock(input logic m, input logic d, output logic o);
    tms = m;
    tdi = d;
    #(half);
    o = tdo;
    tck = 1'b1;
    #(half);
    tck = 1'b0;
  endtask
  task automatic tmsseq(input logic [31:0] bits, input int n);
    logic o;
    for (int i = 0; i < n; i++) clock(bits[i], 1'b0, o);
  endtask
  task automatic reset5;                // Test-Logic-Reset, then Run-Test/Idle
    tmsseq(32'b011111, 6);
  endtask
  // from Run-Test/Idle: shift n bits (LSB first) through IR or DR, back to Run-Test/Idle
  task automatic shift(input logic ir, input logic [127:0] din, input int n, output logic [127:0] dout);
    logic o;
    dout = '0;
    tmsseq(ir ? 32'b0011 : 32'b001, ir ? 4 : 3);   // Select-DR(-IR), Capture, Shift
    for (int i = 0; i < n; i++) begin
      clock(i == n - 1, din[i], o);
      dout[i] = o;
    end
    tmsseq(32'b01, 2);                                // Update, Run-Test/Idle
  endtask
  task automatic ir(input logic [4:0] v);
    logic [127:0] o;
    shift(1'b1, 128'(v), 5, o);
  endtask
  function automatic logic [31:0] ctl_bits(input logic [127:0] v);
    return v[31:0];
  endfunction
  // DBGCTL status, after the pending request (if any) completed
  task automatic ctl_wait(output logic [31:0] s);
    logic [127:0] o;
    int n = 0;
    ir(5'b10000);
    do begin
      shift(1'b0, 128'b0, 32, o);
      n++;
      if (n > 1000) $fatal(1, "DBGCTL busy forever");
    end while (o[2]);
    s = o[31:0];
  endtask
  task automatic ctl(input logic [3:0] c);
    logic [127:0] o;
    ir(5'b10000);
    shift(1'b0, 128'(c), 32, o);
  endtask

  // ------------------------------------------------------------ tests
  task automatic tap_tests;
    logic [127:0] o;
    // TRST_n: Test-Logic-Reset
    trst_n = 1'b0;
    #(3 * half);
    check("TRST -> TLR", 128'(tap_state), 128'h0);
    trst_n = 1'b1;
    // a walk through all 16 states (1149.1 Figure 6-1)
    begin
      // TMS from Test-Logic-Reset, and the state after each clock
      logic t [22] = '{1'b0, 1'b1, 1'b0, 1'b0, 1'b1, 1'b0, 1'b1, 1'b0, 1'b1, 1'b1, 1'b1,
                       1'b1, 1'b0, 1'b0, 1'b1, 1'b0, 1'b1, 1'b0, 1'b1, 1'b1, 1'b0, 1'b1};
      logic [3:0] want [22] = '{4'h1, 4'h2, 4'h3, 4'h4, 4'h5, 4'h6, 4'h7, 4'h4, 4'h5, 4'h8, 4'h2,
                                4'h9, 4'hA, 4'hB, 4'hC, 4'hD, 4'hE, 4'hB, 4'hC, 4'hF, 4'h1, 4'h2};
      for (int i = 0; i < 22; i++) begin
        logic d;
        clock(t[i], 1'b0, d);
        check($sformatf("state walk step %0d", i), 128'(tap_state), 128'(want[i]));
      end
      tmsseq(32'b11, 2);                              // Select-IR-Scan, Test-Logic-Reset
      check("SEL_IR -> TLR", 128'(tap_state), 128'h0);
    end
    // TMS high for five clocks resets from anywhere (here Pause-DR) and selects IDCODE
    reset5;
    ir(5'b11111);
    tmsseq(32'b0101, 4);                              // Select-DR, Capture, Exit1, Pause-DR
    check("in Pause-DR", 128'(tap_state), 128'h6);
    tmsseq(32'b11111, 5);
    check("five TMS high -> TLR", 128'(tap_state), 128'h0);
    tmsseq(32'b0, 1);
    shift(1'b0, 128'b0, 32, o);
    check("IDCODE after reset", 128'(o[31:0]), 128'(32'h10B86001));
    // the IR capture pattern (00001b; the low bits 01b, clause 7.1.1)
    shift(1'b1, 128'b11111, 5, o);
    check("IR capture", 128'(o[4:0]), 128'(5'b00001));
    // BYPASS: one bit, captures 0
    shift(1'b0, 128'hB3, 9, o);
    check("BYPASS delays by one", 128'(o[8:0]), 128'({8'hB3, 1'b0}));
    // an unassigned instruction is BYPASS
    ir(5'b00111);
    shift(1'b0, 128'h5A, 9, o);
    check("unassigned = BYPASS", 128'(o[8:0]), 128'({8'h5A, 1'b0}));
    reset5;
  endtask

  task automatic debug_tests;
    logic [31:0] s;
    logic [127:0] o;
    // reset the debug state (TRST_n), then halt
    trst_n = 1'b0;
    #(4 * half + 30);
    trst_n = 1'b1;
    regs[16] = 64'h100;
    reset5;
    ctl_wait(s);
    check("running", 128'(s[1:0]), 128'b00);
    check("DBGCTL version", 128'(s[31:28]), 128'd1);
    // register access while running: reg_err
    ir(5'b10001);
    shift(1'b0, {55'b0, 1'b0, 8'd5, 64'b0}, 73, o);
    ctl_wait(s);
    check("register access while running -> reg_err", 128'(s[4]), 128'd1);
    ctl(4'b1000);                                     // clear errors
    ctl_wait(s);
    check("errors cleared", 128'(s[5:3]), 128'd0);
    // halt
    ctl(4'b0001);
    do ctl_wait(s); while (!s[0]);
    check("halted and haltreq", 128'(s[1:0]), 128'b11);
    // write register 5, read it back; read RIP (16)
    ir(5'b10001);
    shift(1'b0, {55'b0, 1'b1, 8'd5, 64'hDEAD_BEEF_0123_4567}, 73, o);
    ctl_wait(s);
    check("core register 5 written", 128'(regs[5]), 128'(64'hDEAD_BEEF_0123_4567));
    ir(5'b10001);
    shift(1'b0, {55'b0, 1'b0, 8'd5, 64'b0}, 73, o);
    ctl_wait(s);
    ir(5'b10001);
    shift(1'b0, {55'b0, 1'b0, 8'd16, 64'b0}, 73, o);  // captures reg 5's value, asks for 16
    check("DBGREG read reg 5", 128'(o[63:0]), 128'(64'hDEAD_BEEF_0123_4567));
    check("DBGREG capture sel", 128'(o[71:64]), 128'd5);
    ctl_wait(s);
    ir(5'b10001);
    shift(1'b0, {55'b0, 1'b0, 8'd16, 64'b0}, 73, o);
    check("DBGREG read RIP", 128'(o[63:0]), 128'(64'h100));
    // single step: RIP advances, halted again
    ctl(4'b0100);
    ctl_wait(s);
    do ctl_wait(s); while (!s[0]);
    ir(5'b10001);
    shift(1'b0, {55'b0, 1'b0, 8'd16, 64'b0}, 73, o);
    ctl_wait(s);
    ir(5'b10001);
    shift(1'b0, {55'b0, 1'b0, 8'd16, 64'b0}, 73, o);
    check("step advanced RIP", 128'(o[63:0]), 128'(64'h101));
    // system bus: write 8 bytes at 1008h, read them back, read 2 bytes
    ir(5'b10010);
    shift(1'b0, {61'b0, 3'd3, 64'h1008}, 67, o);
    ir(5'b10011);
    shift(1'b0, {62'b0, 2'b10, 64'h1122_3344_5566_7788}, 66, o);
    ctl_wait(s);
    check("SBA write ok", 128'(s[3]), 128'd0);
    check("memory written", 128'({mem[15], mem[14], mem[13], mem[12], mem[11], mem[10], mem[9], mem[8]}), 128'(64'h1122_3344_5566_7788));
    ir(5'b10011);
    shift(1'b0, {62'b0, 2'b01, 64'b0}, 66, o);
    ctl_wait(s);
    ir(5'b10011);
    shift(1'b0, 128'b0, 66, o);
    check("SBA read", 128'(o[63:0]), 128'(64'h1122_3344_5566_7788));
    check("SBA status idle", 128'(o[65:64]), 128'd0);
    ir(5'b10010);
    shift(1'b0, {61'b0, 3'd1, 64'h100A}, 67, o);
    ir(5'b10011);
    shift(1'b0, {62'b0, 2'b01, 64'b0}, 66, o);
    ctl_wait(s);
    ir(5'b10011);
    shift(1'b0, 128'b0, 66, o);
    check("SBA read 2 bytes (little-endian, 100Ah)", 128'(o[15:0]), 128'h5566);
    // a bus error is sticky until cleared
    ir(5'b10010);
    shift(1'b0, {61'b0, 3'd3, 64'h9000}, 67, o);
    ir(5'b10011);
    shift(1'b0, {62'b0, 2'b01, 64'b0}, 66, o);
    ctl_wait(s);
    check("SBA error", 128'(s[3]), 128'd1);
    ctl(4'b1000);
    ctl_wait(s);
    check("SBA error cleared", 128'(s[3]), 128'd0);
    // a request while one is pending is dropped (the fabric takes 2000
    // cycles at 1FF8h; the second request follows within a few TCKs)
    ir(5'b10010);
    shift(1'b0, {61'b0, 3'd3, 64'h1FF8}, 67, o);
    ir(5'b10011);
    shift(1'b0, {62'b0, 2'b01, 64'b0}, 66, o);
    shift(1'b0, {62'b0, 2'b01, 64'b0}, 66, o);
    ctl_wait(s);
    check("dropped while busy", 128'(s[5]), 128'd1);
    ctl(4'b1000);
    ctl_wait(s);
    check("dropped cleared", 128'(s[5]), 128'd0);
    // resume
    ctl(4'b0010);
    do ctl_wait(s); while (s[0]);
    check("resumed", 128'(s[1:0]), 128'b00);
  endtask

  initial begin
    foreach (half_list[k]) begin
      half = half_list[k];
      tap_tests();
      debug_tests();
    end
    if (errors == 0) $display("tb_jtag: all checks passed");
    else $fatal(1, "tb_jtag: %0d checks failed", errors);
    $finish;
  end
endmodule
