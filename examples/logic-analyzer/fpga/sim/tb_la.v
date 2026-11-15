// Self-checking testbench of la_encoder and la_input (Icarus Verilog):
//   make -C fpga/sim        (or: iverilog -g2012 -o tb ../rtl/*.v tb_la.v && vvp tb)
//
// The levels the encoder saw on every tick are kept; the words it wrote are
// decoded as the program does (src/wire.hpp) and must give back the same
// levels at the same ticks, with and without a full FIFO.
`timescale 1ns/1ps
`default_nettype none

module tb;
    reg clk = 0;
    always #5 clk = ~clk;  // 100 MHz

    reg        rst = 1, enable = 0, fifo_full = 0;
    reg        tick = 0;
    reg  [3:0] levels = 4'b0101;
    reg  [25:0] heartbeat = 26'd50;
    wire       fifo_wr;
    wire [31:0] fifo_data, lost;
    wire       overflow;

    la_encoder dut (.clk(clk), .rst(rst), .enable(enable), .tick(tick), .levels(levels), .heartbeat(heartbeat),
                    .fifo_full(fifo_full), .fifo_wr(fifo_wr), .fifo_data(fifo_data), .lost(lost), .overflow(overflow));

    // ---------------------------------------------------------------- recording
    localparam MAXT = 200000;
    reg  [3:0]  truth [0:MAXT];
    reg  [31:0] words [0:MAXT];
    integer nwords = 0, rel = 0, started = 0, errors = 0;

    always @(posedge clk) begin
        if (fifo_wr) begin
            words[nwords] = fifo_data;
            nwords = nwords + 1;
        end
        if (!started && dut.running && dut.start_due && !fifo_full) begin
            started = 1;
            rel = tick ? 1 : 0;
            truth[rel] = levels;
            truth[0] = levels;
        end else if (started && tick) begin
            rel = rel + 1;
            truth[rel] = levels;
        end
    end

    // ---------------------------------------------------------------- decoding
    task check(input [255:0] name, input integer expect_gaps);
        integer i, t, abs, gaps;
        reg [1:0] typ;
        reg [3:0] cur, lv;
        reg [25:0] d;
        begin
            abs = 0; gaps = 0; cur = 0;
            if (nwords == 0 || words[0][31:30] != 2'd3) begin
                $display("FAIL %0s: no start word", name);
                errors = errors + 1;
            end
            cur = words[0][29:26];
            if (cur !== truth[0]) begin
                $display("FAIL %0s: start levels %b, expected %b", name, cur, truth[0]);
                errors = errors + 1;
            end
            for (i = 1; i < nwords; i = i + 1) begin
                typ = words[i][31:30]; lv = words[i][29:26]; d = words[i][25:0];
                if (d > heartbeat && typ != 2'd2 && typ != 2'd0) begin
                    $display("FAIL %0s: word %0d delta %0d above the heartbeat", name, i, d);
                    errors = errors + 1;
                end
                if (typ != 2'd2) begin
                    for (t = abs + 1; t < abs + d && t <= rel; t = t + 1) begin
                        if (truth[t] !== cur) begin
                            $display("FAIL %0s: tick %0d reads %b, the words say %b", name, t, truth[t], cur);
                            errors = errors + 1;
                            t = abs + d;  // one message per word
                        end
                    end
                end else begin
                    gaps = gaps + 1;
                end
                abs = abs + d;
                if (abs <= rel && truth[abs] !== lv && typ != 2'd0) begin
                    $display("FAIL %0s: word %0d (type %0d) at tick %0d: %b, expected %b", name, i, typ, abs, lv, truth[abs]);
                    errors = errors + 1;
                end
                if (typ == 2'd1 && lv == cur) begin
                    $display("FAIL %0s: edge word %0d changes nothing", name, i);
                    errors = errors + 1;
                end
                if (typ == 2'd0 && lv != cur) begin
                    $display("FAIL %0s: time word %0d changes the levels", name, i);
                    errors = errors + 1;
                end
                cur = (typ == 2'd0) ? cur : lv;
            end
            if (expect_gaps >= 0 && (gaps > 0) != (expect_gaps > 0)) begin
                $display("FAIL %0s: %0d gap words", name, gaps);
                errors = errors + 1;
            end
            $display("%0s: %0d ticks, %0d words, %0d gap words, %0d lost", name, rel, nwords, gaps, lost);
        end
    endtask

    // ---------------------------------------------------------------- stimulus
    // Changes are made just before a tick, as the sampled pins would be.
    integer div = 1, k, cyc;
    task run(input integer ticks, input integer change_per_1000, input integer full_per_1000, input integer full_len);
        integer n, full_left;
        begin
            full_left = 0;
            for (n = 0; n < ticks; n = n + 1) begin
                for (cyc = 0; cyc < div; cyc = cyc + 1) begin
                    @(negedge clk);
                    tick = (cyc == div - 1);
                    if (tick && ($urandom % 1000) < change_per_1000) levels = levels ^ (($urandom % 15) + 1);
                    if (full_left > 0) begin
                        full_left = full_left - 1;
                        fifo_full = 1;
                    end else if (($urandom % 1000) < full_per_1000) begin
                        full_left = full_len;
                        fifo_full = 1;
                    end else begin
                        fifo_full = 0;
                    end
                end
            end
            @(negedge clk);
            tick = 0;
            fifo_full = 0;
            repeat (5) @(negedge clk);
        end
    endtask

    task restart;
        begin
            @(negedge clk);
            enable = 0;
            repeat (3) @(negedge clk);
            nwords = 0; started = 0; rel = 0;
            enable = 1;
        end
    endtask

    // ---------------------------------------------------------------- glitch filter
    reg  [3:0] pins = 0;
    reg  [15:0] glitch = 16'd3;
    wire [3:0] filtered;
    la_input #(.N(4)) inp (.clk(clk), .rst(rst), .tick(1'b1), .glitch(glitch), .pins(pins), .levels(filtered));

    initial begin
        repeat (5) @(negedge clk);
        rst = 0;

        div = 1; heartbeat = 50;
        restart;
        run(30000, 100, 0, 0);
        check("tick every clk, 10 % changes", 0);

        div = 3; heartbeat = 7;
        restart;
        run(20000, 300, 0, 0);
        check("tick every 3rd clk, short heartbeat", 0);

        div = 1; heartbeat = 1000;
        restart;
        run(30000, 200, 5, 40);
        check("FIFO full now and then", 1);
        if (lost == 0 || !overflow) begin $display("FAIL: nothing counted as lost"); errors = errors + 1; end

        // glitch filter: a 2-tick pulse is dropped, a 6-tick one passes, 3 ticks late
        @(negedge clk); pins = 4'b0001;
        repeat (2) @(negedge clk); pins = 4'b0000;
        repeat (10) @(negedge clk);
        if (filtered[0] !== 1'b0) begin $display("FAIL glitch: a 2-tick pulse passed"); errors = errors + 1; end
        pins = 4'b0001;
        repeat (4) @(negedge clk);
        if (filtered[0] !== 1'b0) begin $display("FAIL glitch: passed too early"); errors = errors + 1; end
        repeat (3) @(negedge clk);
        if (filtered[0] !== 1'b1) begin $display("FAIL glitch: a 7-tick level did not pass"); errors = errors + 1; end
        $display("glitch filter: checked");

        if (errors == 0) $display("PASS");
        else $display("FAILED: %0d errors", errors);
        $finish;
    end
endmodule

`default_nettype wire
