// tb_user_core.v - black-box test of the tpg user_core (no shell, no SPI).
//
// Locks onto CSYNC (the first broad vsync pulse after an equalizing pulse is
// line 3, x = 0), then follows the field with its own line/clock counters and
// checks EVERY clock of a whole field, for every pattern, against a model
// derived here from apps/tpg/main/video_timing.h's ns values and
// video_pattern.c's pattern rules (rounded half up to 40 ns clocks). The
// model does not share the design's hand-typed boundary tables, so a typo in
// either shows up as a mismatch.
//
// Text (pattern 9 and the overlay) is modelled from the raw 5x7 font columns
// and label strings in tpg_text_model.vh (generated from the same sources as
// the design's ROMs); the cell grid, centering and overlay box are computed
// here, not taken from the design.
//
// Also checks: output enables, tie-offs, register read-back, pattern and
// overlay changes only at a field start, the field counter, out-of-range
// pattern -> black.
`timescale 1ns/1ps
`include "fw2_profile.vh"

module tb_user_core;

// ------------------------------------------------------------------ spec (from ns)
localparam integer H_TOTAL   = (63560 + 20) / 40;          // 1589
localparam integer HSYNC     = (4700 + 20) / 40;           // 118
localparam integer ACT_START = (4700 + 4700 + 20) / 40;    // 235
localparam integer ACTIVE    = (52640 + 20) / 40;          // 1316
localparam integer HALF      = (H_TOTAL + 1) / 2;          // 795
localparam integer EQ        = (2300 + 20) / 40;           // 58
localparam integer SERR      = (4700 + 20) / 40;           // 118
localparam integer LINE_W    = (320 + 20) / 40;            // 8
localparam integer DOT_W     = (300 + 20) / 40;            // 8
localparam integer V_TOTAL   = 262;
localparam integer ACT_FIRST = 3 + 3 + 3 + 10;             // 19
localparam integer ACT_LINES = 241;
localparam integer NPAT      = 10;                          // 0..8 shared, 9 focus
// Text: 40 x 30 cells of 8 x 8 pixels, a pixel 4 clocks, centered in the
// active width; overlay box = label cell rows 24-25 + 3 lines above/below.
localparam integer PIX       = 4;
localparam integer CELL      = 8 * PIX;                     // 32 clocks
localparam integer TXT_X0    = (ACTIVE - 40 * CELL) / 2;    // 18
localparam integer BOX_Y0    = 24 * 8 - 3;                  // 189
localparam integer BOX_Y1    = 25 * 8 + 7 + 3;              // 210 (row 25 glyph rows 0..6)

