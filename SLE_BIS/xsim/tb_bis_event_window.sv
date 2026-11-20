`timescale 1ns/1ps
module tb_bis_event_window;
    bit clk;
    always #500 clk = !clk; // 1 MHz, one simulation clock per microsecond
    logic rst=1;
    logic [63:0] now_us=0, sampled_us;
    always @(posedge clk) begin
        sampled_us <= now_us;
        if (rst) now_us <= 0; else now_us <= now_us + 1;
    end
    logic submit_valid=0, submit_ready, cancel_valid=0, phy_done=0, irq_ack=0;
    logic [63:0] desc_token=0,desc_time_us=0,cancel_token=0,phy_token=0,active_token;
    logic [31:0] desc_duration_us=0;
    logic [29:0] desc_slot=0,active_slot;
    logic [6:0] desc_channel=0,active_channel;
    logic radio_start,irq_valid;
    logic [2:0] irq_status;
    bis_event_window dut(.*);
    integer starts=0, irqs=0;
    bit previous_irq=0;
    always @(posedge clk) begin
        #1;
        if (radio_start) starts++;
        if (irq_valid && !previous_irq) irqs++;
        previous_irq=irq_valid;
    end

    task automatic check(input logic ok,input string what);
        if (ok !== 1'b1) $fatal(1,"FAIL at %0d us: %s",sampled_us,what);
    endtask
    // Drive at negedge and sample registered outputs after the next rising edge.
    task automatic clock_step;
        @(negedge clk);
    endtask
    task automatic send(input logic [63:0] token, target,
                        input integer duration, slot_no, channel);
        check(submit_ready,"descriptor queue busy");
        desc_token=token; desc_time_us=target; desc_duration_us=duration;
        desc_slot=slot_no; desc_channel=channel; submit_valid=1;
        clock_step();
        submit_valid=0;
        desc_token='1; desc_channel='1; desc_slot='1; // verify latched fields
    endtask
    task automatic report_and_ack(input logic [2:0] status,input logic [63:0] token);
        repeat(3) begin
            check(irq_valid && !submit_ready && active_token==token && irq_status==status,
                  "completion must remain stable until ACK");
            clock_step();
        end
        irq_ack=1; clock_step(); irq_ack=0;
        check(!irq_valid && submit_ready,"IRQ acknowledge");
    endtask
    integer fd,i,rc,slot_no,channel;
    logic [63:0] token,target;
    integer expected_channel[0:3];
    integer expected_slot[0:3];
    initial begin
        expected_channel[0]=55; expected_channel[1]=48; expected_channel[2]=34; expected_channel[3]=19;
        expected_slot[0]=1073741822; expected_slot[1]=0; expected_slot[2]=6; expected_slot[3]=8;
        fd=$fopen("events.txt","r"); check(fd!=0,"open C event trace");
        repeat(2) clock_step();
        rst=0;
        for(i=0;i<4;i++) begin
            rc=$fscanf(fd,"%d %d %d %d",token,target,slot_no,channel);
            check(rc==4,"read four trace columns");
            check(target==1625+1000*(i/2)+250*(i%2) && token==i+1,"C time and token");
            while(now_us+4<target) clock_step();
            send(token,target,200,slot_no,channel);
            while(sampled_us<target) begin
                check(!radio_start && !irq_valid && !submit_ready,"early start or release");
                clock_step();
            end
            check(radio_start && active_token==token,"start exactly at anchor");
            check(active_channel==expected_channel[i] && active_slot==expected_slot[i],"channel and wrap");
            $display("TRACE token=%0d start_us=%0d slot=%0d phy_channel=%0d",active_token,sampled_us,active_slot,active_channel);
            phy_token=token-1; phy_done=1; clock_step();
            check(!irq_valid && !radio_start,"stale PHY completion");
            phy_token=token; clock_step(); phy_done=0;
            report_and_ack(0,token);
        end
        $fclose(fd);
        send(5,now_us+4,10,0,10);
        cancel_token=5; cancel_valid=1; clock_step(); cancel_valid=0;
        report_and_ack(2,5);
        check(starts==4,"cancel before window suppresses start");
        $display("PASS cancel before start");

        send(6,now_us-1,10,0,10);
        report_and_ack(1,6);
        check(starts==4,"late descriptor suppresses start");
        $display("PASS late submission");

        send(7,now_us,3,0,10);
        check(radio_start,"same-cycle due submission");
        cancel_token=5; cancel_valid=1; clock_step(); cancel_valid=0;
        check(!irq_valid,"stale cancel");
        clock_step(); check(!irq_valid,"timeout too early");
        phy_done=1; phy_token=7; clock_step(); phy_done=0;
        report_and_ack(3,7);
        $display("PASS timeout at exclusive end and stale cancel");

        send(8,now_us,10,0,10);
        cancel_token=8; cancel_valid=1; phy_token=8; phy_done=1;
        clock_step(); cancel_valid=0; phy_done=0;
        report_and_ack(2,8);
        check(starts==6 && irqs==8,"total starts and completions");
        $display("PASS active cancellation priority and IRQ backpressure");
        $display("BIS_XSIM_PASS starts=%0d completions=%0d",starts,irqs);
        $finish;
    end
    initial begin #4000000; $fatal(1,"watchdog"); end
endmodule
