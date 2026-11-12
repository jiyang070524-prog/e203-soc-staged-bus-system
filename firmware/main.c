// 文件路径：SoC总线集成/firmware/main.c
// 用途：本科单任务 E203 裸机驱动（单口分阶段协议，2026-11-04）。
//
// 用户最终协议（禁止退回旧模式）：
//   1. CPU 经 sysper 控制块写 LOAD_REQ 任务 token；
//   2. TB 观察到这笔真实握手后，经已有 init 装载事务路径写 X/W/B/desc，
//      最后写共享内存 LOAD_DONE token 并释放端口；
//   3. CPU 对共享 mailbox 的读在 TB 装载阶段被 ready 反压，TB 完成后自然返回；
//   4. CPU 看到 LOAD_DONE 后写 AI_START；DMA 独占同一 SRAM 口；
//   5. CPU 等 AI_DONE（STATUS.DONE），不逐字读 Y、不做 FNV 哈希；
//   6. CPU 写 READOUT_REQ，TB 真实读走 12672 个 Y 字并对 golden，
//      写 READOUT_DONE token 并释放；CPU 确认下一任务。
//
// 约束：固件不内嵌任何 golden Y 或期望哈希；数值验证在 TB/主机侧。
//       共享 SRAM 只经真实 ICB 端口访问，不用层级后门。
#include <stdint.h>
#include "params.h"

#ifndef WS_MMIO_BASE
#define WS_MMIO_BASE    0x11000000u
#endif
#ifndef WS_Y_ADDR
#define WS_Y_ADDR       0xA005b000u
#endif
#ifndef WS_OUT_WORDS
#define WS_OUT_WORDS    12672
#endif
#ifndef WS_LOAD_DONE_ADDR
#define WS_LOAD_DONE_ADDR    0xA007e000u
#endif
#ifndef WS_READOUT_DONE_ADDR
#define WS_READOUT_DONE_ADDR 0xA007e004u
#endif
#ifndef WS_LOAD_MAGIC
#define WS_LOAD_MAGIC    0x4C4F4144u  // "LOAD"
#endif
#ifndef WS_READOUT_MAGIC
#define WS_READOUT_MAGIC 0x52454144u  // "READ"
#endif
#ifndef WS_MAX_SPIN
#define WS_MAX_SPIN     8000000u
#endif

#define MMIO_BASE        WS_MMIO_BASE
#define REG_START        0x04u   // W1S：写 1 启动固定描述符任务
#define REG_STATUS       0x08u   // bit0 BUSY, bit1 DONE, bit2 ERROR, bit3 EXPORT, bit4 LAST
#define REG_IRQ_STATUS   0x10u   // bit0 DONE, bit1 ERROR，sticky level
#define REG_IRQ_W1C      0x14u   // 写 1 清对应 sticky 位
#define REG_ERROR_CODE   0x18u
#define REG_VERSION      0x1cu
#define REG_TASK_CYCLES  0x20u
#define REG_DMA_RD_WORDS 0x24u
#define REG_DMA_WR_WORDS 0x28u
#define REG_TASKS_DONE   0x2cu
#define REG_LAYER_INDEX  0x30u
#define REG_CAPS         0x3cu
#define REG_LOAD_REQ     0x40u   // W：任务 token，触发 TB 装载
#define REG_LOAD_STATUS  0x44u   // bit0 REQ, bit1 DONE, bit2 GRANT
#define REG_READOUT_REQ  0x48u   // W：任务 token，触发 TB 取 Y
#define REG_RD_STATUS    0x4Cu   // bit0 REQ, bit1 DONE, bit2 GRANT
#define REG_CONFIRM      0x50u   // W：确认本任务，准备下一任务
#define REG_PROTO_STATUS 0x54u

#define STATUS_BUSY      0x1u
#define STATUS_DONE      0x2u
#define STATUS_ERROR     0x4u
#define STATUS_EXPORT    0x8u
#define STATUS_LAST      0x10u

// 阶段标志位：测试台据此核对真实分阶段发生，而非只看最终值。
#define FLAG_LOAD_REQ     0x01u
#define FLAG_LOAD_DONE    0x02u
#define FLAG_AI_START     0x04u
#define FLAG_AI_DONE      0x08u
#define FLAG_READOUT_REQ  0x10u
#define FLAG_READOUT_DONE 0x20u
#define FLAG_CONFIRM      0x40u
#define FLAG_DONE         0x80u

// DTCM 邮箱（私有内存，非共享 SRAM）：地址与既有 12 层固件一致，测试台只读不改。
#define DTCM_VERSION   ((volatile uint32_t *)0x90001fe0u)
#define DTCM_TASKCYC   ((volatile uint32_t *)0x90001fe4u)
#define DTCM_DMARD     ((volatile uint32_t *)0x90001fe8u)
#define DTCM_FLAGS     ((volatile uint32_t *)0x90001fecu)
#define DTCM_IRQ_COUNT ((volatile uint32_t *)0x90001ff0u)
#define DTCM_IRQ_LAST  ((volatile uint32_t *)0x90001ff4u)
#define DTCM_STATUS    ((volatile uint32_t *)0x90001ff8u)
#define DTCM_EXIT      ((volatile uint32_t *)0x90001ffcu)

static inline void mmio_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(MMIO_BASE + off) = val;
}
static inline uint32_t mmio_read(uint32_t off) {
    return *(volatile uint32_t *)(MMIO_BASE + off);
}
static inline uint32_t shm_read(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}
static inline void shm_write(uint32_t addr, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)addr = val;
}
static inline void fence_rw(void) {
    __asm__ volatile("fence rw, rw" ::: "memory");
}
static inline void wfi_once(void) {
    __asm__ volatile("wfi" ::: "memory");
}

