// smu: the system management unit's power sequencer and VR control (D-019,
// docs/package.md "Clock, reset and power sequencing" and "Voltage
// regulator control"). It sits on the standby rail: PWROK low resets it.
//
// Sequence: PWROK high (the VRs are up at their boot voltage) -> set each
// controlled rail's operating voltage over the SVID-like bus and wait for
// it to settle -> wait for RESET_L high -> run (the rest of the die leaves
// reset). RESET_L low goes back to waiting for it (a warm reset keeps the
// rails); a VR that NAKs three times, answers with bad parity three times
// or doesn't settle in time leaves the die off, `fatal` set, until PWROK
// falls.
//
// The bus (our own protocol, specified in docs/package.md): SVC is driven
// here, one bit per two clocks (SVD changes while SVC is low, the receiver
// samples on SVC rising); SVD is open drain (svd_o 0 pulls it low, svd_i
// the line); SVT is the VRs' wired-OR alert (low: a VR settled or wants
// attention). Frame: start (SVD falls, SVC high), address[3:0], command[4:0],
// payload[7:0], even parity over those 17 bits, a turnaround bit, the VR's
// 2-bit acknowledge (01 ACK, 10 ACK and busy: SVT will go low when the
// rail has settled, 11 NAK), for a read 8 data bits and their even parity
// from the VR, then stop (SVD rises, SVC high).
module smu #(
  parameter logic [7:0] VID_CPU = 8'd150,   // VDDCR_CPU 1.00 V = 0.25 + 0.005 * VID
  parameter logic [7:0] VID_SOC = 8'd130,   // VDDCR_SOC 0.90 V
  parameter int SETTLE_MAX = 1 << 18        // clocks a rail may take to settle
) (
  input  logic       clk,
  input  logic       pwrok,          // the board's power-good (resets the SMU when low)
  input  logic       rst_n,          // RESET_L
  output logic       svc,
  output logic       svd_o,
  input  logic       svd_i,
  input  logic       svt,
  output logic       die_rst_n,      // everything but the SMU: out of reset
  output logic [2:0] pstate,         // the sequencer's step (the sideband reads it)
  output logic       fatal,          // a VR failed: ERROR_L[2]
  output logic [7:0] vr_status [2]   // each VR's last status register
);
  localparam logic [4:0] CMD_SETVID_SLOW = 5'h02, CMD_GETREG = 5'h10;
  localparam logic [2:0] P_OFF = 3'd0, P_RAILS = 3'd1, P_RESET = 3'd2, P_RUN = 3'd4,
                         P_FAIL = 3'd7;

  // ------------------------------------------------------------ the bus engine
  typedef enum logic [2:0] {B_IDLE, B_START, B_LOW, B_HIGH, B_STOP0, B_STOP1, B_DONE} bst_e;
  bst_e        bst;
  logic        go, rd;
  logic [17:0] frame;
  logic [4:0]  n;                  // the bit slot: 0-17 ours, 18 turnaround, 19-20 ack, 21-29 read data
  logic [10:0] rx;
  logic        done;               // a frame finished (one clock)

  wire [4:0] last = rd ? 5'd29 : 5'd20;

  always_ff @(posedge clk or negedge pwrok) begin
    if (!pwrok) begin
      bst <= B_IDLE;
      svc <= 1'b1;
      svd_o <= 1'b1;
      n <= '0;
      rx <= '0;
      done <= 1'b0;
    end else begin
      done <= 1'b0;
      case (bst)
        B_IDLE: begin
          svc <= 1'b1;
          svd_o <= 1'b1;
          if (go) begin
            svd_o <= 1'b0;             // start: SVD falls while SVC is high
            bst <= B_START;
          end
        end
        B_START: begin
          n <= '0;
          svc <= 1'b0;
          svd_o <= frame[17];
          bst <= B_HIGH;
        end
        B_LOW: begin
          // the outputs are registered: while this state runs, the pins
          // still show the previous slot with SVC high, and SVD carries
          // what the VR drove for it -- sample its slots (19 on) here
          if (n >= 5'd20) rx <= {rx[9:0], svd_i};
          svc <= 1'b0;
          svd_o <= n <= 5'd17 ? frame[5'd17 - n] : 1'b1;   // released from the turnaround on
          bst <= B_HIGH;
        end
        B_HIGH: begin
          svc <= 1'b1;
          if (n == last) bst <= B_STOP0;
          else begin
            n <= n + 5'd1;
            bst <= B_LOW;
          end
        end
        B_STOP0: begin
          rx <= {rx[9:0], svd_i};      // the last slot (as in B_LOW)
          svc <= 1'b0;
          svd_o <= 1'b0;
          bst <= B_STOP1;
        end
        B_STOP1: begin
          svc <= 1'b1;
          bst <= B_DONE;
        end
        default: begin                 // B_DONE: SVD rises while SVC is high
          svd_o <= 1'b1;
          done <= 1'b1;
          bst <= B_IDLE;
        end
      endcase
    end
  end

  // the received acknowledge, data and parity (the last bits shifted in)
  wire [1:0] ack = rd ? rx[10:9] : rx[1:0];
  wire [7:0] rdata = rx[8:1];
  wire       rpar_ok = ^rx[8:0] == 1'b0;

  // ------------------------------------------------------------ the sequencer
  typedef enum logic [2:0] {Q_SET, Q_SETWAIT, Q_SETTLE, Q_STATUS, Q_STATWAIT, Q_NEXT} qst_e;
  qst_e        q;
  logic        rail;               // 0 VDDCR_CPU, 1 VDDCR_SOC
  logic [1:0]  tries;
  logic [18:0] timer;
  logic        run_q;

  function automatic logic [17:0] mkframe(input logic [3:0] a, input logic [4:0] c,
                                          input logic [7:0] p);
    return {a, c, p, ^{a, c, p}};
  endfunction

  assign die_rst_n = run_q && rst_n;

  always_ff @(posedge clk or negedge pwrok) begin
    if (!pwrok) begin
      pstate <= P_OFF;
      q <= Q_SET;
      rail <= 1'b0;
      tries <= '0;
      timer <= '0;
      run_q <= 1'b0;
      fatal <= 1'b0;
      go <= 1'b0;
      rd <= 1'b0;
      frame <= '0;
      vr_status[0] <= '0;
      vr_status[1] <= '0;
    end else begin
      go <= 1'b0;
      case (pstate)
        P_OFF: pstate <= P_RAILS;      // PWROK is high: the rails are at their boot voltage
        P_RAILS: case (q)
          Q_SET: begin
            frame <= mkframe({3'b0, rail}, CMD_SETVID_SLOW, rail ? VID_SOC : VID_CPU);
            rd <= 1'b0;
            go <= 1'b1;
            q <= Q_SETWAIT;
          end
          Q_SETWAIT: if (done) begin
            timer <= '0;
            if (ack == 2'b01) q <= Q_NEXT;            // already there
            else if (ack == 2'b10) q <= Q_SETTLE;     // busy: SVT says when
            else if (tries == 2'd2) pstate <= P_FAIL;
            else begin
              tries <= tries + 2'd1;
              q <= Q_SET;
            end
          end
          Q_SETTLE: begin
            timer <= timer + 19'd1;
            if (!svt) q <= Q_STATUS;
            else if (32'(timer) >= SETTLE_MAX) pstate <= P_FAIL;
          end
          Q_STATUS: begin
            // read the status register: it says the rail is settled and
            // clears the VR's alert
            frame <= mkframe({3'b0, rail}, CMD_GETREG, 8'h00);
            rd <= 1'b1;
            go <= 1'b1;
            q <= Q_STATWAIT;
          end
          Q_STATWAIT: if (done) begin
            if (ack == 2'b01 && rpar_ok) begin
              vr_status[rail] <= rdata;
              q <= Q_NEXT;
            end else if (tries == 2'd2) pstate <= P_FAIL;
            else begin
              tries <= tries + 2'd1;
              q <= Q_STATUS;
            end
          end
          default: begin              // Q_NEXT
            tries <= '0;
            if (rail) pstate <= P_RESET;
            else begin
              rail <= 1'b1;
              q <= Q_SET;
            end
          end
        endcase
        P_RESET: if (rst_n) begin
          run_q <= 1'b1;
          pstate <= P_RUN;
        end
        P_RUN: if (!rst_n) begin
          run_q <= 1'b0;
          pstate <= P_RESET;
        end
        default: begin                // P_FAIL: off until PWROK falls
          run_q <= 1'b0;
          fatal <= 1'b1;
        end
      endcase
    end
  end
endmodule
