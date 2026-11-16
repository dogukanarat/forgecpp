`timescale 1ns/1ps
// la_encoder: turns the four input levels into the change words of the
// capture buffer (fpga/README.md, src/wire.hpp):
//
//   31 30 | 29 28 27 26 | 25 ................. 0
//   type  | levels      | delta: sample ticks since the previous word
//
//   type 0 time   no change: a heartbeat, or a delta that would not fit
//   type 1 edge   the inputs changed; levels are the new ones
//   type 2 gap    words were dropped (FIFO full) before this one; the time is right
//   type 3 start  first word after enable: the levels at the start
//
// One word per change of any input (several inputs changing on one tick are
// one word), and a time word at least every `heartbeat` ticks so the reader
// knows time goes on while nothing changes.
//
// The output is the write side of a FIFO (e.g. XPM_FIFO_SYNC): fifo_wr for one
// clk with fifo_data, never while fifo_full. When the FIFO is full a word is
// dropped (lost counts it); the ticks keep counting, and once there is room a
// gap word (after time words for what does not fit in 26 bits) says so.
`default_nettype none

module la_encoder (
    input  wire        clk,
    input  wire        rst,
    input  wire        enable,      // CTRL.enable: a rising edge starts a capture
    input  wire        tick,        // sample tick (clk / (DIVIDER + 1))
    input  wire [3:0]  levels,      // from la_input
    input  wire [25:0] heartbeat,   // HEARTBEAT register, 1 .. 2^26-1
    input  wire        fifo_full,
    output reg         fifo_wr,
    output reg  [31:0] fifo_data,
    output reg  [31:0] lost,        // LOST register
    output reg         overflow     // STATUS.overflow (sticky until enable)
);
    localparam [1:0] T_TIME = 2'd0, T_EDGE = 2'd1, T_GAP = 2'd2, T_START = 2'd3;
    localparam [39:0] MAX = 40'h3FFFFFF;  // 2^26 - 1

    reg        running, enable_q, start_due, gap_due;
    reg [3:0]  last;    // levels of the last word written
    reg [39:0] count;   // ticks since the last word written (more while dropping)

    function [31:0] word(input [1:0] t, input [3:0] l, input [25:0] d);
        word = {t, l, d};
    endfunction

    wire [39:0] count_next = count + (tick ? 40'd1 : 40'd0);
    wire        changed    = tick && (levels != last);
    wire        beat       = tick && (count_next >= {14'd0, heartbeat});

    always @(posedge clk) begin
        fifo_wr  <= 1'b0;
        enable_q <= enable;
        if (rst || !enable) begin
            running   <= 1'b0;
            start_due <= 1'b0;
            gap_due   <= 1'b0;
            count     <= 40'd0;
            if (rst) begin
                lost     <= 32'd0;
                overflow <= 1'b0;
            end
        end else if (!enable_q) begin
            // enable rose: a new capture
            running   <= 1'b1;
            start_due <= 1'b1;
            gap_due   <= 1'b0;
            count     <= 40'd0;
            last      <= levels;
            lost      <= 32'd0;
            overflow  <= 1'b0;
        end else if (running) begin
            if (start_due) begin
                if (!fifo_full) begin
                    fifo_wr   <= 1'b1;
                    fifo_data <= word(T_START, last, 26'd0);
                    start_due <= 1'b0;
                    count     <= tick ? 40'd1 : 40'd0;
                end else begin
                    count <= count_next;
                end
            end else if (gap_due) begin
                // Room again: first the time that does not fit, then the gap word.
                if (fifo_full) begin
                    count <= count_next;
                    if (changed || beat) lost <= lost + 32'd1;  // still dropping
                end else if (count_next > MAX) begin
                    fifo_wr   <= 1'b1;
                    fifo_data <= word(T_TIME, last, MAX[25:0]);
                    count     <= count_next - MAX;
                end else begin
                    fifo_wr   <= 1'b1;
                    fifo_data <= word(T_GAP, levels, count_next[25:0]);
                    last      <= levels;
                    count     <= 40'd0;
                    gap_due   <= 1'b0;
                end
            end else if (changed || beat) begin
                if (!fifo_full) begin
                    fifo_wr   <= 1'b1;
                    fifo_data <= word(changed ? T_EDGE : T_TIME, levels, count_next[25:0]);
                    last      <= levels;
                    count     <= 40'd0;
                end else begin
                    lost     <= lost + 32'd1;
                    overflow <= 1'b1;
                    gap_due  <= 1'b1;
                    count    <= count_next;
                end
            end else begin
                count <= count_next;
            end
        end
    end
endmodule

`default_nettype wire
