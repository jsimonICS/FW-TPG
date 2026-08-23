/* TPG -- 15.734 kHz RGB + composite-sync test-pattern generator, driven from
 * the FreeWili OG main CPU's breakout header. UNVERIFIED ON HARDWARE -- see
 * video_timing.h and the design discussion this was built from. Needs a
 * scope (and ideally a real 15 kHz arcade monitor) to confirm the timing,
 * pin polarity, and field-wrap boundary are actually clean.
 *
 * Architecture: two PIO state machines (tpg_video.pio) drive two disjoint
 * breakout-header pin groups in lockstep -- GPIO 8-15 (8 color bits) and
 * GPIO 26-27 (the 9th color bit + CSYNC) -- each fed by its own one-shot DMA
 * channel from a precomputed one-field buffer (video_pattern.c). A DMA
 * completion IRQ re-arms both channels every field (~16.7 ms); that is a
 * generous window for a "rearm and re-trigger" handler, nothing like the
 * per-sample latency budget audio/video-per-pixel DMA would need. */
#include "fwog_main.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "pico/assert.h"
#include "video_timing.h"
#include "video_pattern.h"
#include "tpg_video.pio.h"

FWOG_WATCHDOG_DEFAULT();

static uint32_t s_bus8_words[TPG_FIELD_WORDS];
static uint32_t s_bus2_words[TPG_FIELD_WORDS];

static uint s_bus8_dma, s_bus2_dma;
static PIO const s_pio = pio0;
static uint s_sm8, s_sm2;

/* Re-arms one channel for another full-field, one-shot transfer. Called
 * once per field (~16.7 ms) from the DMA IRQ -- not remotely timing
 * critical, unlike a per-line or per-pixel rearm would be. */
static void rearm(uint channel, volatile void *fifo, const uint32_t *buf, uint dreq) {
    dma_channel_config c = dma_channel_get_default_config(channel);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, dreq);
    dma_channel_configure(channel, &c, fifo, buf, TPG_FIELD_WORDS, true);
}

static void dma_irq0_handler(void) {
    if (dma_channel_get_irq0_status(s_bus8_dma)) {
        dma_channel_acknowledge_irq0(s_bus8_dma);
        rearm(s_bus8_dma, &s_pio->txf[s_sm8], s_bus8_words, pio_get_dreq(s_pio, s_sm8, true));
    }
    if (dma_channel_get_irq0_status(s_bus2_dma)) {
        dma_channel_acknowledge_irq0(s_bus2_dma);
        rearm(s_bus2_dma, &s_pio->txf[s_sm2], s_bus2_words, pio_get_dreq(s_pio, s_sm2, true));
    }
}

/* Marks the ten breakout pins we use as outputs on all three surfaces the
 * direction has to move on at once (FPGA io_buffer, PCAL6416 expander, the
 * RP2040's own pads) -- see gpio/breakout.h. Requires the display CPU to be
 * up and servicing the link, which fwog_display_update_run() (below, in
 * main()) already established before this runs. Called exactly once: after
 * this, main never touches the FPGA/expander direction path again, so pins
 * 12-15 (which double as the FPGA's own SPI bus during this call) are safe
 * to hand to PIO afterward and keep indefinitely -- see ice40.c's
 * spi_claim()/spi_release(), which only fire on an explicit call like this
 * one, never in the background. */
static bool claim_breakout_pins_as_outputs(void) {
    fwog_io_cfg_t cfg;
    fwog_io_cfg_default(&cfg);
    cfg.uart_tx_out = true;   /* GPIO 8  */
    cfg.uart_rx_out = true;   /* GPIO 9  */
    cfg.uart_cts_out = true;  /* GPIO 10 */
    cfg.uart_rts_out = true;  /* GPIO 11 */
    cfg.spi_rx_out = true;    /* GPIO 12 */
    cfg.spi_cs_out = true;    /* GPIO 13 */
    cfg.spi_sclk_out = true;  /* GPIO 14 */
    cfg.spi_tx_out = true;    /* GPIO 15 */
    cfg.gpio26_out = true;    /* GPIO 26 */
    cfg.gpio27_out = true;    /* GPIO 27 */
    cfg.gpio25_out = true;    /* PIN_LED; fwog_io_dir_apply() rejects false */

    fwog_io_result_t r = fwog_io_dir_apply(&cfg);
    if (r != FWOG_IO_OK) {
        DIAG("[tpg] fwog_io_dir_apply failed: %d\n", (int)r);
        return false;
    }
    return true;
}

