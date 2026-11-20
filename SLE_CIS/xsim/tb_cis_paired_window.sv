`timescale 1ns/1ps
module tb_cis_paired_window;
    bit clk; always #500 clk = !clk;
    logic rst=1, valid=0,ready, first_tx=0,cancel=0,phy_done=0,phy_second=0,irq_ack=0;
    logic start_phase,phase_second,tx_direction,irq_valid;
    logic [63:0] now_us=0,sampled_us,desc_token=0,first_us=0,first_end_us=0,second_us=0,end_us=0;
    logic [63:0] cancel_token=0,phy_token=0,active_token;
    logic [29:0] desc_slot=0,active_slot;
    logic [6:0] desc_channel=0,active_channel;
    logic [2:0] irq_status;
    cis_paired_window dut(.*);

    always @(posedge clk) begin
        sampled_us <= now_us;
        if (rst) now_us <= 0;
        else now_us <= now_us+1;
    end
    integer launches=0,completions=0;
    bit previous_irq=0;
    always @(posedge clk) begin
        #1;
        if(start_phase) launches++;
        if(irq_valid && !previous_irq) completions++;
        previous_irq=irq_valid;
    end
    task automatic tick; @(negedge clk); endtask
    task automatic assert_true(input logic test,input string why);
        if(test !== 1'b1) $fatal(1,"CIS RTL failure at %d us: %s",sampled_us,why);
    endtask
    task automatic no_launch_until(input integer goal);
        while(now_us<goal) begin
            tick();
            assert_true(!start_phase && !irq_valid,"unexpected phase/IRQ");
        end
    endtask
    task automatic submit_pair(input integer token,t0,t1,t2,t3,slot,ch,tx);
        assert_true(ready,"queue busy");
        desc_token=token; first_us=t0; first_end_us=t1; second_us=t2; end_us=t3;
        first_tx=tx; desc_slot=slot; desc_channel=ch;
        valid=1; tick(); valid=0;
        desc_token='1; desc_slot='1; desc_channel='1; first_tx=!first_tx;
        first_us='1; first_end_us='1; second_us='1; end_us='1;
    endtask
    task automatic report(input integer token,status);
        repeat(3) begin
            assert_true(irq_valid && irq_status==status && active_token==token && !ready,
                        "IRQ contents held until ACK");
            tick();
        end
        irq_ack=1; tick(); irq_ack=0;
        assert_true(ready && !irq_valid,"acknowledge");
    endtask

    integer fd,i,token,t0,t2,t3,slot,ch,tx,air,n;
    integer expect_time[0:3],expect_slot[0:3],expect_ch[0:3];
    initial begin
        expect_time[0]=1000; expect_time[1]=1500; expect_time[2]=3000; expect_time[3]=3400;
        expect_slot[0]=1073741820; expect_slot[1]=0; expect_slot[2]=0; expect_slot[3]=8;
        expect_ch[0]=53; expect_ch[1]=41; expect_ch[2]=41; expect_ch[3]=32;
        fd=$fopen("events.txt","r"); assert_true(fd!=0,"open A descriptors");
        repeat(2) tick(); rst=0;
        for(i=0;i<4;i++) begin
            n=$fscanf(fd,"%d %d %d %d %d %d %d %d",token,t0,t2,t3,slot,ch,tx,air);
            assert_true(n==8 && token==i+1 && t0==expect_time[i],"descriptor trace");
            assert_true(t2==t0+125 && t3==t0+245 && tx==(i<2) && air==100,"directional offsets");
            while(now_us+4<t0) tick();
            submit_pair(token,t0,t0+air,t2,t3,slot,ch,tx);
            no_launch_until(t0); tick();
            assert_true(start_phase && !phase_second && tx_direction==tx && !irq_valid,
                        "first direction at exact anchor");
            assert_true(active_slot==expect_slot[i] && active_channel==expect_ch[i],"wrap and hopping");
            phy_done=1; phy_token=token-1; phy_second=0; tick();
            phy_token=token; phy_second=1; tick();
            assert_true(!irq_valid && !start_phase,"stale token/phase IRQ");
            phy_done=0;
            no_launch_until(t0+air);
            phy_done=1; phy_second=0; tick(); phy_done=0;
            no_launch_until(t2); tick();
            assert_true(start_phase && phase_second && tx_direction!=tx && !irq_valid,
                        "second direction at specified turnaround");
            phy_done=1; phy_token=token; phy_second=0; tick(); phy_done=0;
            assert_true(!irq_valid,"stale first phase completion");
            no_launch_until(t3); phy_done=1; phy_second=1; tick(); phy_done=0;
            report(token,0);
            $display("TRACE tag=%0d first=%0d second=%0d end=%0d first_tx=%0d slot=%0d ch=%0d",token,t0,t2,t3,tx,slot,ch);
        end
        $fclose(fd);
        t0=now_us;
        submit_pair(5,t0+10,t0+15,t0+17,t0+20,0,1,0);
        cancel=1; cancel_token=5; tick(); cancel=0; report(5,2);
        t0=now_us; submit_pair(6,t0-1,t0+2,t0+3,t0+5,0,1,1); report(6,1);

        t0=now_us; submit_pair(7,t0,t0+3,t0+4,t0+6,0,1,1);
        assert_true(start_phase && !phase_second,"immediate arm");
        cancel=1; cancel_token=5; tick(); cancel=0;
        no_launch_until(t0+3); tick(); report(7,3);

        // First phase finishes, second starts; missing second END must time out.
        t0=now_us; submit_pair(8,t0,t0+2,t0+3,t0+6,0,1,1);
        no_launch_until(t0+2); phy_token=8; phy_second=0; phy_done=1; tick(); phy_done=0;
        tick(); assert_true(start_phase && phase_second,"second timeout path");
        no_launch_until(t0+6); tick(); report(8,3);
        t0=now_us; submit_pair(9,t0,t0+2,t0+3,t0+7,0,1,1);
        no_launch_until(t0+2); phy_token=9; phy_second=0; phy_done=1; tick(); phy_done=0;
        tick(); assert_true(phase_second && start_phase,"cancel in second phase");
        cancel=1; cancel_token=9; phy_done=1; phy_token=9; phy_second=1;
        tick(); cancel=0; phy_done=0; report(9,2);
        assert_true(launches==13 && completions==9,"phase/IRQ total");
        // Actual descriptors emitted from the negotiated G/T endpoint run.
        // Independent hopping vectors: ID 0x123456 at slot 48, ID 0x654321
        // at slots 80 and 83. The old ID at slot 80 would select channel 31.
        expect_time[0]=6000; expect_time[1]=10000; expect_time[2]=10375;
        expect_slot[0]=48; expect_slot[1]=80; expect_slot[2]=83;
        expect_ch[0]=56; expect_ch[1]=59; expect_ch[2]=35;
        fd=$fopen("e2e_events.txt","r"); assert_true(fd!=0,"open negotiated endpoint trace");
        for(i=0;i<3;i++) begin
            n=$fscanf(fd,"%d %d %d %d %d %d %d %d",token,t0,t2,t3,slot,ch,tx,air);
            assert_true(n==8 && t0==expect_time[i] && tx==1,"negotiated anchor and direction");
            assert_true(t2==t0+250 && t3==t0+350 && air==200,"endpoint durations");
            no_launch_until(t0);
            submit_pair(token,t0,t0+air,t2,t3,slot,ch,tx);
            assert_true(start_phase && sampled_us==t0 && !phase_second && tx_direction,
                        "first phase after control update");
            assert_true(active_slot==expect_slot[i] && active_channel==expect_ch[i],
                        "old and new link hopping at update boundary");
            $display("LINK_TRACE tag=%0d first=%0d second=%0d end=%0d slot=%0d ch=%0d",token,t0,t2,t3,slot,ch);
            if(i==2) begin
                no_launch_until(10377);
                cancel_token=token; cancel=1; tick(); cancel=0;
                assert_true(sampled_us==10377 && !start_phase,"disconnect takes effect in first phase");
                report(token,2);
                no_launch_until(t3+1);  // no feedback window after the disconnect
                $display("PASS endpoint disconnect at 10377 us suppresses planned second phase");
            end else begin
                no_launch_until(t0+air); phy_token=token; phy_second=0; phy_done=1; tick(); phy_done=0;
                no_launch_until(t2); tick();
                assert_true(start_phase && phase_second && !tx_direction && sampled_us==t2,
                            "negotiated feedback window");
                no_launch_until(t3); phy_second=1; phy_done=1; tick(); phy_done=0;
                report(token,0);
            end
        end
        $fclose(fd);
        assert_true(launches==18 && completions==12,"combined trace phase/IRQ total");
        $display("CIS_XSIM_PASS phases=%0d completions=%0d",launches,completions);
        $finish;
    end
    initial begin #12000000; $fatal(1,"watchdog"); end
endmodule
