// uc386-dos MicroPython HAL header.
//
// `mp_hal_stdin_rx_chr` / `mp_hal_stdout_tx_strn` are out-of-line in
// `upstream/ports/minimal/uart_core.c`. The timing-related HAL
// functions (mp_hal_ticks_ms, mp_hal_delay_ms, etc.) are out-of-line
// in `uc386-dos/mphal_uc386dos.c`, so this header just defers to the
// `extern` declarations py/mphal.h ships when no override is present.

static inline void mp_hal_set_interrupt_char(char c) {
    (void)c;
}
