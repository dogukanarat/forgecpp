# Simulates the encoder and the input filter with Icarus Verilog (apt install iverilog).
all: tb
	vvp -n tb | tee tb.log
	@grep -q '^PASS' tb.log

tb: ../rtl/la_input.v ../rtl/la_encoder.v tb_la.v
	iverilog -g2012 -Wall -o $@ $^

clean:
	rm -f tb tb.log

.PHONY: all clean
