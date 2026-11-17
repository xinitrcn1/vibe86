// fabric: the die's address fabric (D-014, docs/die-interface.md).
//
// Masters: the core (bus_*), the JTAG debug module (dm_*) and the init
// engine's POST codes, in round-robin order through the outbound engine;
// requests arriving over the I/O link (device DMA: MRD/MWR) through the
// inbound engine (see "the engines" below).
// Targets: the four DDR5 subchannel controllers (DRAM), the host bridge
// registers on the die (bus 0 device 0, an i440FX's identity as the PC
// platform presents: PAM 59h-5Fh and SMRAM 72h, Intel 82441FX datasheet
// 290549-001, 3.2), and the I/O link (everything else: MMIO, ports, the
// flash, other PCI configuration).
//
// Physical map: DRAM [0, TOLM) and [4 GiB, 4 GiB + size - TOLM) (AMD's data
// fabric hoists the DRAM under the PCI hole above 4 GiB the same way); in
// the legacy region A_0000h-B_FFFFh is SMRAM (DRAM) for the core in SMM or
// while the SMRAM register's D_OPEN is set (G_SMRAME on), else the link
// (VGA); C_0000h-F_FFFFh follows the PAM registers: reads go to DRAM when
// RE is set, writes when WE is set, else to the link (the flash's legacy
// alias, option ROMs). Port CF8h (configuration address) is kept here and
// passed on; CFCh-CFFh and the ECAM page of 00:00.0 are answered here.
//
// DRAM offsets interleave 64-byte lines over the subchannels: off[7:6] with
// both channels populated, off[6] (channel 0) otherwise; the bits above
// shift down by one then (sim/ddr5.h implements the same map).
module fabric
  import vibe86_platform_pkg::*;
  import apic_pkg::*;
