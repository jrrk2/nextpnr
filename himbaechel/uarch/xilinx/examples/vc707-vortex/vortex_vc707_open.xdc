# VC707 pins for the open (nextpnr-himbaechel) flow.
set_property -dict {PACKAGE_PIN E19 IOSTANDARD LVDS} [get_ports IO_CLK_P]
set_property -dict {PACKAGE_PIN E18 IOSTANDARD LVDS} [get_ports IO_CLK_N]
create_clock -period 5.000 -name sysclk [get_ports IO_CLK_P]

## CPU_RESET pushbutton
set_property -dict {PACKAGE_PIN AV40 IOSTANDARD LVCMOS18} [get_ports IO_RST]

## User LEDs LD0-7
set_property -dict {PACKAGE_PIN AM39 IOSTANDARD LVCMOS18} [get_ports {LED[0]}]
set_property -dict {PACKAGE_PIN AN39 IOSTANDARD LVCMOS18} [get_ports {LED[1]}]
set_property -dict {PACKAGE_PIN AR37 IOSTANDARD LVCMOS18} [get_ports {LED[2]}]
set_property -dict {PACKAGE_PIN AT37 IOSTANDARD LVCMOS18} [get_ports {LED[3]}]
set_property -dict {PACKAGE_PIN AR35 IOSTANDARD LVCMOS18} [get_ports {LED[4]}]
set_property -dict {PACKAGE_PIN AP41 IOSTANDARD LVCMOS18} [get_ports {LED[5]}]
set_property -dict {PACKAGE_PIN AP42 IOSTANDARD LVCMOS18} [get_ports {LED[6]}]
set_property -dict {PACKAGE_PIN AU39 IOSTANDARD LVCMOS18} [get_ports {LED[7]}]

## The clock placement the clkgen_vc707 MMCM topology was proven with.
set_property LOC BUFGCTRL_X0Y18 [get_cells clkgen.clk_mac_bufg]
set_property LOC BUFGCTRL_X0Y16 [get_cells clkgen.clk_sys_bufg]
set_property LOC MMCME2_ADV_X1Y5 [get_cells clkgen.mmcm]