static void init_pio_bus(PIO pio, uint sm, uint offset,
                          uint base_pin, uint pin_count,
                          pio_sm_config (*get_cfg)(uint)) {
    for (uint p = base_pin; p < base_pin + pin_count; p++) {
        pio_gpio_init(pio, p);
    }
    pio_sm_set_consecutive_pindirs(pio, sm, base_pin, pin_count, true);

    pio_sm_config c = get_cfg(offset);
    sm_config_set_out_pins(&c, base_pin, pin_count);
    sm_config_set_out_shift(&c, true /* shift right */, true /* autopull */, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio, sm, offset, &c);
}

int main(void) {
    board_init();
    fwog_display_result_t d = fwog_display_update_run();

    bool io_ok = claim_breakout_pins_as_outputs();

    uint32_t sys_hz = clock_get_hz(clk_sys);
    tpg_pattern_build(s_bus8_words, s_bus2_words, sys_hz);

    s_sm8 = pio_claim_unused_sm(s_pio, true);
    s_sm2 = pio_claim_unused_sm(s_pio, true);
    int off8 = pio_add_program(s_pio, &tpg_bus8_program);
    int off2 = pio_add_program(s_pio, &tpg_bus2_program);
    /* Each program is 3 instructions; the RP2040 PIO instruction memory is
     * 32 words, shared by whatever else is loaded on this PIO block (nothing,
     * today) -- a negative offset here would mean this BSP grew a lot more
     * PIO users on pio0 than exist at the time this was written. */
    hard_assert(off8 >= 0 && off2 >= 0);

    init_pio_bus(s_pio, s_sm8, (uint)off8,
                 TPG_BUS8_BASE_PIN, TPG_BUS8_PIN_COUNT, tpg_bus8_program_get_default_config);
    init_pio_bus(s_pio, s_sm2, (uint)off2,
                 TPG_BUS2_BASE_PIN, TPG_BUS2_PIN_COUNT, tpg_bus2_program_get_default_config);

    s_bus8_dma = dma_claim_unused_channel(true);
    s_bus2_dma = dma_claim_unused_channel(true);

    irq_add_shared_handler(DMA_IRQ_0, dma_irq0_handler,
                            PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    dma_channel_set_irq0_enabled(s_bus8_dma, true);
    dma_channel_set_irq0_enabled(s_bus2_dma, true);
    irq_set_enabled(DMA_IRQ_0, true);

    /* Prime both FIFOs before the SMs start consuming, then start both SMs
     * on the same cycle so bus8/bus2 begin their first `out pins` in lock
     * step. */
    rearm(s_bus8_dma, &s_pio->txf[s_sm8], s_bus8_words, pio_get_dreq(s_pio, s_sm8, true));
    rearm(s_bus2_dma, &s_pio->txf[s_sm2], s_bus2_words, pio_get_dreq(s_pio, s_sm2, true));
    pio_enable_sm_mask_in_sync(s_pio, (1u << s_sm8) | (1u << s_sm2));

    const absolute_time_t announce_until = make_timeout_time_ms(10000);
    while (true) {
        board_watchdog_kick();
        if (!time_reached(announce_until)) {
            DIAG("[tpg_main] display: %s io_dir: %s\n",
                 fwog_display_result_text(d), io_ok ? "ok" : "FAILED");
        } else {
            DIAG("[tpg_main] alive\n");
        }
        sleep_ms(500);
    }
}