(
  input  logic         clk,
  input  logic         rst_n,
  input  logic         dram_ready,
  input  logic [40:0]  dram_size,
  input  logic         two_ch,         // lines interleave over 4 subchannels, else channel 0's 2
  // the core
  input  logic         c_req,
  input  logic         c_we,
  input  logic         c_io,
  input  logic [63:0]  c_addr,
  input  logic [2:0]   c_size,
  input  logic [127:0] c_wdata,
  input  logic         c_smm,
  output logic         c_ack,
  output logic         c_err,
  output logic [511:0] c_rdata,        // a 64-byte read (size 6) fills all of it
  // the write snoop for the core's caches (D-018): every DRAM
  // write, by any master, names its line (writes never cross a line)
  output logic         ic_inv,
  output logic [51:6]  ic_inv_line,
  output logic         ic_inv_core,    // the core's own write (its data cache already has it)
  // the debug module (physical, 1-8 bytes)
  input  logic         d_req,
  input  logic         d_we,
  input  logic [63:0]  d_addr,
  input  logic [2:0]   d_size,
  input  logic [63:0]  d_wdata,
  output logic         d_ack,
  output logic         d_err,
  output logic [63:0]  d_rdata,
  // POST codes (meminit)
  input  logic         p_req,
  input  logic [7:0]   p_code,
  output logic         p_ack,
  // the core's local APIC (D-017): its xAPIC page at IA32_APIC_BASE, the
  // MSIs arriving over the link, the EOI messages for the I/O APIC
  input  logic [51:10] apic_base,      // IA32_APIC_BASE: base [51:12], EN [11], EXTD [10]
  output logic [11:4]  la_off,
  input  logic [31:0]  la_rdata,
  output logic         la_wr,
  output logic [31:0]  la_wdata,
  output logic         msi_valid,
  output apic_msg_t    msi_msg,
  input  logic         msi_ready,
  input  logic         eoi_valid,
  input  logic [7:0]   eoi_vec,
  output logic         eoi_ack,
  // the chipset's virtual wires over the link (D-019; HyperTransport I/O
  // Link Specification 3.10, 8.5): INTR as a level with the 8259's vector
  // (the local APICs' LINT0), NMI (LINT1) and SMI as one-clock pulses; INTA
  // goes back when a core took the ExtINT
  output logic         vw_intr,
  output logic [7:0]   vw_vec,
  output logic         vw_nmi,
  output logic         vw_smi,
  input  logic         inta,
  // a machine check the chipset reports (MCE message): one-clock pulse
  output logic         ext_mce,
  output logic [1:0]   ext_mce_type,
  output logic [63:0]  ext_mce_addr,
  // DRAM: four subchannel controllers
  output logic [3:0]   m_valid,
  input  logic [3:0]   m_ready,
  output logic         m_we,
  output logic [36:8]  m_off,
  output logic [511:0] m_wdata,
  output logic [63:0]  m_be,
  input  logic [3:0]   m_rsp,
  input  logic [511:0] m_rdata [4],
  input  logic [3:0]   m_ue,
  // the I/O link
  output logic         tx_valid,
  output logic [63:0]  tx_flit,
  input  logic         tx_ready,
  input  logic         rx_valid,
  input  logic [63:0]  rx_flit,
  output logic         rx_ready,
  // to the uncore's machine-check reporting
  output logic         mce_ue,
  output logic [63:0]  mce_addr
);

  localparam logic [7:0] OP_MRD = 8'h01, OP_MWR = 8'h02, OP_IORD = 8'h03, OP_IOWR = 8'h04,
                         OP_VW = 8'h05, OP_INTA = 8'h06, OP_MCE = 8'h07,
                         OP_CPL = 8'h08, OP_CPLE = 8'h09;
  // VW payload (the address flit): [7:0] vector, [10:8] the wire, [11] asserted
  localparam logic [2:0] VW_INTR = 3'd0, VW_NMI = 3'd1, VW_SMI = 3'd2, VW_INIT = 3'd3;

  // ------------------------------------------------------------ host bridge
  logic [7:0]  pam [7];              // 59h-5Fh
  logic [7:0]  smram;                // 72h
  logic [15:0] hb_cmd;               // 04h
  logic [31:0] cf8;                  // the configuration address register

  function automatic logic [31:0] hb_read(input logic [5:0] dw);   // dword dw
    logic [31:0] v;
    case (dw)
      6'h00: v = 32'h1237_8086;                     // 82441FX
      6'h01: v = {16'h0000, hb_cmd};
      6'h02: v = 32'h0600_0002;                     // class 06/00/00, revision 02h
      6'h16: v = {pam[2], pam[1], pam[0], 8'h00};  // 58h: 59h-5Bh
      6'h17: v = {pam[6], pam[5], pam[4], pam[3]}; // 5Ch-5Fh
      6'h1C: v = {8'h00, smram, 16'h0000};          // 70h: 72h
      default: v = 32'h0;
    endcase
    return v;
  endfunction

  // the PAM bits (RE = 1, WE = 2) of a legacy address C_0000h-F_FFFFh
  function automatic logic [1:0] pam_of(input logic [19:0] a);
    logic [3:0] k;
    if (a >= 20'hF0000) return pam[0][5:4];
    k = 4'(a[17:14]);                     // 16 KiB segments from C_0000h
    return k[0] ? pam[1 + 32'(k[3:1])][5:4] : pam[1 + 32'(k[3:1])][1:0];
  endfunction

  // ------------------------------------------------------------ the engines
  // Two engines, so that device DMA never waits behind the core: the
  // outbound engine serves the core, the debug module and the POST codes
  // (DRAM, the host bridge, or a request over the link whose completion it
  // waits for); the inbound engine serves requests arriving over the link
  // (device DMA) from DRAM and sends their completions. A single engine
  // deadlocks: a core read waiting for its completion while a posted DMA
  // write ahead of that completion on the link waits for the engine (the
  // head-of-line blocking PCIe's separate posted/completion flow-control
  // classes exist to prevent: PCI Express Base Specification 4.0, 2.4 and
  // 2.6). The DRAM port and the link's transmit channel are arbitrated
  // between them; a link packet is never interleaved with another.
  typedef enum logic [3:0] {
    O_IDLE, O_ROUTE, O_DRAM, O_DRAMWAIT, O_TXWAIT, O_TXHDR, O_TXADDR, O_TXDATA, O_RXWAIT, O_DONE
  } ost_e;
  typedef enum logic [2:0] {I_IDLE, I_DRAM, I_DRAMWAIT, I_TXWAIT, I_CPLHDR, I_CPLDATA, I_MSI} ist_e;
  ost_e ost;
  ist_e ist;

  typedef enum logic [2:0] {MS_CORE, MS_DBG, MS_POST, MS_EOI, MS_INTA} msel_e;
  msel_e who;
  logic [2:0] rr;                    // round-robin pointer over the outbound masters
  logic       inta_pend;             // INTA to send to the chipset
  // the xAPIC page: the core's accesses while the APIC is enabled, not x2APIC
  wire apic_page = who == MS_CORE && !t_io && apic_base[11] && !apic_base[10] &&
                   t_addr[51:12] == apic_base[51:12] && t_addr[63:52] == 12'h0;
  assign la_off = t_addr[11:4];
  assign la_wdata = t_data[31:0];

  // the outbound transaction
  logic        t_we, t_io, t_err;
  logic [63:0] t_addr;
  logic [2:0]  t_size;
  logic [511:0] t_data;
  logic [3:0]  t_nflits, t_flit;

  // the inbound transaction
  logic        i_we, i_err;
  logic [63:0] i_addr;
  logic [2:0]  i_size;
  logic [7:0]  i_tag;
  logic [511:0] i_data;
  logic [3:0]  i_nflits, i_flit;

  // link receive: inbound requests are assembled here (header, address,
  // data flits); completions go to the outbound engine
  logic [22:0] in_hdr;                // opcode, tag, size, data flits
  logic [1:0]  in_aux;                // header [33:32]: an MCE message's type
  logic [63:0] in_addr;
  logic [3:0]  in_n;
  logic        in_pending;            // a complete inbound request is held
  logic [511:0] in_data;
  logic        rx_cpl;                // receiving our completion's data flits

  // shared resources
  logic        dram_busy;             // the DRAM port is taken (one line request)
  logic        tx_busy;               // the transmit channel is taken (one packet)

  // DRAM decode of a physical address: {hit, offset}
  function automatic logic [41:0] dram_decode(input logic [63:0] pa, input logic smm, input logic we);
    logic hit;
    logic [40:0] off;
    hit = 1'b0;
    off = '0;
    if (dram_ready) begin
      if (pa < 64'(PLAT_MAP_TOLM) && 41'(pa) < dram_size) begin
        hit = 1'b1;
        off = 41'(pa);
        if (pa >= 64'hA0000 && pa < 64'hC0000)
          hit = smram[3] && (smm || smram[6]);
        else if (pa >= 64'hC0000 && pa < 64'h100000)
          hit = we ? pam_of(pa[19:0])[1] : pam_of(pa[19:0])[0];
      end else if (pa >= 64'(PLAT_MAP_HOIST_BASE) &&
                   pa - 64'(PLAT_MAP_HOIST_BASE) < 64'(dram_size) - 64'(PLAT_MAP_TOLM)) begin
        hit = 1'b1;
        off = 41'(pa - 64'(PLAT_MAP_HOIST_BASE) + 64'(PLAT_MAP_TOLM));
      end
    end
    return {hit, off};
  endfunction

  function automatic logic [3:0] nflits_of(input logic [2:0] size);
    return (size == 3'd6) ? 4'd8 : (size == 3'd5) ? 4'd4 : (size == 3'd4) ? 4'd2 : 4'd1;
  endfunction

  // the outbound engine's decode
  logic [41:0] dd;                    // {DRAM hit, offset}; offsets fit 37 bits (128 GiB)
  logic        cfg_local;             // CFCh-CFFh or ECAM for 00:00.0
  logic [7:0]  cfg_reg;
  always_comb begin
    dd = dram_decode(t_addr, c_smm && who == MS_CORE, t_we);
    cfg_local = 1'b0;
    cfg_reg = '0;
    if (t_io && t_addr[15:2] == 14'(16'hCFC >> 2) && cf8[31] && cf8[23:8] == 16'h0) begin
      cfg_local = 1'b1;
      cfg_reg = {cf8[7:2], t_addr[1:0]};
    end else if (!t_io && t_addr >= 64'(PLAT_MAP_ECAM_BASE) && t_addr < 64'(PLAT_MAP_ECAM_BASE) + 64'h1000) begin
      cfg_local = 1'b1;
      cfg_reg = t_addr[7:0];
    end
  end
  // the inbound engine's decode (device DMA never sees SMRAM)
  logic [41:0] di;
  assign di = dram_decode(i_addr, 1'b0, i_we);

  // the DRAM line request of an engine: subchannel, offset, byte lane
  function automatic logic [1:0] subch(input logic [7:6] d);
    return two_ch ? d[7:6] : {1'b0, d[6]};
  endfunction
  function automatic logic [36:8] lineoff(input logic [36:7] d);
    return two_ch ? d[36:8] : d[35:7];
  endfunction
  /* verilator lint_off UNUSEDSIGNAL */
  wire [7:0] dd_unused = {dd[40:37], di[40:37]};   // beyond the 4-slot maximum (128 GiB)
  /* verilator lint_on UNUSEDSIGNAL */

  // a held inbound request stops further inbound packets, never the
  // completion of the outbound engine's request
  assign rx_ready = !in_pending || rx_cpl ||
                    (ost == O_RXWAIT && in_n == 0 && (rx_flit[7:0] == OP_CPL || rx_flit[7:0] == OP_CPLE));

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      ost <= O_IDLE;
      ist <= I_IDLE;
      who <= MS_CORE;
      rr <= '0;
      c_ack <= 1'b0;
      c_err <= 1'b0;
      c_rdata <= '0;
      ic_inv <= 1'b0;
      ic_inv_line <= '0;
      ic_inv_core <= 1'b0;
      d_ack <= 1'b0;
      d_err <= 1'b0;
      d_rdata <= '0;
      p_ack <= 1'b0;
      eoi_ack <= 1'b0;
      la_wr <= 1'b0;
      msi_valid <= 1'b0;
      msi_msg <= '0;
      vw_intr <= 1'b0;
      vw_vec <= '0;
      vw_nmi <= 1'b0;
      vw_smi <= 1'b0;
      inta_pend <= 1'b0;
      in_aux <= '0;
      ext_mce <= 1'b0;
      ext_mce_type <= '0;
      ext_mce_addr <= '0;
      m_valid <= '0;
      m_we <= 1'b0;
      m_off <= '0;
      m_wdata <= '0;
      m_be <= '0;
      tx_valid <= 1'b0;
      tx_flit <= '0;
      t_we <= 1'b0;
      t_io <= 1'b0;
      t_err <= 1'b0;
      t_addr <= '0;
      t_size <= '0;
      t_data <= '0;
      t_nflits <= '0;
      t_flit <= '0;
      i_we <= 1'b0;
      i_err <= 1'b0;
      i_addr <= '0;
      i_size <= '0;
      i_tag <= '0;
      i_data <= '0;
      i_nflits <= '0;
      i_flit <= '0;
      in_hdr <= '0;
      in_addr <= '0;
      in_n <= '0;
      in_pending <= 1'b0;
      in_data <= '0;
      rx_cpl <= 1'b0;
      dram_busy <= 1'b0;
      tx_busy <= 1'b0;
      cf8 <= '0;
      hb_cmd <= 16'h0006;
      smram <= 8'h02;
      for (int i = 0; i < 7; i++) pam[i] <= '0;
      mce_ue <= 1'b0;
      mce_addr <= '0;
    end else begin
      c_ack <= 1'b0;
      ic_inv <= 1'b0;
      d_ack <= 1'b0;
      p_ack <= 1'b0;
      eoi_ack <= 1'b0;
      la_wr <= 1'b0;
      mce_ue <= 1'b0;
      vw_nmi <= 1'b0;
      vw_smi <= 1'b0;
      ext_mce <= 1'b0;
      if (inta) begin
        // the core took the ExtINT: INTR counts as dropped until the
        // chipset's next VW says what its 8259 has now
        inta_pend <= 1'b1;
        vw_intr <= 1'b0;
      end
      if (m_valid != 0 && (m_valid & m_ready) != 0) m_valid <= '0;

      // ---------------------------------------------------- link receive
      if (rx_valid && rx_ready) begin
        if (in_n == 0 && ost == O_RXWAIT && !rx_cpl &&
            (rx_flit[7:0] == OP_CPL || rx_flit[7:0] == OP_CPLE) && rx_flit[15:8] == 8'h00) begin
          // data flits follow unless the completion has none (I/O writes, errors)
          rx_cpl <= rx_flit[22:19] != 0;
          t_flit <= '0;
          t_nflits <= rx_flit[22:19];
          t_err <= rx_flit[7:0] == OP_CPLE;
          if (rx_flit[22:19] == 0) ost <= O_DONE;
        end else if (rx_cpl) begin
          t_data[64 * 32'(t_flit) +: 64] <= rx_flit;
          t_flit <= t_flit + 4'd1;
          if (t_flit + 4'd1 == t_nflits) begin
            rx_cpl <= 1'b0;
            ost <= O_DONE;
          end
        end else if (in_n == 0) begin
          in_hdr <= rx_flit[22:0];
          in_aux <= rx_flit[33:32];
          in_n <= 4'd1;
        end else if (in_n == 1) begin
          in_addr <= rx_flit;
          if (in_hdr[22:19] == 0) begin
            in_n <= 0;
            in_pending <= 1'b1;
          end else in_n <= 4'd2;
        end else begin
          in_data[64 * 32'(in_n - 4'd2) +: 64] <= rx_flit;
          if (in_n - 4'd1 == {1'b0, in_hdr[22:19]}) begin
            in_n <= 0;
            in_pending <= 1'b1;
          end else in_n <= in_n + 4'd1;
        end
      end

      // ---------------------------------------------------- the inbound engine
      case (ist)
        I_IDLE: if (in_pending && (in_hdr[7:0] == OP_VW || in_hdr[7:0] == OP_MCE)) begin
          // messages: handled at once, nothing goes back
          in_pending <= 1'b0;
          if (in_hdr[7:0] == OP_MCE) begin
            ext_mce <= 1'b1;
            ext_mce_type <= in_aux;
            ext_mce_addr <= in_addr;
          end else case (in_addr[10:8])
            VW_INTR: begin
              vw_intr <= in_addr[11];
              vw_vec <= in_addr[7:0];
            end
            VW_NMI: vw_nmi <= in_addr[11];
            VW_SMI: vw_smi <= in_addr[11];
            VW_INIT: if (in_addr[11]) begin
              // INIT to every local APIC (as an INIT IPI to all including self)
              msi_valid <= 1'b1;
              msi_msg <= '{dest: 32'd0, x2: 1'b0, dm: 1'b0, sh: 2'd2, dlv: DLV_INIT,
                           level: 1'b1, trig: 1'b0, vec: 8'd0, src: 32'hFFFF_FFFF};
              ist <= I_MSI;
            end
            default: ;
          endcase
        end else if (in_pending) begin
          i_we <= in_hdr[7:0] == OP_MWR;
          i_addr <= in_addr;
          i_size <= in_hdr[18:16];
          i_tag <= in_hdr[15:8];
          i_data <= in_data;
          i_err <= 1'b0;
          in_pending <= 1'b0;
          ist <= I_DRAM;
        end
        I_DRAM: begin
          logic [6:0] n;
          n = 7'(1) << i_size;
          if (i_we && i_addr[63:20] == 44'h0_0000_0FEE) begin
            // an MSI (Intel SDM Vol. 3A 12.11.1-2): address FEEx_xxxxh with the
            // destination in [19:12], RH [3], DM [2]; data: vector [7:0],
            // delivery mode [10:8], level [14], trigger [15]
            msi_valid <= 1'b1;
            msi_msg <= '{dest: {24'b0, i_addr[19:12]}, x2: 1'b0, dm: i_addr[2], sh: 2'd0,
                         dlv: i_data[10:8], level: i_data[14], trig: i_data[15],
                         vec: i_data[7:0], src: 32'hFFFF_FFFF};
            ist <= I_MSI;
          end else if (!di[41] || 7'(di[5:0]) + n > 7'd64) begin
            // outside DRAM or across a line: an unsupported request (a read
            // completes with an error; a posted write is dropped)
            i_err <= 1'b1;
            ist <= i_we ? I_IDLE : I_TXWAIT;
          end else if (!dram_busy && !(ost == O_DRAM)) begin
            dram_busy <= 1'b1;
            m_off <= lineoff(di[36:7]);
            m_we <= i_we;
            ic_inv <= i_we;
            ic_inv_line <= i_addr[51:6];
            ic_inv_core <= 1'b0;
            m_wdata <= i_data << (8 * di[5:0]);
            m_be <= ((64'(1) << n) - 64'd1) << di[5:0];
            m_valid <= 4'(1) << subch(di[7:6]);
            ist <= I_DRAMWAIT;
          end
        end
        I_DRAMWAIT: if (m_rsp[subch(di[7:6])]) begin
          dram_busy <= 1'b0;
          i_data <= m_rdata[subch(di[7:6])] >> (8 * di[5:0]);
          if (m_ue[subch(di[7:6])] && !i_we) i_err <= 1'b1;
          ist <= i_we ? I_IDLE : I_TXWAIT;      // a write is posted
        end
        I_TXWAIT: if (!tx_busy && ost != O_TXWAIT) begin
          tx_busy <= 1'b1;
          i_nflits <= i_err ? 4'd0 : nflits_of(i_size);
          ist <= I_CPLHDR;
        end
        I_CPLHDR: begin
          tx_valid <= 1'b1;
          tx_flit <= {32'b0, 9'b0, i_nflits, i_size, i_tag, i_err ? OP_CPLE : OP_CPL};
          if (tx_valid && tx_ready) begin
            if (i_nflits == 0) begin
              tx_valid <= 1'b0;
              tx_busy <= 1'b0;
              ist <= I_IDLE;
            end else begin
              tx_flit <= i_data[63:0];
              i_flit <= '0;
              ist <= I_CPLDATA;
            end
          end
        end
        I_CPLDATA: if (tx_ready) begin
          if (i_flit + 4'd1 == i_nflits) begin
            tx_valid <= 1'b0;
            tx_busy <= 1'b0;
            ist <= I_IDLE;
          end else begin
            tx_flit <= i_data[64 * 32'(i_flit + 4'd1) +: 64];
            i_flit <= i_flit + 4'd1;
          end
        end
        I_MSI: if (msi_ready) begin
          msi_valid <= 1'b0;
          ist <= I_IDLE;
        end
        default: ist <= I_IDLE;
      endcase

      // ---------------------------------------------------- the outbound engine
      case (ost)
        O_IDLE: begin
          logic [4:0] want;
          logic [2:0] pick;
          logic found;
          want = {inta_pend, eoi_valid && !eoi_ack, p_req && !p_ack, d_req && !d_ack,
                  c_req && !c_ack};
          found = 1'b0;
          pick = rr;
          for (int k = 0; k < 5; k++) begin
            logic [2:0] m;
            m = 3'((32'(rr) + k) % 5);
            if (!found && want[m]) begin
              found = 1'b1;
              pick = m;
            end
          end
          if (found) begin
            rr <= pick == 3'd4 ? 3'd0 : pick + 3'd1;
            who <= msel_e'(pick);
            t_err <= 1'b0;
            case (pick)
              3'd0: begin
                t_we <= c_we; t_io <= c_io; t_addr <= c_addr; t_size <= c_size;
                t_data <= {384'b0, c_wdata};
              end
              3'd1: begin
                t_we <= d_we; t_io <= 1'b0; t_addr <= d_addr; t_size <= d_size;
                t_data <= {448'b0, d_wdata};
              end
              3'd2: begin
                t_we <= 1'b1; t_io <= 1'b1; t_addr <= 64'h80; t_size <= 3'd0;
                t_data <= {504'b0, p_code};
              end
              3'd4: begin
                // INTA: the address flit carries nothing
                t_we <= 1'b0; t_io <= 1'b0; t_addr <= '0; t_size <= 3'd0; t_data <= '0;
              end
              default: begin
                // the EOI message for a level-triggered vector: the I/O APIC's
                // EOI register (offset 40h, as the ICH I/O APICs of version 20h)
                t_we <= 1'b1; t_io <= 1'b0; t_addr <= 64'(PLAT_MAP_IOAPIC_BASE) + 64'h40;
                t_size <= 3'd2; t_data <= {504'b0, eoi_vec};
              end
            endcase
            ost <= O_ROUTE;
          end
        end
        O_ROUTE: begin
          // the host bridge, DRAM, or the link
          if (who == MS_INTA) begin
            t_nflits <= 4'd0;
            ost <= O_TXWAIT;
          end else if (t_io && t_addr[15:0] == 16'hCF8 && t_size == 3'd2) begin
            if (t_we) begin
              cf8 <= t_data[31:0];
              t_nflits <= 4'd1;
              ost <= O_TXWAIT;                   // the I/O die keeps its copy too
            end else begin
              t_data <= {480'b0, cf8};
              ost <= O_DONE;
            end
          end else if (apic_page) begin
            // the local APIC's registers (32 bits at 16-byte offsets)
            if (t_we) la_wr <= 1'b1;
            else t_data <= {480'b0, la_rdata};
            ost <= O_DONE;
          end else if (cfg_local) begin
            if (t_we)
              for (int i = 0; i < 4; i++)
                if (i < (1 << t_size)) begin
                  // a byte write at register r: PAM 59h-5Fh (RE/WE bits), the
                  // command register, SMRAM 72h: C_BASE_SEG fixed 010b; D_LCK
                  // makes D_OPEN, D_LCK and G_SMRAME read-only and clears
                  // D_OPEN (82441FX datasheet 3.2.21)
                  logic [7:0] r, v;
                  r = cfg_reg + 8'(i);
                  v = t_data[8 * i +: 8];
                  if (r >= 8'h59 && r <= 8'h5F) pam[3'(r - 8'h59)] <= v & 8'h33;
                  else if (r == 8'h04) hb_cmd[7:0] <= v & 8'h06;
                  else if (r == 8'h72) begin
                    if (smram[4]) smram <= (smram & 8'hDF) | (v & 8'h20);
                    else if (v[4]) smram <= ((v & 8'h38) | 8'h02) & 8'hBF;
                    else smram <= (v & 8'h78) | 8'h02;
                  end
                end
            t_data <= {480'b0, hb_read(cfg_reg[7:2]) >> (8 * cfg_reg[1:0])};
            ost <= O_DONE;
          end else if (!t_io && dd[41]) begin
            if (7'(dd[5:0]) + (7'(1) << t_size) > 7'd64) begin
              t_err <= 1'b1;                     // a transfer must stay inside its line
              ost <= O_DONE;
            end else ost <= O_DRAM;
          end else begin
            t_nflits <= t_we ? nflits_of(t_size) : 4'd0;
            ost <= O_TXWAIT;
          end
        end
        O_DRAM: if (!dram_busy) begin
          dram_busy <= 1'b1;
          m_off <= lineoff(dd[36:7]);
          m_we <= t_we;
          ic_inv <= t_we;
          ic_inv_line <= t_addr[51:6];
          ic_inv_core <= who == MS_CORE;
          m_wdata <= t_data << (8 * dd[5:0]);
          m_be <= ((64'(1) << (7'(1) << t_size)) - 64'd1) << dd[5:0];
          m_valid <= 4'(1) << subch(dd[7:6]);
          ost <= O_DRAMWAIT;
        end
        O_DRAMWAIT: if (m_rsp[subch(dd[7:6])]) begin
          dram_busy <= 1'b0;
          t_data <= m_rdata[subch(dd[7:6])] >> (8 * dd[5:0]);
          if (m_ue[subch(dd[7:6])] && !t_we) begin
            t_err <= 1'b1;
            mce_ue <= 1'b1;
            mce_addr <= t_addr;
          end
          ost <= O_DONE;
        end
        O_TXWAIT: if (!tx_busy) begin
          tx_busy <= 1'b1;
          ost <= O_TXHDR;
        end
        O_TXHDR: begin
          tx_valid <= 1'b1;
          tx_flit <= {32'b0, 9'b0, t_nflits, t_size, 8'h00,
                      who == MS_INTA ? OP_INTA :
                      t_io ? (t_we ? OP_IOWR : OP_IORD) : (t_we ? OP_MWR : OP_MRD)};
          if (tx_valid && tx_ready) begin
            tx_flit <= t_addr;
            ost <= O_TXADDR;
          end
        end
        O_TXADDR: if (tx_ready) begin
          t_flit <= '0;
          if (t_nflits != 0) begin
            tx_flit <= t_data[63:0];
            ost <= O_TXDATA;
          end else begin
            tx_valid <= 1'b0;
            tx_busy <= 1'b0;
            ost <= who == MS_INTA ? O_DONE : O_RXWAIT;   // INTA is posted
          end
        end
        O_TXDATA: if (tx_ready) begin
          if (t_flit + 4'd1 == t_nflits) begin
            tx_valid <= 1'b0;
            tx_busy <= 1'b0;
            // memory writes are posted; port writes wait for their completion
            ost <= (t_io || !t_we) ? O_RXWAIT : O_DONE;
          end else begin
            tx_flit <= t_data[64 * 32'(t_flit + 4'd1) +: 64];
            t_flit <= t_flit + 4'd1;
          end
        end
        O_RXWAIT: ;                              // the completion arrives above
        O_DONE: begin
          case (who)
            MS_CORE: begin
              c_ack <= 1'b1;
              c_err <= t_err;
              c_rdata <= t_data;
            end
            MS_DBG: begin
              d_ack <= 1'b1;
              d_err <= t_err;
              d_rdata <= t_data[63:0];
            end
            MS_POST: p_ack <= 1'b1;
            MS_INTA: if (!inta) inta_pend <= 1'b0;
            default: eoi_ack <= 1'b1;
          endcase
          ost <= O_IDLE;
        end
        default: ost <= O_IDLE;
      endcase
    end
  end

endmodule
