# VC707 Vortex GPGPU (xc7vx485tffg1761-2)

The Vortex RISC-V GPGPU (https://github.com/vortexgpgpu/vortex), one core of
4 warps x 4 threads, RV32I with no floating point, standing alone on the
VC707: 256 KB of block RAM as device memory, a DCR launch sequencer in place
of the host driver, and the eight user LEDs as the only output.  The kernel
in the block RAM counts and stores the top bits of its counter to the LED
register; LED7 is the GPU's busy flag.

Like the other SoC entries it ships a **netlist**, not Verilog:
`vortex_vc707.json.gz` (79 MB raw, 3.3 MB here) with the program in the
RAMB36 `INIT` parameters.  Unlike most of them it is not yosys' output: the
Verilator synthesis fork lowered the SystemVerilog (interfaces, modports,
unpacked array ports, streaming operators) to primitive cells and ran abc on
each module instance separately, 93 partitions in all.

## What it covers

    cells      183656     the largest netlist in the gate
    LUT        110725     LUT1..LUT6, 22% of the part
    FDRE/FDSE   63872
    RAM64X1D     6815     register files, operand collectors, cache tags
    CARRY4       1931
    RAMB36E1      241     23 of them initialised (the kernel)
    DSP48E1        59
    MMCME2_ADV      1     with two BUFGs, LOC-constrained as clkgen_vc707 was proven

- **Distributed RAM at scale.** No other entry packs anything like 6,815
  RAM64X1D; the SLICEM supply and the packer's LUTRAM shapes are the first
  thing this design tests.
- **Router cost.** 4.15 M wires at convergence, 71 router2 iterations.
- **A long, flat critical path**: about 20 LUT levels from the D-cache flush
  arbiter to a cache-bank tag register, 3.8 ns of logic and 14.9 ns of
  routing across the die.  Fmax is where this design is sensitive, and it is
  the number to watch, not gate on.

## On hardware

This netlist, placed and routed with the gate-wide flags
(`--placer-heap-timingweight 60`, router2, hold-fix), assembled and loaded
over JTAG on 2026-10-26: LED7 lit while the kernel runs and LEDs 6..0 count.

Measured on an Apple-silicon desktop: place 2 x (HeAP + SA) about 45 min,
router2 3 h 11 min, 3 h 57 min in all; routed Fmax 53.61 MHz against the
50 MHz the MMCM produces from the 200 MHz system clock.  On a hosted runner
that may not fit the 6-hour job limit, which is why the entry is stretch and
its timeout is the maximum.

The post-route router1 legality check found one net carrying a wire no sink
path uses, a stub left by a rip-up.  All sink paths were intact, the FASM was
written and the bitstream works; the check now reports the net by name
instead of asserting.

## Provenance

    netlist   ~/vortex (branch vc707-synthgui, 06832e0a8), hw/syn/xilinx/vc707,
              `make synth` through synthgui with the Verilator fork at 4f716ba7d
              (branch vortex-sv); defines SYNTHESIS NDEBUG VX_CFG_XLEN=32
              VX_CFG_XLEN_32 VX_CFG_EXT_F_DISABLE
    xdc       hw/syn/xilinx/vc707/vortex_vc707_open.xdc: the 200 MHz LVDS
              system clock pair, CPU_RESET, LD0..7, and the BUFG/MMCM LOCs

The top has four ports (IO_CLK_P/N, IO_RST, LED[7:0]), all constrained by
the xdc.
