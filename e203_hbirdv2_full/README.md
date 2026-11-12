# E203 CPU依赖摘录

历史来源提交见../来源说明.json；许可证见LICENSE。目录名为历史名称，不表示完整SoC仓库。

仅保留本项目kwt_e203_soc_system实例化的e203_cpu_top及其文件级依赖和两个配置头。删除上游外设、JTAG顶层、旧SoC/子系统和NICE实现；config.v已关闭E203_CFG_HAS_NICE。general多模块公共文件保留为原文件，未拆分CPU仍使用的FIFO、触发器和RAM实现。

仿真与DC通过SoC总线集成/tools/e203_systembus_sources.py生成构建副本。TCM当前仍使用上游sirv_sim_ram行为描述，不能据此宣称TCM已映射到真实宏。
