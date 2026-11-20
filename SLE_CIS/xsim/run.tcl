log_wave /tb_cis_paired_window/dut/*
log_wave /tb_cis_paired_window/sampled_us
set dut /tb_cis_paired_window/dut
foreach signal {clk rst valid ready active_token active_slot active_channel start_phase phase_second tx_direction cancel phy_done phy_second irq_valid irq_status irq_ack} {
    add_wave $dut/$signal
}
add_wave /tb_cis_paired_window/sampled_us
save_wave_config cis_paired_window.wcfg
open_vcd cis_paired_window.vcd
log_vcd /tb_cis_paired_window/dut/*
run all
close_vcd
quit