`include "tpg_text_model.vh"

// ------------------------------------------------------------------ DUT
reg        clk = 1'b0;
reg        rst_n = 1'b0;
reg  [6:0] reg_addr = 7'h00;
reg  [7:0] reg_wdata = 8'h00;
reg        reg_wr = 1'b0;
wire [7:0] reg_rdata;
wire [9:0] hdr_out, hdr_oe, rp_out;
wire       fifo_tx_ready, fifo_rx_valid;
wire [7:0] fifo_rx_data;

always #20 clk = ~clk;   // 25 MHz

user_core dut (
  .clk(clk), .rst_n(rst_n),
  .hdr_in(10'b0), .hdr_out(hdr_out), .hdr_oe(hdr_oe),
  .rp_in(10'b0), .rp_out(rp_out),
  .reg_addr(reg_addr), .reg_wdata(reg_wdata), .reg_wr(reg_wr), .reg_rd(1'b0), .reg_rdata(reg_rdata),
  .fifo_tx_data(8'h00), .fifo_tx_valid(1'b0), .fifo_tx_ready(fifo_tx_ready),
  .fifo_rx_data(fifo_rx_data), .fifo_rx_valid(fifo_rx_valid), .fifo_rx_ready(1'b0)
);

// Pin map, stated independently of the design: GPIO -> header signal.
//   R2 GPIO14 spi_sclk, R1 GPIO15 spi_mosi, R0 GPIO26
//   G2 GPIO11 uart_rts, G1 GPIO12 spi_miso, G0 GPIO13 spi_cs
//   B2 GPIO8 uart_tx,   B1 GPIO9 uart_rx,   B0 GPIO10 uart_cts
wire       csync = hdr_out[`FW2_SIG_GPIO27];
wire [2:0] pin_r = {hdr_out[`FW2_SIG_SPI_SCLK], hdr_out[`FW2_SIG_SPI_MOSI], hdr_out[`FW2_SIG_GPIO26]};
wire [2:0] pin_g = {hdr_out[`FW2_SIG_UART_RTS], hdr_out[`FW2_SIG_SPI_MISO], hdr_out[`FW2_SIG_SPI_CS]};
wire [2:0] pin_b = {hdr_out[`FW2_SIG_UART_TX], hdr_out[`FW2_SIG_UART_RX], hdr_out[`FW2_SIG_UART_CTS]};
wire [8:0] pin_rgb = {pin_r, pin_g, pin_b};

// ------------------------------------------------------------------ column model
// Per active clock: bar (0..7), third (0..2), crosshatch vertical index
// (-1 = none) and dot flag, from the ns geometry in video_timing.h.
integer col_t [0:ACTIVE-1];
integer thr_t [0:ACTIVE-1];
integer vk_t  [0:ACTIVE-1];
integer dot_t [0:ACTIVE-1];

integer i, k, s;
integer box_x0, box_x1;       // overlay box, active clocks [box_x0, box_x1)
integer bc0, bc1, bn, bp;
initial begin
  // Box: 3 pixels either side of the glyphs (pixel columns 1..5 of a cell)
  // of the widest-reaching label, over every pattern name and the timing line.
  bc0 = 40; bc1 = 0;
  for (bp = 0; bp < 17; bp = bp + 1) begin
    bn = (bp == 16) ? tm_len(0, 1) : tm_len(bp, 0);
    if (bn > 0) begin
      if ((40 - bn) / 2 < bc0)      bc0 = (40 - bn) / 2;
      if ((40 - bn) / 2 + bn > bc1) bc1 = (40 - bn) / 2 + bn;
    end
  end
  box_x0 = TXT_X0 + bc0 * CELL + 1 * PIX - 3 * PIX;
  box_x1 = TXT_X0 + (bc1 - 1) * CELL + 6 * PIX + 3 * PIX;
  $display("  text box: columns %0d..%0d, active clocks %0d..%0d, lines %0d..%0d",
           bc0, bc1 - 1, box_x0, box_x1 - 1, BOX_Y0, BOX_Y1 - 1);

  for (i = 0; i < ACTIVE; i = i + 1) begin
    col_t[i] = i * 8 / ACTIVE;
    thr_t[i] = i * 3 / ACTIVE;
    vk_t[i]  = -1;
    dot_t[i] = 0;
  end
  for (k = 0; k <= 8; k = k + 1) begin
    s = (k * 6540 + 20) / 40;                     // TPG_GRID_PITCH_NS
    for (i = s; i < s + LINE_W; i = i + 1) vk_t[i] = k;
  end
  for (k = 0; k < 8; k = k + 1) begin
    s = (k * 6540 + 320 + 2960 + 20) / 40;        // line + TPG_GRID_DOT_SIDE_NS
    for (i = s; i < s + DOT_W; i = i + 1) dot_t[i] = 1;
  end
end

// ------------------------------------------------------------------ expected values
function exp_sync_low(input integer line, input integer x);
  begin
    if (line < 3 || (line >= 6 && line < 9))
      exp_sync_low = (x < EQ) || (x >= HALF && x < HALF + EQ);
    else if (line < 6)
      exp_sync_low = (x < HALF - SERR) || (x >= HALF && x < H_TOTAL - SERR);
    else
      exp_sync_low = (x < HSYNC);
  end
endfunction

function [8:0] grey(input integer v);
  grey = {v[2:0], v[2:0], v[2:0]};
endfunction

// Glyph pixel (px 0..7, gy 0..7 within the cell) for ASCII ch: font5x7
// column px-1 for px 1..5, rows 0..6.
function font_px(input integer ch, input integer px, input integer gy);
  reg [7:0] colbits;
  begin
    font_px = 1'b0;
    if (ch >= 32 && px >= 1 && px <= 5 && gy <= 6) begin
      colbits = tm_font(ch, px - 1);
      font_px = colbits[gy];
    end
  end
endfunction

// Text at active (ax, ay): -1 if no text element covers it, else its rgb.
function integer exp_text(input integer pat, input integer ovl, input integer ax, input integer ay);
  integer col, px, row, gy, sel, n, s0, ch;
  reg     grid;
  begin
    exp_text = -1;
    grid = (ay < 240) && (ax >= TXT_X0) && (ax < TXT_X0 + 40 * CELL);
    col = 0; px = 0;
    if (ax >= TXT_X0) begin
      col = (ax - TXT_X0) / CELL;
      px  = ((ax - TXT_X0) % CELL) / PIX;
    end
    row = ay / 8;
    gy  = ay % 8;
    if (ovl != 0 && ay >= BOX_Y0 && ay < BOX_Y1 && ax >= box_x0 && ax < box_x1) begin
      ch = 32;
      if (grid && (row == 24 || row == 25)) begin
        sel = row - 24;
        n   = tm_len(pat, sel);
        s0  = (40 - n) / 2;
        if (col >= s0 && col < s0 + n) ch = tm_char(pat, sel, col - s0);
      end
      exp_text = font_px(ch, px, gy) ? 9'o777 : 9'o000;
    end else if (pat == 9) begin
      exp_text = (grid && font_px(72, px, gy)) ? 9'o777 : 9'o000;   // 'H'
    end
  end
endfunction

function [8:0] exp_rgb(input integer pat, input integer ovl, input integer line, input integer x);
  integer ax, ay, band, row, lvl, vk, t;
  reg     gridrow, dotrow;
  begin
    exp_rgb = 9'd0;
    if (line >= ACT_FIRST && line < ACT_FIRST + ACT_LINES && x >= ACT_START && x < ACT_START + ACTIVE) begin
      ax = x - ACT_START;
      ay = line - ACT_FIRST;
      t = exp_text(pat, ovl, ax, ay);
      band = ay / (ACT_LINES / 8); if (band > 7) band = 7;   // gradient step = level
      row  = ay / (ACT_LINES / 6); if (row > 5) row = 5;
      gridrow = (ay % 40) == 0;
      dotrow  = (ay % 40) == 19 || (ay % 40) == 20;
      vk = vk_t[ax];
      if (t >= 0) exp_rgb = t[8:0];
      else case (pat)
        0: case (col_t[ax])   // BAR_R/G/B in video_pattern.c
             0: exp_rgb = {3'd7, 3'd7, 3'd7};
             1: exp_rgb = {3'd7, 3'd7, 3'd0};
             2: exp_rgb = {3'd0, 3'd7, 3'd7};
             3: exp_rgb = {3'd0, 3'd7, 3'd0};
             4: exp_rgb = {3'd7, 3'd0, 3'd7};
             5: exp_rgb = {3'd7, 3'd0, 3'd0};
             6: exp_rgb = {3'd0, 3'd0, 3'd7};
             default: exp_rgb = 9'd0;
           endcase
        1: begin
             lvl = band;
             exp_rgb = (thr_t[ax] == 0) ? {lvl[2:0], 6'd0} :
                       (thr_t[ax] == 1) ? {3'd0, lvl[2:0], 3'd0} : {6'd0, lvl[2:0]};
           end
        2: exp_rgb = grey(band);
        3: exp_rgb = grey(((row + col_t[ax]) % 2 == 0) ? 7 : 0);
        4: exp_rgb = grey((gridrow || vk >= 0 || (dotrow && dot_t[ax] != 0)) ? 7 : 0);
        5: begin
             if (gridrow)               exp_rgb = grey(7);
             else if (vk < 0)           exp_rgb = 9'd0;
             else case (vk % 4)
               0: exp_rgb = {3'd7, 6'd0};
               1: exp_rgb = {3'd0, 3'd7, 3'd0};
               2: exp_rgb = {6'd0, 3'd7};
               default: exp_rgb = grey(7);
             endcase
           end
        6: exp_rgb = {3'd7, 6'd0};
        7: exp_rgb = {6'd0, 3'd7};
        8: exp_rgb = {3'd0, 3'd7, 3'd0};
        default: exp_rgb = 9'd0;   // out of range -> black
      endcase
    end
  end
endfunction

// ------------------------------------------------------------------ lock + per-clock check
reg     locked = 1'b0;
integer tline = 0, tx = 0;
integer low_run = 0, prev_pulse = 0;
integer written = 0;          // last value written to 0x10
integer exp_pat = 0;          // pattern the current field should show
integer written_ovl = 1;      // last value written to 0x13 bit 0
integer exp_ovl = 1;          // overlay state of the current field
integer errors = 0;
integer field_no = 0;         // fields started since lock
integer checked = 0;          // clocks compared
reg     exp_low;
reg [8:0] exp_c;

// Expected samples for the current line. exp_rgb/exp_sync_low depend on the
// line only through its kind (eq / vsync / blank / active) and, for active
// lines, band, checker row, grid row and dot row (and, on text lines, the
// line itself), so the 1589-sample line is rebuilt only when that signature
// changes (~40 times a field, every line for the focus pattern): per-clock
// function calls made the run ~25 minutes.
reg       line_low [0:H_TOTAL-1];
reg [8:0] line_rgb [0:H_TOTAL-1];
integer   cur_sig = -1;

function integer line_sig(input integer pat, input integer ovl, input integer line);
  integer ay, kind, band, row, textline;
  begin
    if (line < 3 || (line >= 6 && line < 9)) kind = 1;
    else if (line < 6)                       kind = 2;
    else if (line < ACT_FIRST || line >= ACT_FIRST + ACT_LINES) kind = 3;
    else                                     kind = 4;
    ay = line - ACT_FIRST;
    textline = (kind == 4) && (pat == 9 || (ovl != 0 && ay >= BOX_Y0 && ay < BOX_Y1));
    line_sig = ((ovl != 0) * 16 + pat) * 5 + kind;              // <= 159
    line_sig = line_sig * 1000 + (textline ? ay + 1 : 0);       // <= 159241
    line_sig = line_sig * 10000;                                // < 1.6e9
    if (kind == 4) begin
      band = ay / (ACT_LINES / 8); if (band > 7) band = 7;
      row  = ay / (ACT_LINES / 6); if (row > 5) row = 5;
      line_sig = line_sig + band * 1000 + row * 100 + ((ay % 40) == 0) * 10
                 + ((ay % 40) == 19 || (ay % 40) == 20);
    end
  end
endfunction

task build_line(input integer pat, input integer ovl, input integer line);
  integer sig, x;
  begin
    sig = line_sig(pat, ovl, line);
    if (sig != cur_sig) begin
      for (x = 0; x < H_TOTAL; x = x + 1) begin
        line_low[x] = exp_sync_low(line, x);
        line_rgb[x] = exp_rgb(pat, ovl, line, x);
      end
      cur_sig = sig;
    end
  end
endtask

always @(negedge clk) begin
  if (rst_n) begin
    if (!locked) begin
      if (!csync) begin
        low_run = low_run + 1;
      end else if (low_run > 0) begin
        if (low_run == HALF - SERR && prev_pulse == EQ) begin
          locked = 1'b1;      // this sample is line 3, x = 677
          tline = 3;
          tx = HALF - SERR;
          build_line(exp_pat, exp_ovl, tline);
        end
        prev_pulse = low_run;
        low_run = 0;
      end
    end

    if (locked) begin
      if (tline == 0 && tx == 0) begin
        exp_pat = (written >= 0 && written < NPAT) ? written : 15;
        exp_ovl = written_ovl;
        field_no = field_no + 1;
        $display("  field %0d starts (pattern %0d, overlay %0d) at %0t", field_no, exp_pat, exp_ovl, $time);
      end
      if (tx == 0) build_line(exp_pat, exp_ovl, tline);
      exp_low = line_low[tx];
      exp_c = line_rgb[tx];
      checked = checked + 1;
      if (csync !== ~exp_low || pin_rgb !== exp_c) begin
        errors = errors + 1;
        if (errors <= 10)
          $display("MISMATCH pattern %0d overlay %0d line %0d x %0d: csync %b (want %b) rgb %o (want %o)",
                   exp_pat, exp_ovl, tline, tx, csync, ~exp_low, pin_rgb, exp_c);
      end
      tx = tx + 1;
      if (tx == H_TOTAL) begin
        tx = 0;
        tline = (tline + 1) % V_TOTAL;
      end
    end
  end
end

// ------------------------------------------------------------------ stimulus
task write_reg(input [6:0] a, input [7:0] d);
begin
  @(negedge clk); reg_addr = a; reg_wdata = d; reg_wr = 1'b1;
  @(negedge clk); reg_wr = 1'b0;
end
endtask

task read_reg(input [6:0] a, output [7:0] d);
begin
  @(negedge clk); reg_addr = a; #1 d = reg_rdata;
end
endtask

task check(input cond, input [8*64-1:0] msg);
begin
  if (!cond) begin $display("FAIL: %0s", msg); $fatal(1); end
end
endtask

task wait_pos(input integer line, input integer x);   // until the checker is at (line, x)
begin
  @(negedge clk);
  while (!(locked && tline == line && tx == x)) @(negedge clk);
end
endtask

integer p, f0;
reg [7:0] v, f1, f2;

initial begin
  repeat (3) @(negedge clk);
  check(hdr_oe == 10'h3FF, "all ten header signals output-enabled");
  check(csync === 1'b1 && pin_rgb === 9'd0, "in reset: black, sync idle");
  rst_n = 1'b1;

  read_reg(7'h10, v); check(v == 8'd0, "PATTERN resets to 0 (color bars)");
  read_reg(7'h12, v); check(v == NPAT, "COUNT reads 10");
  read_reg(7'h13, v); check(v == 8'd1, "CONTROL resets to 1 (overlay on)");
  read_reg(7'h14, v); check(v == 8'd0, "unused register reads 0");
  check(rp_out == 10'b0 && fifo_rx_valid == 1'b0 && fifo_tx_ready == 1'b0 && fifo_rx_data == 8'h00,
        "unused outputs tied off");

  wait (locked);
  // Field counter: +1 per field. Then a mid-field write: the field in
  // progress must keep showing color bars to its end (checked per clock).
  wait_pos(100, 0); read_reg(7'h11, f1);
  wait_pos(100, 0); read_reg(7'h11, f2);
  check(f2 == f1 + 8'd1, "FIELDS advances by one per field");
  write_reg(7'h10, 8'd1);
  written = 1;

  // Every pattern, then an out-of-range value. Write late in a field (after
  // the last active line, so ~1 field per pattern); the current field must
  // finish with the old pattern (the checker only switches its expectation at
  // the next field start), then one whole field is checked.
  for (p = 0; p <= NPAT; p = p + 1) begin
    wait_pos(V_TOTAL - 1, 0);
    write_reg(7'h10, p[7:0]);
    written = p;
    read_reg(7'h10, v); check(v == p[7:0], "PATTERN reads back what was written");
    f0 = field_no;
    wait (field_no == f0 + 1);   // the new pattern's field has started; the next
                                 // write lands at its end, so it is checked whole
  end
  // Overlay off: the focus pattern (whose own H's then fill the label rows)
  // and crosshatch + dots, one whole field each.
  wait_pos(V_TOTAL - 1, 0);
  write_reg(7'h13, 8'h00); write_reg(7'h10, 8'd9);
  written_ovl = 0; written = 9;
  read_reg(7'h13, v); check(v == 8'd0, "CONTROL reads back 0");
  f0 = field_no; wait (field_no == f0 + 1);
  wait_pos(V_TOTAL - 1, 0);
  write_reg(7'h10, 8'd4);
  written = 4;
  f0 = field_no; wait (field_no == f0 + 1);
  wait (field_no == f0 + 2);     // let the last one finish

  check(checked > (NPAT + 3) * V_TOTAL * H_TOTAL, "checked at least thirteen whole fields");
  if (errors != 0) begin
    $display("FAIL: %0d mismatching clocks", errors);
    $fatal(1);
  end
  $display("tb_user_core (tpg) PASS: %0d clocks checked, %0d fields", checked, field_no);
  $finish;
end

initial begin
  #800_000_000;   // 800 ms of simulated time (~48 fields)
  $display("FAIL: timeout (locked=%0d field_no=%0d)", locked, field_no);
  $fatal(1);
end

endmodule
