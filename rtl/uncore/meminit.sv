// meminit: the die's memory initialization engine, the stand-in for what
// AMD's Platform Security Processor does before the x86 cores leave reset
// (its ABL "memory training" firmware): read the DIMMs' SPDs over the SPD
// SMBus, check the population, hold and release the channels' RESET_n
// (tINIT1), start the subchannel controllers' JEDEC initialization
// (ddr5_sub.sv), and release the x86 core when DRAM is ready (D-014).
//
// SPD: the JESD300-5 SPD5 hub at 50h + channel*2 + DIMM, legacy I2C mode:
// MR11 (0Bh) selects the 128-byte NVM page, an address byte with bit 7 set
// reads NVM. Bytes (JESD400-5): 2 (DRAM type, 12h = DDR5), 4 (density per
// die), 6 (I/O width), 234 (ranks), 235 (bus width / ECC / subchannels).
// SMBus master: NXP UM10204 "I2C-bus specification and user manual" Rev. 7
// (2021) — START, repeated START, STOP, ACK/NACK, clock stretching; 1 MHz
// (Fast-mode Plus) SCL, divided down with the meminit_fast strap.
//
// Progress is posted as port 80h codes over the I/O link (as AMD's PSP and
// ABL post codes): D0h start, D1h SPD read, D2h RESET_n released, D3h DRAM
// ready (the core starts); errors E1h no DIMMs, E2h not DDR5 / unsupported
// organisation, E3h channels populated differently or DIMM 1 without DIMM 0
// (the slots fill from 0, as server boards require), E4h SPD bus error.
module meminit (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        fast,
  // SPD SMBus (open drain)
  output logic        scl_o,
  output logic        sda_o,
  input  logic        scl_i,
  input  logic        sda_i,
  // channels
  output logic [1:0]  ddr_reset_n,
  output logic        sub_in_reset,
  output logic        sub_start,
  output logic [3:0]  sub_pop,          // chip selects populated (DIMM*2 + rank), same on every subchannel
  output logic        sub_two_rank,
  input  logic [3:0]  sub_ready,        // per subchannel (0a, 0b, 1a, 1b)
  // results
  output logic        dram_ready,       // the x86 core may leave reset
  output logic [40:0] dram_size,        // bytes
  // POST codes (port 80h) through the fabric
  output logic        post_valid,
  output logic [7:0]  post_code,
  input  logic        post_ack
);
  import ddr5_pkg::*;

  // ------------------------------------------------------------ SMBus master
  typedef enum logic [2:0] {B_IDLE, B_START, B_RSTART, B_WRITE, B_READ, B_STOP} bop_e;
  bop_e        bop;
  logic        bgo, bbusy, back;          // operation request, busy, ACK received
  logic [7:0]  bbyte, brx;
  logic [4:0]  bstep;
  logic [3:0]  bbit;
  logic [15:0] qtimer;

  wire [15:0] quarter = fast ? 16'd2 : 16'd600;   // 2.4 GHz / 1 MHz / 4

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      scl_o <= 1'b1;
      sda_o <= 1'b1;
      bbusy <= 1'b0;
      bstep <= '0;
      bbit <= '0;
      qtimer <= '0;
      back <= 1'b0;
      brx <= '0;
    end else if (bgo && !bbusy) begin
      bbusy <= 1'b1;
      bstep <= '0;
      bbit <= '0;
      qtimer <= quarter;
    end else if (bbusy) begin
      if (qtimer != 0) qtimer <= qtimer - 16'd1;
      else if (scl_o && !scl_i) ;              // clock stretching: wait for SCL high
      else begin
        qtimer <= quarter;
        bstep <= bstep + 5'd1;
        case (bop)
          B_START: case (bstep)                // SCL, SDA high -> SDA low -> SCL low
            5'd0: begin sda_o <= 1'b1; scl_o <= 1'b1; end
            5'd1: sda_o <= 1'b0;
            5'd2: scl_o <= 1'b0;
            default: bbusy <= 1'b0;
          endcase
          B_RSTART: case (bstep)               // SCL low: SDA high, SCL high, SDA low, SCL low
            5'd0: sda_o <= 1'b1;
            5'd1: scl_o <= 1'b1;
            5'd2: sda_o <= 1'b0;
            5'd3: scl_o <= 1'b0;
            default: bbusy <= 1'b0;
          endcase
          B_STOP: case (bstep)                 // SDA low, SCL high, SDA high
            5'd0: sda_o <= 1'b0;
            5'd1: scl_o <= 1'b1;
            5'd2: sda_o <= 1'b1;
            default: bbusy <= 1'b0;
          endcase
          B_WRITE, B_READ: begin
            // 9 bits (8 data + ACK), 4 steps each: set SDA, SCL high, sample, SCL low
            case (bstep[1:0])
              2'd0: begin
                if (bbit < 4'd8) sda_o <= (bop == B_WRITE) ? bbyte[3'd7 - bbit[2:0]] : 1'b1;
                else sda_o <= (bop == B_WRITE) ? 1'b1 : 1'b1;   // master NACKs its single read
              end
              2'd1: scl_o <= 1'b1;
              2'd2: begin
                if (bbit < 4'd8) brx <= {brx[6:0], sda_i};
                else back <= !sda_i;
              end
              default: begin
                scl_o <= 1'b0;
                if (bbit == 4'd8) bbusy <= 1'b0;
                bbit <= bbit + 4'd1;
              end
            endcase
          end
          default: bbusy <= 1'b0;
        endcase
      end
    end
  end

  // ------------------------------------------------------------ sequencer
  typedef enum logic [3:0] {
    M_POST, M_SLOT, M_ISSUE, M_WAITOP, M_CHECK, M_RESET, M_WAIT, M_READY, M_FAIL
  } ms_e;
  ms_e         ms, after_post;
  logic [1:0]  slot;
  logic [2:0]  fld;                      // which SPD byte: 0..4
  logic [3:0]  op;                       // micro-op of one SPD byte read
  logic        absent;                   // this slot's hub didn't answer
  logic [7:0]  spd [4][5];
  logic [3:0]  present;
  logic [31:0] tcnt;

  function automatic logic [9:0] spd_off(input logic [2:0] f);
    case (f)
      3'd0: return 10'd2;
      3'd1: return 10'd4;
      3'd2: return 10'd6;
      3'd3: return 10'd234;
      default: return 10'd235;
    endcase
  endfunction

  // one SPD byte: S W(hub) W(0Bh) W(page) P S W(hub) W(80h|off) Sr R(hub) R P
  always_comb begin
    logic [9:0] o;
    o = spd_off(fld);
    bgo = (ms == M_ISSUE);
    bbyte = '0;
    case (op)
      4'd0: bop = B_START;
      4'd1: begin bop = B_WRITE; bbyte = {5'b10100, slot, 1'b0}; end
      4'd2: begin bop = B_WRITE; bbyte = 8'h0B; end
      4'd3: begin bop = B_WRITE; bbyte = {5'b0, o[9:7]}; end
      4'd4: bop = B_STOP;
      4'd5: bop = B_START;
      4'd6: begin bop = B_WRITE; bbyte = {5'b10100, slot, 1'b0}; end
      4'd7: begin bop = B_WRITE; bbyte = {1'b1, o[6:0]}; end
      4'd8: bop = B_RSTART;
      4'd9: begin bop = B_WRITE; bbyte = {5'b10100, slot, 1'b1}; end
      4'd10: bop = B_READ;
      default: bop = B_STOP;
    endcase
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      ms <= M_POST;
      after_post <= M_SLOT;
      post_valid <= 1'b0;
      post_code <= 8'hD0;
      slot <= '0;
      fld <= '0;
      op <= '0;
      absent <= 1'b0;
      present <= '0;
      tcnt <= '0;
      ddr_reset_n <= 2'b00;
      sub_in_reset <= 1'b1;
      sub_start <= 1'b0;
      sub_pop <= '0;
      sub_two_rank <= 1'b0;
      dram_ready <= 1'b0;
      dram_size <= '0;
      for (int i = 0; i < 4; i++)
        for (int j = 0; j < 5; j++) spd[i][j] <= '0;
    end else begin
      tcnt <= tcnt + 32'd1;               // from reset: RESET_n LOW for tINIT1
      sub_start <= 1'b0;
      case (ms)
        M_POST: begin
          post_valid <= 1'b1;
          if (post_valid && post_ack) begin
            post_valid <= 1'b0;
            ms <= after_post;
          end
        end
        M_SLOT: begin
          op <= '0;
          fld <= '0;
          absent <= 1'b0;
          ms <= M_ISSUE;
        end
        M_ISSUE: ms <= M_WAITOP;          // bgo this clock; the bus is busy from the next
        M_WAITOP: if (!bbusy) begin
          ms <= M_ISSUE;
          if (op == 4'd1 && !back) begin
            absent <= 1'b1;               // no hub: an empty slot; STOP and go on
            op <= 4'd11;
          end else if ((op == 4'd2 || op == 4'd3 || op == 4'd6 || op == 4'd7 || op == 4'd9) && !back) begin
            post_code <= 8'hE4;
            after_post <= M_FAIL;
            ms <= M_POST;
          end else if (op == 4'd10) begin
            spd[slot][fld] <= brx;
            op <= 4'd11;
          end else if (op == 4'd11) begin
            if (absent || fld == 3'd4) begin
              present[slot] <= !absent;
              if (slot == 2'd3) ms <= M_CHECK;
              else begin
                slot <= slot + 2'd1;
                ms <= M_SLOT;
              end
            end else begin
              fld <= fld + 3'd1;
              op <= '0;
            end
          end else op <= op + 4'd1;
        end
        M_CHECK: begin
          // the population: DDR5, x8, 2 x (32 + 8) bits, 1 or 2 ranks, the
          // same on every slot, both channels alike (interleaved, balanced)
          logic ok;
          logic tr;
          ok = 1'b1;
          tr = 1'b0;
          for (int i = 0; i < 4; i++)
            if (present[i]) begin
              if (spd[i][0] != 8'h12) ok = 1'b0;
              if (spd[i][2][7:5] != 3'b001) ok = 1'b0;
              if (spd[i][4][2:0] != 3'b011 || spd[i][4][4:3] != 2'b01 || spd[i][4][7:5] != 3'b001) ok = 1'b0;
              if (spd[i][3][5:3] > 3'd1) ok = 1'b0;
              tr = spd[i][3][3];
            end
          for (int i = 0; i < 4; i++)
            if (present[i] && spd[i][3][3] != tr) ok = 1'b0;
          if (present == 4'b0000) begin
            post_code <= 8'hE1;
            after_post <= M_FAIL;
          end else if (present[1:0] != present[3:2] || (present[1] && !present[0])) begin
            post_code <= 8'hE3;
            after_post <= M_FAIL;
          end else if (!ok) begin
            post_code <= 8'hE2;
            after_post <= M_FAIL;
          end else begin
            sub_two_rank <= tr;
            sub_pop <= {present[1] & tr, present[1], present[0] & tr, present[0]};
            // 16 GiB per rank per DIMM (8 GiB per subchannel), 2 channels
            dram_size <= (41'(present[0]) + 41'(present[1])) * (tr ? 41'd2 : 41'd1) * (41'd32 << 30);
            post_code <= 8'hD1;
            after_post <= M_RESET;
          end
          ms <= M_POST;
        end
        M_RESET: begin
          if (tcnt >= (fast ? (32'(T_INIT1) >> 10) : 32'(T_INIT1))) begin
            ddr_reset_n <= 2'b11;
            sub_in_reset <= 1'b0;
            sub_start <= 1'b1;
            post_code <= 8'hD2;
            after_post <= M_WAIT;
            ms <= M_POST;
          end
        end
        M_WAIT: if (sub_ready == 4'b1111) begin
          post_code <= 8'hD3;
          after_post <= M_READY;
          ms <= M_POST;
        end
        M_READY: dram_ready <= 1'b1;
        default: ;                        // M_FAIL: the core stays in reset
      endcase
    end
  end

endmodule
