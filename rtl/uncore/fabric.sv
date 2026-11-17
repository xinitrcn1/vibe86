// fabric: the die's address fabric (D-014, docs/die-interface.md).
//
// Masters, served one transaction at a time in round-robin order: the core
// (bus_*), the JTAG debug module (dm_*), requests arriving over the I/O
// link (device DMA: MRD/MWR), and the init engine's POST codes.
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
// DRAM offsets interleave 64-byte lines over the subchannels: off[7:6].
module fabric
  import vibe86_platform_pkg::*;
(
  input  logic         clk,
  input  logic         rst_n,
  input  logic         dram_ready,
  input  logic [40:0]  dram_size,
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
  output logic [127:0] c_rdata,
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
                         OP_CPL = 8'h08, OP_CPLE = 8'h09;

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

  // ------------------------------------------------------------ transactions
  typedef enum logic [3:0] {
    F_IDLE, F_DRAM, F_DRAMWAIT, F_TXHDR, F_TXADDR, F_TXDATA, F_RXWAIT, F_DONE,
    F_INRX, F_INDATA, F_CPLHDR, F_CPLDATA
  } fst_e;
  fst_e st;

  typedef enum logic [1:0] {MS_CORE, MS_DBG, MS_LINK, MS_POST} msel_e;
  msel_e who;
  logic [1:0] rr;                    // round-robin pointer

  logic        t_we, t_io;
  logic [63:0] t_addr;
  logic [2:0]  t_size;
  logic [511:0] t_data;              // write data (aligned to the line for DRAM) / read result
  logic [3:0]  t_nflits, t_flit;
  logic [7:0]  t_tag;                // inbound request tag (link)
  logic        t_err;

  // inbound requests from the link: header, address, data flits
  logic [22:0] in_hdr;                // opcode, tag, size, data flits
  logic [63:0] in_addr;
  logic [3:0]  in_n;
  logic        in_pending;            // a complete inbound request is held
  logic [511:0] in_data;
  logic        rx_cpl;                // completion for our outstanding request

  // DRAM decode of a physical address: hit and offset
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

  wire [5:0] lo = dd[5:0];            // byte within the DRAM line
  wire [1:0] sc = dd[7:6];            // subchannel
  /* verilator lint_off UNUSEDSIGNAL */
  wire [3:0] dd_unused = dd[40:37];   // beyond the 4-slot maximum (128 GiB)
  /* verilator lint_on UNUSEDSIGNAL */

  // a held inbound request stops further inbound requests, never the
  // completion of our own request (that would deadlock the link)
  assign rx_ready = !in_pending || rx_cpl ||
                    (st == F_RXWAIT && in_n == 0 && (rx_flit[7:0] == OP_CPL || rx_flit[7:0] == OP_CPLE));

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      st <= F_IDLE;
      who <= MS_CORE;
      rr <= '0;
      c_ack <= 1'b0;
      c_err <= 1'b0;
      c_rdata <= '0;
      d_ack <= 1'b0;
      d_err <= 1'b0;
      d_rdata <= '0;
      p_ack <= 1'b0;
      m_valid <= '0;
      m_we <= 1'b0;
      m_off <= '0;
      m_wdata <= '0;
      m_be <= '0;
      tx_valid <= 1'b0;
      tx_flit <= '0;
      t_we <= 1'b0;
      t_io <= 1'b0;
      t_addr <= '0;
      t_size <= '0;
      t_data <= '0;
      t_nflits <= '0;
      t_flit <= '0;
      t_tag <= '0;
      t_err <= 1'b0;
      in_hdr <= '0;
      in_addr <= '0;
      in_n <= '0;
      in_pending <= 1'b0;
      in_data <= '0;
      rx_cpl <= 1'b0;
      cf8 <= '0;
      hb_cmd <= 16'h0006;
      smram <= 8'h02;
      for (int i = 0; i < 7; i++) pam[i] <= '0;
      mce_ue <= 1'b0;
      mce_addr <= '0;
    end else begin
      c_ack <= 1'b0;
      d_ack <= 1'b0;
      p_ack <= 1'b0;
      mce_ue <= 1'b0;

      // ---------------------------------------------------- link receive
      // completions for our request go to the transaction; requests from
      // the I/O die (DMA) are assembled and served in their turn
      if (rx_valid && rx_ready) begin
        if (in_n == 0 && st == F_RXWAIT && (rx_flit[7:0] == OP_CPL || rx_flit[7:0] == OP_CPLE)
            && rx_flit[15:8] == 8'h00 && !rx_cpl) begin
          rx_cpl <= 1'b1;
          t_flit <= '0;
          t_nflits <= rx_flit[22:19];
          t_err <= rx_flit[7:0] == OP_CPLE;
          if (rx_flit[22:19] == 0) st <= F_DONE;
        end else if (rx_cpl) begin
          // completion data flits
          t_data[64 * t_flit +: 64] <= rx_flit;
          t_flit <= t_flit + 4'd1;
          if (t_flit + 4'd1 == t_nflits) begin
            rx_cpl <= 1'b0;
            st <= F_DONE;
          end
        end else if (in_n == 0) begin
          in_hdr <= rx_flit[22:0];
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

      case (st)
        F_IDLE: begin
          // round robin over the masters with a request
          logic [3:0] want;
          logic [1:0] pick;
          logic found;
          want = {p_req, in_pending, d_req && !d_ack, c_req && !c_ack};
          found = 1'b0;
          pick = rr;
          for (int k = 0; k < 4; k++)
            if (!found && want[2'(rr + 2'(k))]) begin
              found = 1'b1;
              pick = 2'(rr + 2'(k));
            end
          if (found) begin
            rr <= pick + 2'd1;
            who <= msel_e'(pick);
            t_err <= 1'b0;
            t_tag <= 8'h00;
            case (pick)
              2'd0: begin
                t_we <= c_we; t_io <= c_io; t_addr <= c_addr; t_size <= c_size;
                t_data <= {384'b0, c_wdata};
              end
              2'd1: begin
                t_we <= d_we; t_io <= 1'b0; t_addr <= d_addr; t_size <= d_size;
                t_data <= {448'b0, d_wdata};
              end
              2'd2: begin
                t_we <= in_hdr[7:0] == OP_MWR; t_io <= 1'b0; t_addr <= in_addr;
                t_size <= in_hdr[18:16]; t_data <= in_data; t_tag <= in_hdr[15:8];
                in_pending <= 1'b0;
              end
              default: begin
                t_we <= 1'b1; t_io <= 1'b1; t_addr <= 64'h80; t_size <= 3'd0;
                t_data <= {504'b0, p_code};
              end
            endcase
            st <= F_DRAM;
          end
        end
        F_DRAM: begin
          // route: the host bridge, DRAM, or the link
          if (t_io && t_addr[15:0] == 16'hCF8 && t_size == 3'd2) begin
            if (t_we) cf8 <= t_data[31:0];
            else t_data <= {480'b0, cf8};
            if (t_we) begin
              st <= F_TXHDR;                     // the I/O die keeps its copy too
              t_nflits <= 4'd1;
            end else st <= F_DONE;
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
            st <= F_DONE;
          end else if (!t_io && dd[41]) begin
            // DRAM: one 64-byte line (a transfer must not cross it)
            logic [6:0] n;
            n = 7'(1) << t_size;
            if (7'(lo) + n > 7'd64) begin
              t_err <= 1'b1;
              st <= (who == MS_LINK) ? (t_we ? F_IDLE : F_CPLHDR) : F_DONE;
            end else begin
              m_off <= dd[36:8];
              m_we <= t_we;
              m_wdata <= t_data << (8 * lo);
              m_be <= ((64'(1) << n) - 64'd1) << lo;
              m_valid <= 4'(1) << sc;
              st <= F_DRAMWAIT;
            end
          end else if (who == MS_LINK) begin
            // a device's DMA outside DRAM: an unsupported request (a read
            // completes with an error; a posted write is dropped)
            t_err <= 1'b1;
            st <= t_we ? F_IDLE : F_CPLHDR;
          end else begin
            t_nflits <= t_we ? ((t_size == 3'd4) ? 4'd2 : (t_size == 3'd6) ? 4'd8 : 4'd1) : 4'd0;
            st <= F_TXHDR;
          end
        end
        F_DRAMWAIT: begin
          if (m_valid != 0 && (m_valid & m_ready) != 0) m_valid <= '0;
          if (m_rsp[sc]) begin
            t_data <= m_rdata[sc] >> (8 * lo);
            if (m_ue[sc] && !t_we) begin
              t_err <= 1'b1;
              mce_ue <= 1'b1;
              mce_addr <= t_addr;
            end
            // a device's memory write is posted: no completion
            st <= (who == MS_LINK) ? (t_we ? F_IDLE : F_CPLHDR) : F_DONE;
          end
        end
        // ---------------------------------------------------- link: our request
        F_TXHDR: begin
          tx_valid <= 1'b1;
          tx_flit <= {32'b0, 9'b0, t_nflits, t_size, 8'h00,
                      t_io ? (t_we ? OP_IOWR : OP_IORD) : (t_we ? OP_MWR : OP_MRD)};
          if (tx_valid && tx_ready) begin
            tx_flit <= t_addr;
            st <= F_TXADDR;
          end
        end
        F_TXADDR: if (tx_ready) begin
          t_flit <= '0;
          if (t_nflits != 0) begin
            tx_flit <= t_data[63:0];
            st <= F_TXDATA;
          end else begin
            tx_valid <= 1'b0;
            st <= F_RXWAIT;
          end
        end
        F_TXDATA: if (tx_ready) begin
          if (t_flit + 4'd1 == t_nflits) begin
            tx_valid <= 1'b0;
            // memory writes are posted; port writes wait for their completion
            st <= (t_io || !t_we) ? F_RXWAIT : F_DONE;
          end else begin
            tx_flit <= t_data[64 * 32'(t_flit + 4'd1) +: 64];
            t_flit <= t_flit + 4'd1;
          end
        end
        F_RXWAIT: ;                          // the completion arrives above
        // ---------------------------------------------------- link: a completion for a DMA
        F_CPLHDR: begin
          tx_valid <= 1'b1;
          tx_flit <= {32'b0, 9'b0, (t_we || t_err) ? 4'd0 : ((t_size == 3'd6) ? 4'd8 : (t_size == 3'd4) ? 4'd2 : 4'd1),
                      t_size, t_tag, t_err ? OP_CPLE : OP_CPL};
          t_nflits <= (t_we || t_err) ? 4'd0 : ((t_size == 3'd6) ? 4'd8 : (t_size == 3'd4) ? 4'd2 : 4'd1);
          if (tx_valid && tx_ready) begin
            if (t_nflits == 0) begin
              tx_valid <= 1'b0;
              st <= F_IDLE;
            end else begin
              tx_flit <= t_data[63:0];
              t_flit <= '0;
              st <= F_CPLDATA;
            end
          end
        end
        F_CPLDATA: if (tx_ready) begin
          if (t_flit + 4'd1 == t_nflits) begin
            tx_valid <= 1'b0;
            st <= F_IDLE;
          end else begin
            tx_flit <= t_data[64 * 32'(t_flit + 4'd1) +: 64];
            t_flit <= t_flit + 4'd1;
          end
        end
        F_DONE: begin
          case (who)
            MS_CORE: begin
              c_ack <= 1'b1;
              c_err <= t_err;
              c_rdata <= t_data[127:0];
            end
            MS_DBG: begin
              d_ack <= 1'b1;
              d_err <= t_err;
              d_rdata <= t_data[63:0];
            end
            MS_POST: p_ack <= 1'b1;
            default: ;
          endcase
          st <= F_IDLE;
        end
        default: st <= F_IDLE;
      endcase
    end
  end

endmodule
