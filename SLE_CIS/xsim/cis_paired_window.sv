`timescale 1ns/1ps
// Single-clock board-interface reference. Pairs are programmed atomically.
// now_us is monotonic. The RF block owns actual airtime and CRC verdicts.
module cis_paired_window (
    input logic clk, rst,
    input logic [63:0] now_us,
    input logic valid,
    output logic ready,
    input logic [63:0] desc_token, first_us, first_end_us, second_us, end_us,
    input logic [29:0] desc_slot,
    input logic [6:0] desc_channel,
    input logic first_tx, cancel, phy_done, phy_second, irq_ack,
    input logic [63:0] cancel_token, phy_token,
    output logic start_phase, phase_second, tx_direction, irq_valid,
    output logic [63:0] active_token,
    output logic [29:0] active_slot,
    output logic [6:0] active_channel,
    output logic [2:0] irq_status
);
    typedef enum logic [2:0] {IDLE, ARMED, FIRST, TURN, SECOND, REPORT} state_t;
    state_t state;
    localparam logic [2:0] DONE=0, MISSED=1, CANCELLED=2, TIMEOUT=3, INVALID=4;
    logic [63:0] first_at, first_end, second_at, second_end;
    logic first_direction;
    wire phy_match = phy_done && phy_token == active_token;
    assign ready = state == IDLE;
    assign irq_valid = state == REPORT;

    always_ff @(posedge clk) begin
        start_phase <= 0;
        if (rst) begin
            state <= IDLE;
            active_token <= 0;
            active_slot <= 0;
            active_channel <= 0;
            phase_second <= 0;
            tx_direction <= 0;
            first_direction <= 0;
            first_at <= 0;
            first_end <= 0;
            second_at <= 0;
            second_end <= 0;
            irq_status <= DONE;
        end else if (!ready && !irq_valid && cancel && cancel_token == active_token) begin
            state <= REPORT;
            irq_status <= CANCELLED;
        end else case (state)
            IDLE: if (valid) begin
                active_token <= desc_token;
                active_slot <= desc_slot;
                active_channel <= desc_channel;
                first_direction <= first_tx;
                first_at <= first_us;
                first_end <= first_end_us;
                second_at <= second_us;
                second_end <= end_us;
                phase_second <= 0;
                tx_direction <= first_tx;
                if (first_end_us <= first_us || second_us < first_end_us || end_us <= second_us) begin
                    state <= REPORT;
                    irq_status <= INVALID;
                end else if (now_us > first_us) begin
                    state <= REPORT;
                    irq_status <= MISSED;
                end else if (now_us == first_us) begin
                    state <= FIRST;
                    start_phase <= 1;
                end else state <= ARMED;
            end
            ARMED: begin
                if (now_us > first_at) begin
                    state <= REPORT;
                    irq_status <= MISSED;
                end else if (now_us == first_at) begin
                    state <= FIRST;
                    start_phase <= 1;
                end
            end
            FIRST: begin
                if (phy_match && !phy_second && now_us <= first_end) begin
                    if (now_us == second_at) begin
                        state <= SECOND;
                        phase_second <= 1;
                        tx_direction <= !first_direction;
                        start_phase <= 1;
                    end else state <= TURN;
                end else if (now_us >= first_end) begin
                    state <= REPORT;
                    irq_status <= TIMEOUT;
                end
            end
            TURN: begin
                if (now_us == second_at) begin
                    state <= SECOND;
                    phase_second <= 1;
                    tx_direction <= !first_direction;
                    start_phase <= 1;
                end else if (now_us > second_at) begin
                    state <= REPORT;
                    irq_status <= MISSED;
                end
            end
            SECOND: begin
                if (phy_match && phy_second && now_us <= second_end) begin
                    state <= REPORT;
                    irq_status <= DONE;
                end else if (now_us >= second_end) begin
                    state <= REPORT;
                    irq_status <= TIMEOUT;
                end
            end
            REPORT: if (irq_ack) state <= IDLE;
        endcase
    end
endmodule
