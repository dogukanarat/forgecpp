`timescale 1ns/1ps
// la_input: the four inputs from the shield, made safe to use and optionally filtered.
//
//   pins -> 2-FF synchronizer (clk) -> glitch filter (per sample tick) -> levels
//
// The filter passes a new level only after it was seen on `glitch` + 1
// consecutive sample ticks (glitch = 0: no filter). Every edge is then late by
// the same `glitch` ticks, so times between edges stay exact.
`default_nettype none

module la_input #(
    parameter N = 4
) (
    input  wire          clk,
    input  wire          rst,
    input  wire          tick,       // one clk wide, at the sample rate
    input  wire [15:0]   glitch,     // GLITCH register (ticks)
    input  wire [N-1:0]  pins,       // from the shield's comparators (asynchronous)
    output reg  [N-1:0]  levels      // synchronous, filtered
);
    (* ASYNC_REG = "TRUE" *) reg [N-1:0] s1, s2;
    always @(posedge clk) begin
        s1 <= pins;
        s2 <= s1;
    end

    genvar i;
    generate
        for (i = 0; i < N; i = i + 1) begin : ch
            reg [15:0] count;
            always @(posedge clk) begin
                if (rst) begin
                    levels[i] <= s2[i];
                    count     <= 16'd0;
                end else if (tick) begin
                    if (s2[i] == levels[i]) begin
                        count <= 16'd0;
                    end else if (count >= glitch) begin
                        levels[i] <= s2[i];
                        count     <= 16'd0;
                    end else begin
                        count <= count + 16'd1;
                    end
                end
            end
        end
    endgenerate
endmodule

`default_nettype wire