static volatile uint32_t irq_count;
static volatile uint32_t irq_last;
static volatile uint32_t sw_flags;

__attribute__((interrupt("machine"), aligned(4)))
void irq_handler(void) {
    uint32_t st = mmio_read(REG_IRQ_STATUS);
    irq_last = st;
    mmio_write(REG_IRQ_W1C, st);
    irq_count = irq_count + 1u;
}

static void irq_setup(void) {
    uint32_t vec = (uint32_t)(uintptr_t)&irq_handler;
    __asm__ volatile("csrw mtvec, %0" :: "r"(vec) : "memory");
    __asm__ volatile("csrs mie, %0" :: "r"(1u << 11) : "memory");
}
static void irq_global_on(void)  { __asm__ volatile("csrs mstatus, %0" :: "r"(1u << 3) : "memory"); }
static void irq_global_off(void) { __asm__ volatile("csrci mstatus, 8" ::: "memory"); }

static int wait_status(uint32_t mask) {
    uint32_t spins = 0;
    for (;;) {
        irq_global_off();
        uint32_t st = mmio_read(REG_STATUS);
        if (st & mask) { irq_global_on(); return 0; }
        if (++spins > WS_MAX_SPIN) { irq_global_on(); return -1; }
        wfi_once();
        irq_global_on();
    }
}

// 共享 mailbox 阻塞等待：TB 装载/取数阶段 loader 独占 SRAM 口，CPU 的这次读
// 会被 ready 反压；TB 写 DONE token 并释放端口后自然返回。没有 load_mode 复位。
static int wait_magic(uint32_t addr, uint32_t magic) {
    uint32_t spins = 0;
    for (;;) {
        uint32_t v = shm_read(addr);
        if (v == magic) return 0;
        if (++spins > WS_MAX_SPIN) return -1;
    }
}

static int wait_irq_cleared(void) {
    uint32_t spins = 0;
    for (;;) {
        irq_global_off();
        if (mmio_read(REG_IRQ_STATUS) == 0u) { irq_global_on(); return 0; }
        if (++spins > WS_MAX_SPIN) { irq_global_on(); return -1; }
        wfi_once();
        irq_global_on();
    }
}

static void finish(int errors) {
    irq_global_off();
    *DTCM_VERSION = mmio_read(REG_VERSION);
    *DTCM_TASKCYC = mmio_read(REG_TASK_CYCLES);
    *DTCM_DMARD  = mmio_read(REG_DMA_RD_WORDS);
    *DTCM_FLAGS  = sw_flags;
    *DTCM_IRQ_COUNT = irq_count;
    *DTCM_IRQ_LAST  = irq_last;
    *DTCM_STATUS = errors ? (0xEEEE0000u | (uint32_t)(errors & 0xffff)) : 0x88888888u;
    *DTCM_EXIT   = errors ? 1u : 0u;
    for (;;) wfi_once();
}

int main(void) {
    uint32_t before;
    irq_setup();
    *DTCM_STATUS = 0u;
    *DTCM_EXIT = 0u;
    *DTCM_FLAGS = 0u;
    *DTCM_IRQ_COUNT = 0u;
    *DTCM_IRQ_LAST = 0u;

    // 先清共享 mailbox，避免读到上一任务/毒化残留的旧 DONE token。
    shm_write(WS_LOAD_DONE_ADDR, 0u);
    shm_write(WS_READOUT_DONE_ADDR, 0u);
    fence_rw();

    // CAPS bit0 = staged single-port protocol present.
    if ((mmio_read(REG_CAPS) & 1u) == 0u) finish(0x0D01);

    irq_global_on();
    before = irq_count;

    // ---- Stage 1: CPU requests load; TB observes this real handshake. ----
    mmio_write(REG_LOAD_REQ, 1u);
    fence_rw();
    sw_flags |= FLAG_LOAD_REQ;

    if (wait_magic(WS_LOAD_DONE_ADDR, WS_LOAD_MAGIC)) finish(0x0D02);
    sw_flags |= FLAG_LOAD_DONE;

    // ---- Stage 2: real AI_START, CPU waits for AI_DONE (no Y read). ----
    mmio_write(REG_START, 1u);
    fence_rw();
    sw_flags |= FLAG_AI_START;

    if (wait_status(STATUS_DONE | STATUS_ERROR)) finish(0x0D03);
    if (mmio_read(REG_STATUS) & STATUS_ERROR) finish(0x0D04);
    if ((mmio_read(REG_STATUS) & STATUS_DONE) == 0u) finish(0x0D05);
    sw_flags |= FLAG_AI_DONE;

    // ---- Stage 3: CPU requests readout; TB reads Y from real responses. ----
    mmio_write(REG_READOUT_REQ, 1u);
    fence_rw();
    sw_flags |= FLAG_READOUT_REQ;

    if (wait_magic(WS_READOUT_DONE_ADDR, WS_READOUT_MAGIC)) finish(0x0D06);
    sw_flags |= FLAG_READOUT_DONE;

    // ---- Stage 4: confirm task and check completion bookkeeping. ----
    mmio_write(REG_CONFIRM, 1u);
    fence_rw();
    sw_flags |= FLAG_CONFIRM;

    if (mmio_read(REG_TASKS_DONE) != 1u) finish(0x0D07);
    if (mmio_read(REG_DMA_RD_WORDS) == 0u) finish(0x0D08);
    if (mmio_read(REG_DMA_WR_WORDS) == 0u) finish(0x0D09);
    if (mmio_read(REG_TASK_CYCLES) == 0u) finish(0x0D0A);
    if (irq_count != before + 1u) finish(0x0D0B);
    if (wait_irq_cleared()) finish(0x0D0C);

    sw_flags |= FLAG_DONE;
    finish(0);
    return 0;
}
