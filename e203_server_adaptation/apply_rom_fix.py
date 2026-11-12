from pathlib import Path
def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('Unexpected teacher source near ' + repr(before))
    return text.replace(before, after)
def fix_bootrom(src):
    old = '  assign rom_icb_rsp_valid = rom_icb_cmd_valid;\n  assign rom_icb_cmd_ready = rom_icb_rsp_ready;\n  assign rom_icb_rsp_err = ~rom_icb_cmd_read;\n  assign rom_icb_rsp_rdata = rom_dout;'
    new = "  reg rsp_valid, rsp_err;\n  wire accepted = rom_icb_cmd_valid && rom_icb_cmd_ready;\n  assign rom_icb_cmd_ready = rst_n && (!rsp_valid || rom_icb_rsp_ready);\n  assign rom_icb_rsp_valid = rsp_valid;\n  assign rom_icb_rsp_err = rsp_err;\n  always @(posedge clk or negedge rst_n) begin\n    if (!rst_n) begin rsp_valid <= 1'b0; rsp_err <= 1'b0; end\n    else if (rom_icb_cmd_ready) begin\n      rsp_valid <= rom_icb_cmd_valid;\n      if (accepted) rsp_err <= !rom_icb_cmd_read;\n    end\n  end\n`ifdef FPGA\n  reg [DW-1:0] sampled_data;\n  always @(posedge clk) if (accepted && rom_icb_cmd_read) sampled_data <= rom_dout;\n  assign rom_icb_rsp_rdata = sampled_data;\n`else\n  assign rom_icb_rsp_rdata = rom_dout;\n`endif"
    text = replace_once(src.read_text(encoding='utf-8'), old, new)
    text = replace_once(text, '.CEN     (!rom_icb_cmd_read)', '.CEN     (!(accepted && rom_icb_cmd_read))')
    src.write_text(text, encoding='utf-8')

if __name__ == "__main__":
 import sys
 fix_bootrom(Path(sys.argv[1]))
