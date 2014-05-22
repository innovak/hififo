`timescale 1ns / 1ps

module vna_dsp
  (
   output [3:0]     pci_exp_txp,
   output [3:0]     pci_exp_txn,
   input [3:0] 	    pci_exp_rxp,
   input [3:0] 	    pci_exp_rxn,
   input 	    sys_clk_p,
   input 	    sys_clk_n,
   input 	    sys_rst_n,
   output reg [3:0] led = 4'h5
   );
   
   wire 	clock;
   wire 	user_reset, user_lnk_up;
   wire 	s_axis_tx_tready;
   reg [63:0] 	s_axis_tx_tdata = 0;
   reg [7:0] 	s_axis_tx_tkeep = 0;
   reg 		s_axis_tx_tlast = 0;
   reg 		s_axis_tx_tvalid = 0;
   wire [63:0] 	m_axis_rx_tdata;
   wire 	m_axis_rx_tlast;
   wire 	m_axis_rx_tvalid;
      
   wire 	cfg_to_turnoff;
   wire [7:0] 	cfg_bus_number;
   wire [4:0] 	cfg_device_number;
   wire [2:0] 	cfg_function_number;
   wire [15:0] 	pcie_id = {cfg_bus_number, cfg_device_number, cfg_function_number};
   wire 	sys_rst_n_c;
   wire 	sys_clk;
   reg 		pcie_reset = 1'b1;
   reg 		cfg_turnoff_ok = 0;
   
   IBUF   pci_reset_ibuf (.O(sys_rst_n_c), .I(sys_rst_n));
   IBUFDS_GTE2 refclk_ibuf (.O(sys_clk), .ODIV2(), .I(sys_clk_p), .CEB(1'b0), .IB(sys_clk_n));

   reg [23:0] 	rx_rid_tag = 0;
   reg [7:0] 	rx_address = 0;
   reg 		read_valid = 0;
   reg 		write_valid = 0;
   reg [63:0] 	write_data = 0;
   
   always @(posedge clock) 
     begin
	pcie_reset <= user_reset | ~user_lnk_up;
	// fix this
   	cfg_turnoff_ok <= cfg_to_turnoff; // not if a completion is pending
	if(write_valid)
	  led[2:0] <= write_data[2:0];
	if(s_axis_tx_tvalid & s_axis_tx_tready)
	  led[3] <= 1'b1;
     end  
   
   
   // state
   // 0 = 1st 2 DWORDs
   // 1 = 2nd 2 DWORDs
   // 2 = additional DWORDs
   // 3 = waiting for tlast
   reg [1:0] 	    rx_state = 0;
   reg [31:0] 	    rx_previous_dw = 0;
   reg 		    rx_length_is_2 = 0;
   reg 		    is_write_32 = 0;
   reg 		    is_cpld = 0;
   reg 		    is_read_32 = 0;

   // receive
   always @ (posedge clock)
     begin
	if(rx_state == 0)
	  begin
	     is_write_32 <= m_axis_rx_tdata[30:24] == 7'b1000000;
	     is_cpld <= m_axis_rx_tdata[30:24] == 7'b1001010;
	     is_read_32 <= m_axis_rx_tdata[30:24] == 7'b0000000;
	     rx_length_is_2 <= m_axis_rx_tdata[9:0] == 10'd2;
	     rx_rid_tag <= m_axis_rx_tdata[63:40];
	  end
	if(rx_state == 1)
	  begin
	     rx_address <= m_axis_rx_tdata[10:3];
	  end
	if(m_axis_rx_tvalid)
	  begin
	     write_data <= {m_axis_rx_tdata[31:0],rx_previous_dw};
	     rx_previous_dw <= m_axis_rx_tdata[63:32];
	  end
	if(pcie_reset)
	  begin
	     rx_state <= 2'd0;
	  end
	else if(m_axis_rx_tvalid)
	  begin
	     if(m_axis_rx_tlast)
	       rx_state <= 2'd0;
	     else if(rx_state == 0) // 1st 2 DWORDs of header
	       rx_state <= 2'd1;
	     else if(rx_state == 1) // last DWORD of header, data DW0 (if present)
	       rx_state <= (is_write_32 | is_cpld) ? 2'd2 : 2'd3;
	  end // if (tvalid)
	write_valid <= (rx_state == 2) && m_axis_rx_tvalid;
	read_valid <= rx_length_is_2 && is_read_32 && (rx_state == 1) && m_axis_rx_tvalid; // memory read(rx_last_qw[30:24] = 0), len(rx_last_qw[9:0]) = 2 DW

     end // always @ (posedge clock)

   // transmit
   reg [1:0] tx_state = 0;
      
   always @(posedge clock)
     begin
	s_axis_tx_tvalid <= tx_state != 0;
	s_axis_tx_tkeep <= tx_state == 3 ? 8'h0F : 8'hFF;
	s_axis_tx_tlast <= tx_state == 3;
	case(tx_state)
	  1: s_axis_tx_tdata <= { pcie_id, 16'd8, 32'h4A000002 };
	  2: s_axis_tx_tdata <= { 32'hDEADBEEF, rx_rid_tag, 1'b0, rx_address[3:0], 3'd0 };
	  3: s_axis_tx_tdata <= rx_address;
	  default: s_axis_tx_tdata <= 64'h0;
	endcase
	if(pcie_reset)
	  tx_state <= 2'd0;
	else if(tx_state == 0 && read_valid)
	  tx_state <= 2'd1;
	else if(tx_state != 0)
	  tx_state <= tx_state + s_axis_tx_tready;
     end
   
   pcie_7x_0 pcie_7x_0_i
     (
      // PCI express data pairs
      .pci_exp_txn(pci_exp_txn), .pci_exp_txp(pci_exp_txp),
      .pci_exp_rxn(pci_exp_rxn), .pci_exp_rxp(pci_exp_rxp),
      // AXI
      .user_clk_out(clock),
      .user_reset_out(user_reset),
      .user_lnk_up(user_lnk_up),
      .user_app_rdy(),
      .s_axis_tx_tready(s_axis_tx_tready),
      .s_axis_tx_tdata(s_axis_tx_tdata),
      .s_axis_tx_tkeep(s_axis_tx_tkeep),
      .s_axis_tx_tuser(4'd0), // may want to assert 2 for cut through
      .s_axis_tx_tlast(s_axis_tx_tlast),
      .s_axis_tx_tvalid(s_axis_tx_tvalid),
      .m_axis_rx_tdata(m_axis_rx_tdata),
      .m_axis_rx_tkeep(),
      .m_axis_rx_tlast(m_axis_rx_tlast),
      .m_axis_rx_tvalid(m_axis_rx_tvalid),
      .m_axis_rx_tready(1'b1), // always ready
      .m_axis_rx_tuser(),
      
      .tx_cfg_gnt(1'b1), .rx_np_ok(1'b1), .rx_np_req(1'b1),
      .cfg_trn_pending(1'b0),
      .cfg_pm_halt_aspm_l0s(1'b0), .cfg_pm_halt_aspm_l1(1'b0),
      .cfg_pm_force_state_en(1'b0), .cfg_pm_force_state(2'd0),
      .cfg_dsn(64'h0),
      .cfg_turnoff_ok(cfg_turnoff_ok),
      .cfg_pm_wake(1'b0), .cfg_pm_send_pme_to(1'b0),
      .cfg_ds_bus_number(8'b0),
      .cfg_ds_device_number(5'b0),
      .cfg_ds_function_number(3'b0),
      // flow control
      .fc_cpld(), .fc_cplh(), .fc_npd(), .fc_nph(),
      .fc_pd(), .fc_ph(), .fc_sel(3'd0),
      // configuration
      .cfg_device_number(cfg_device_number),
      .cfg_dcommand2(), .cfg_pmcsr_pme_status(),
      .cfg_status(), .cfg_to_turnoff(cfg_to_turnoff),
      .cfg_received_func_lvl_rst(),
      .cfg_dcommand(),
      .cfg_bus_number(cfg_bus_number),
      .cfg_function_number(cfg_function_number),
      .cfg_command(),
      .cfg_dstatus(),
      .cfg_lstatus(),
      .cfg_pcie_link_state(),
      .cfg_lcommand(),
      .cfg_pmcsr_pme_en(),
      .cfg_pmcsr_powerstate(),
      .tx_buf_av(),
      .tx_err_drop(),
      .tx_cfg_req(),
      // root port only
      .cfg_bridge_serr_en(),
      .cfg_slot_control_electromech_il_ctl_pulse(),
      .cfg_root_control_syserr_corr_err_en(),
      .cfg_root_control_syserr_non_fatal_err_en(),
      .cfg_root_control_syserr_fatal_err_en(),
      .cfg_root_control_pme_int_en(),
      .cfg_aer_rooterr_corr_err_reporting_en(),
      .cfg_aer_rooterr_non_fatal_err_reporting_en(),
      .cfg_aer_rooterr_fatal_err_reporting_en(),
      .cfg_aer_rooterr_corr_err_received(),
      .cfg_aer_rooterr_non_fatal_err_received(),
      .cfg_aer_rooterr_fatal_err_received(),
      // both
      .cfg_vc_tcvc_map(),
      // Management Interface
      .cfg_mgmt_di(32'h0),
      .cfg_mgmt_byte_en(4'h0),
      .cfg_mgmt_dwaddr(10'h0),
      .cfg_mgmt_wr_en(1'b0),
      .cfg_mgmt_rd_en(1'b0),
      .cfg_mgmt_wr_readonly(1'b0),
      .cfg_mgmt_wr_rw1c_as_rw(1'b0),
      .cfg_mgmt_do(),
      .cfg_mgmt_rd_wr_done(),
      // Error Reporting Interface
      .cfg_err_ecrc(1'b0),
      .cfg_err_ur(1'b0),
      .cfg_err_cpl_timeout(1'b0),
      .cfg_err_cpl_unexpect(1'b0),
      .cfg_err_cpl_abort(1'b0),
      .cfg_err_posted(1'b0),
      .cfg_err_cor(1'b0),
      .cfg_err_atomic_egress_blocked(1'b0),
      .cfg_err_internal_cor(1'b0),
      .cfg_err_malformed(1'b0),
      .cfg_err_mc_blocked(1'b0),
      .cfg_err_poisoned(1'b0),
      .cfg_err_norecovery(1'b0),
      .cfg_err_tlp_cpl_header(48'h0),
      .cfg_err_cpl_rdy(),
      .cfg_err_locked(1'b0),
      .cfg_err_acs(1'b0),
      .cfg_err_internal_uncor(1'b0),
      
      .cfg_err_aer_headerlog(128'h0), .cfg_aer_interrupt_msgnum(5'h0),
      .cfg_err_aer_headerlog_set(), .cfg_aer_ecrc_check_en(), .cfg_aer_ecrc_gen_en(),
      
      .cfg_interrupt(1'b0),
      .cfg_interrupt_rdy(),
      .cfg_interrupt_assert(1'b0),
      .cfg_interrupt_di(8'h0),
      .cfg_interrupt_do(),
      .cfg_interrupt_mmenable(),
      .cfg_interrupt_msienable(),
      .cfg_interrupt_msixenable(),
      .cfg_interrupt_msixfm(),
      .cfg_interrupt_stat(1'b0),
      .cfg_pciecap_interrupt_msgnum(5'h0),      
      .cfg_msg_received_err_cor(),
      .cfg_msg_received_err_non_fatal(),
      .cfg_msg_received_err_fatal(),
      .cfg_msg_received_pm_as_nak(),
      .cfg_msg_received_pme_to_ack(),
      .cfg_msg_received_assert_int_a(), .cfg_msg_received_assert_int_b(),
      .cfg_msg_received_assert_int_c(), .cfg_msg_received_assert_int_d(),
      .cfg_msg_received_deassert_int_a(), .cfg_msg_received_deassert_int_b(),
      .cfg_msg_received_deassert_int_c(), .cfg_msg_received_deassert_int_d(),
      .cfg_msg_received_pm_pme(),
      .cfg_msg_received_setslotpowerlimit(),
      .cfg_msg_received(),
      .cfg_msg_data(),
      .pl_directed_link_change(2'd0), .pl_directed_link_width(2'd0),
      .pl_directed_link_speed(1'b0), .pl_directed_link_auton(1'b0),
      .pl_upstream_prefer_deemph(1'b1), .pl_sel_lnk_rate(), .pl_sel_lnk_width(),
      .pl_ltssm_state(), .pl_lane_reversal_mode(),
      .pl_phy_lnk_up(), .pl_tx_pm_state(), .pl_rx_pm_state(), .pl_link_upcfg_cap(),
      .pl_link_gen2_cap(), .pl_link_partner_gen2_supported(),
      .pl_initial_link_width(), .pl_directed_change_done(),
      .pl_received_hot_rst(), .pl_transmit_hot_rst(1'b0),
      .pl_downstream_deemph_source(1'b0),
      .pcie_drp_clk(1'b1), .pcie_drp_en(1'b0), .pcie_drp_we(1'b0),
      .pcie_drp_addr(9'h0), .pcie_drp_di(16'h0),
      .pcie_drp_rdy(), .pcie_drp_do(),
      // these are the clock and reset from the card edge connector
      .sys_clk                                    ( sys_clk ),
      .sys_rst_n                                  ( sys_rst_n_c )
      );      
endmodule