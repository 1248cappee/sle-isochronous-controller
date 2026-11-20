log_wave /tb_bis_event_window/dut/*
log_wave /tb_bis_event_window/sampled_us
set scope /tb_bis_event_window/dut
foreach signal {clk rst submit_valid submit_ready desc_time_us active_token active_slot active_channel radio_start cancel_valid phy_done irq_valid irq_status irq_ack} {
    add_wave $scope/$signal
}
add_wave /tb_bis_event_window/sampled_us
save_wave_config bis_event_window.wcfg
open_vcd bis_event_window.vcd
log_vcd /tb_bis_event_window/dut/*
run all
close_vcd
quit
