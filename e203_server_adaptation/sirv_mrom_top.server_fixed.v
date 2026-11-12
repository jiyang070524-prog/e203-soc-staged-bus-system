 /*                                                                      
 Copyright 2018-2020 Nuclei System Technology, Inc.                
                                                                         
 Licensed under the Apache License, Version 2.0 (the "License");         
 you may not use this file except in compliance with the License.        
 You may obtain a copy of the License at                                 
                                                                         
     http://www.apache.org/licenses/LICENSE-2.0                          
                                                                         
  Unless required by applicable law or agreed to in writing, software    
 distributed under the License is distributed on an "AS IS" BASIS,       
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and     
 limitations under the License.                                          
 */                                                                      
                                                                         
                                                                         
                                                                         
//=====================================================================
//
// Designer   : Bob Hu
//
// Description:
//  The module is to control the mask ROM 
//
// ====================================================================

module sirv_mrom_top #(
    parameter AW = 12,
    parameter DW = 32,
    parameter DP = 1024
)(
  //    * Bus cmd channel
  input  rom_icb_cmd_valid, // Handshake valid
  output rom_icb_cmd_ready, // Handshake ready
  input  [AW-1:0] rom_icb_cmd_addr, // Bus transaction start addr 
  input  rom_icb_cmd_read,   // Read or write

  //    * Bus RSP channel
  output rom_icb_rsp_valid, // Response valid 
  input  rom_icb_rsp_ready, // Response ready
  output rom_icb_rsp_err,   // Response error
  output [DW-1:0] rom_icb_rsp_rdata, 

  input  clk,
  input  rst_n
  );
        
  wire [DW-1:0] rom_dout; 

  reg rsp_valid, rsp_err;
  wire accepted = rom_icb_cmd_valid && rom_icb_cmd_ready;
  assign rom_icb_cmd_ready = rst_n && (!rsp_valid || rom_icb_rsp_ready);
  assign rom_icb_rsp_valid = rsp_valid;
  assign rom_icb_rsp_err = rsp_err;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin rsp_valid <= 1'b0; rsp_err <= 1'b0; end
    else if (rom_icb_cmd_ready) begin
      rsp_valid <= rom_icb_cmd_valid;
      if (accepted) rsp_err <= !rom_icb_cmd_read;
    end
  end
`ifdef FPGA
  reg [DW-1:0] sampled_data;
  always @(posedge clk) if (accepted && rom_icb_cmd_read) sampled_data <= rom_dout;
  assign rom_icb_rsp_rdata = sampled_data;
`else
  assign rom_icb_rsp_rdata = rom_dout;
`endif
`ifndef FPGA
ROM1KX32 u_sirv_mrom(
  .CENY    (),
  .AY      (),
  .Q       (rom_dout),
  .CLK     (clk),
  .CEN     (!(accepted && rom_icb_cmd_read)),
  .A       (rom_icb_cmd_addr[AW-1:2]),
  .EMA     (3'h2),
  .TEN     (1'b1),
  .BEN     (1'b1),
  .TCEN    (1'b1),
  .TA      (10'b0),
  .TQ      (32'b0),
  .PGEN    (1'b0),
  .KEN     (1'b1)
);
`else
sirv_mrom # (
 .AW(AW),
 .DW(DW),
 .DP(DP)
)u_sirv_mrom (
  .rom_addr (rom_icb_cmd_addr[AW-1:2]),
  .rom_dout (rom_dout) 
);
`endif
endmodule

