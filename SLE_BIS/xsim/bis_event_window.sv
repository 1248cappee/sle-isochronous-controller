`timescale 1ns/1ps
// Single-descriptor timing/IRQ reference for the B-side BSP contract.
// One clock domain. now_us is a monotonic, synchronized hardware timestamp.
module bis_event_window (
    input logic clk, rst,
    input logic [63:0] now_us,
    input logic submit_valid,
    output logic submit_ready,
    input logic [63:0] desc_token, desc_time_us,
    input logic [31:0] desc_duration_us,
    input logic [29:0] desc_slot,
    input logic [6:0] desc_channel,
    input logic cancel_valid, phy_done,
    input logic [63:0] cancel_token, phy_token,
    output logic radio_start,
    output logic [63:0] active_token,
    output logic [29:0] active_slot,
    output logic [6:0] active_channel,
    output logic irq_valid,
    output logic [2:0] irq_status,
    input logic irq_ack
);
    typedef enum logic [1:0] {IDLE, ARMED, RUNNING, REPORT} state_t;
    localparam logic [2:0] DONE=0, MISSED=1, CANCELLED=2, TIMEOUT=3;
    state_t state;
    logic [63:0] target_us;
    logic [31:0] duration_us;

    assign submit_ready = state == IDLE;
    assign irq_valid = state == REPORT;

    always_ff @(posedge clk) begin
        radio_start <= 0;
        if (rst) begin
            state <= IDLE;
            active_token <= 0;
            active_slot <= 0;
            active_channel <= 0;
            target_us <= 0;
            duration_us <= 0;
            irq_status <= DONE;
        end else case (state)
            IDLE: if (submit_valid) begin
                active_token <= desc_token;
                active_slot <= desc_slot;
                active_channel <= desc_channel;
                target_us <= desc_time_us;
                duration_us <= desc_duration_us;
                if (now_us > desc_time_us || desc_duration_us == 0) begin
                    irq_status <= MISSED;
                    state <= REPORT;
                end else if (now_us == desc_time_us) begin
                    radio_start <= 1;
                    state <= RUNNING;
                end else state <= ARMED;
            end
            ARMED: begin
                if (cancel_valid && cancel_token == active_token) begin
                    irq_status <= CANCELLED;
                    state <= REPORT;
                end else if (now_us == target_us) begin
                    radio_start <= 1;
                    state <= RUNNING;
                end else if (now_us > target_us) begin
                    irq_status <= MISSED;
                    state <= REPORT;
                end
            end
            RUNNING: begin
                if (cancel_valid && cancel_token == active_token) begin
                    irq_status <= CANCELLED;
                    state <= REPORT;
                end else if (now_us - target_us >= duration_us) begin
                    irq_status <= TIMEOUT;
                    state <= REPORT;
                end else if (phy_done && phy_token == active_token) begin
                    irq_status <= DONE;
                    state <= REPORT;
                end
            end
            REPORT: if (irq_ack) state <= IDLE;
        endcase
    end
endmodule
