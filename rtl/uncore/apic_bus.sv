// apic_bus: the die's interrupt-message bus between its N local APICs and
// the MSIs arriving over the I/O link (D-017). One message per two clocks:
// a sender is picked round-robin and the message broadcast; every local
// APIC says whether it is addressed (lapic.sv: physical, logical, the
// shorthands), and the bus delivers it to all of them, or for the lowest-
// priority delivery mode to the one with the lowest arbitration priority
// (APR; ties to the lowest index) -- Intel SDM Vol. 3A 12.6.2.4 leaves the
// arbitration among equals to the implementation. A message nobody accepts
// is dropped (no Send Accept Error: that ESR bit exists only on P6 and
// Pentium APICs).
module apic_bus
  import apic_pkg::*;
#(
  parameter int N = 1
) (
  input  logic      clk,
  input  logic      rst_n,
  // senders: the local APICs' ICRs, and the MSI port
  input  logic      src_valid [N],
  input  apic_msg_t src_msg [N],
  output logic      src_ready [N],
  input  logic      msi_valid,
  input  apic_msg_t msi_msg,
  output logic      msi_ready,
  // the broadcast
  output logic      bus_valid,
  output apic_msg_t bus_msg,
  input  logic      match [N],
  input  logic [7:0] apr [N],
  output logic      accept [N]
);
  localparam int W = $clog2(N + 1) + 1;
  logic [W-1:0] rr;

  // delivery: everyone addressed, or the lowest-priority one
  always_comb begin
    int pick;
    logic [7:0] best;
    pick = -1;
    best = 8'hFF;
    for (int i = 0; i < N; i++)
      if (match[i] && (pick < 0 || apr[i] < best)) begin
        pick = i;
        best = apr[i];
      end
    for (int i = 0; i < N; i++)
      accept[i] = bus_valid && match[i] && (bus_msg.dlv != DLV_LOWEST || i == pick);
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rr <= '0;
      bus_valid <= 1'b0;
      bus_msg <= '0;
      msi_ready <= 1'b0;
      for (int i = 0; i < N; i++) src_ready[i] <= 1'b0;
    end else begin
      msi_ready <= 1'b0;
      for (int i = 0; i < N; i++) src_ready[i] <= 1'b0;
      if (bus_valid) bus_valid <= 1'b0;            // one clock on the bus
      else begin
        // round-robin over the N senders and the MSI port (index N)
        logic found;
        found = 1'b0;
        for (int k = 0; k <= N; k++) begin
          int s;
          s = (int'(rr) + k) % (N + 1);
          if (!found && ((s == N) ? (msi_valid && !msi_ready) : (src_valid[s] && !src_ready[s]))) begin
            found = 1'b1;
            bus_valid <= 1'b1;
            if (s == N) begin
              bus_msg <= msi_msg;
              msi_ready <= 1'b1;
            end else begin
              bus_msg <= src_msg[s];
              src_ready[s] <= 1'b1;
            end
            rr <= W'((s + 1) % (N + 1));
          end
        end
      end
    end
  end
endmodule
