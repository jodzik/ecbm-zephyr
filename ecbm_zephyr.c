#include "ecbm_zephyr.h"

#include <safe_c.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include <errno.h>

LOG_MODULE_REGISTER(ecbm_zephyr);

BUILD_ASSERT(0 == (CONFIG_ECBM_ZEPHYR_RX_RING_BUF_SIZE & (CONFIG_ECBM_ZEPHYR_RX_RING_BUF_SIZE - 1)),
    "CONFIG_ECBM_ZEPHYR_RX_RING_BUF_SIZE must be a power of two");

static uint64_t _get_time_ms(void* ctx);
static void _sleep_ms(uint32_t ms, void* ctx);
static int _read(uint8_t* buf, uint16_t buf_size, void* ctx);
static int _write(uint8_t const* data, uint16_t ndata, void* ctx);
static enum EcbmWriteStatus _get_write_status(void* ctx);
static void _uart_callback(struct device const* dev, struct uart_event* evt, void* user_data);
static void _owner_thread(void* arg1, void* arg2, void* arg3);

static uint64_t _get_time_ms(void* const ctx) {
    UNUSED(ctx);

    return (uint64_t)k_uptime_get();
}

static void _sleep_ms(uint32_t const ms, void* const ctx) {
    UNUSED(ctx);

    k_msleep(ms);
}

static int _read(uint8_t* const buf, uint16_t const buf_size, void* const ctx) {
    struct EcbmZephyr* const self = ctx;

    k_spinlock_key_t const key = k_spin_lock(&self->rx_lock);
    uint32_t const got = ring_buf_get(&self->rx_ring, buf, buf_size);
    k_spin_unlock(&self->rx_lock, key);

    return (int)got;
}

/// @brief The tx_state is set to PROCEEDED before uart_tx(), so the TX completion
/// event(ISR) cannot be missed. On a failed uart_tx() the state is left as is:
/// the core polls it only after an accepted write(), which always refreshes it.
static int _write(uint8_t const* const data, uint16_t const ndata, void* const ctx) {
    struct EcbmZephyr* const self = ctx;

    atomic_set(&self->tx_state, (int)ECBM_WRITE_STATUS__PROCEEDED);

    int const rc = uart_tx(self->uart, data, ndata, CONFIG_ECBM_ZEPHYR_TX_TIMEOUT_MS);
    if (0 == rc) {
        return ndata;
    }
    if (-EBUSY == rc) {
        return 0;
    }

    LOG_ERRf("Fail to start UART TX: %i", rc);
    return ER_IO;
}

static enum EcbmWriteStatus _get_write_status(void* const ctx) {
    struct EcbmZephyr* const self = ctx;

    int const state = atomic_get(&self->tx_state);
    switch ((enum EcbmWriteStatus)state) {
    case ECBM_WRITE_STATUS__COMPLETED:
    case ECBM_WRITE_STATUS__PROCEEDED:
    case ECBM_WRITE_STATUS__FAILED:
        return (enum EcbmWriteStatus)state;
    default:
        return ECBM_WRITE_STATUS__PROCEEDED;
    }
}

static void _uart_callback(struct device const* const dev, struct uart_event* const evt, void* const user_data) {
    struct EcbmZephyr* const self = user_data;

    switch (evt->type) {
    case UART_RX_RDY: {
        uint8_t const* const data = &evt->data.rx.buf[evt->data.rx.offset];
        k_spinlock_key_t const key = k_spin_lock(&self->rx_lock);
        uint32_t const put = ring_buf_put(&self->rx_ring, data, (uint32_t)evt->data.rx.len);
        k_spin_unlock(&self->rx_lock, key);
        if (put != evt->data.rx.len) {
            LOG_WRNf("RX ring full, dropped %u bytes", (uint32_t)evt->data.rx.len - put);
        }
        k_sem_give(&self->rx_sem);
        break;
    }
    case UART_RX_BUF_REQUEST:
        if (NULL != self->rx_next_buf) {
            int const rc = uart_rx_buf_rsp(dev, self->rx_next_buf, CONFIG_ECBM_ZEPHYR_RX_CHUNK_SIZE);
            if (0 == rc) {
                self->rx_next_buf = NULL;
            }
            else {
                LOG_ERRf("Fail to give the next RX buffer to the driver: %i", rc);
            }
        }
        else {
            LOG_WRN("No free RX buffer for the driver");
        }
        break;
    case UART_RX_BUF_RELEASED:
        if ((evt->data.rx_buf.buf == self->rx_uart_bufs[0]) || (evt->data.rx_buf.buf == self->rx_uart_bufs[1])) {
            self->rx_next_buf = evt->data.rx_buf.buf;
        }
        else {
            LOG_ERR("Released RX buffer is not owned by the instance");
        }
        break;
    case UART_TX_DONE:
        atomic_set(&self->tx_state, (int)ECBM_WRITE_STATUS__COMPLETED);
        break;
    case UART_TX_ABORTED:
        LOG_WRNf("UART TX aborted, sent %u bytes", (unsigned int)evt->data.tx.len);
        atomic_set(&self->tx_state, (int)ECBM_WRITE_STATUS__FAILED);
        break;
    case UART_RX_DISABLED:
        LOG_ERR("UART RX disabled, incoming traffic is lost");
        break;
    case UART_RX_STOPPED:
        LOG_ERR("UART RX stopped");
        break;
    default:
        break;
    }
}

static void _owner_thread(void* const arg1, void* const arg2, void* const arg3) {
    struct EcbmZephyr* const self = arg1;

    UNUSED(arg2);
    UNUSED(arg3);

    while (true) {
        k_sem_take(&self->rx_sem, K_FOREVER);

        int const rc = ecbm__poll(&self->ecbm);
        if (0 != rc) {
            LOG_WRNf("Fail to poll: %i", rc);
        }
    }
}

int ecbm_zephyr__init(
    struct EcbmZephyr* const self, struct EcbmZephyrConfig const* const config, EcbmPubHandler const pub_handler,
    void* const pub_user_data) {
    int rc = 0;

    ASSERT(NULL != self, ER_INVAL);
    ASSERT(NULL != config, ER_INVAL);
    ASSERT(NULL != config->uart, ER_INVAL);
    ASSERT(device_is_ready(config->uart), ER_NO_DEV);

    self->uart = config->uart;
    self->uart_rx_timeout_us = config->uart_rx_timeout_us;
    self->rx_next_buf = self->rx_uart_bufs[1];

    ring_buf_init(&self->rx_ring, sizeof(self->rx_ring_mem), self->rx_ring_mem);
    k_sem_init(&self->rx_sem, 0, 1);
    k_mutex_init(&self->api_lock);
    atomic_set(&self->tx_state, (int)ECBM_WRITE_STATUS__COMPLETED);

    TRY(ecbm__init(&self->ecbm, _get_time_ms, _read, _write, _get_write_status, _sleep_ms, pub_handler,
        pub_user_data, self));

    TRY(uart_callback_set(self->uart, _uart_callback, self));
    TRY(uart_rx_enable(self->uart, self->rx_uart_bufs[0], CONFIG_ECBM_ZEPHYR_RX_CHUNK_SIZE,
        self->uart_rx_timeout_us));

    self->tid = k_thread_create(&self->thread, self->thread_stack, K_KERNEL_STACK_SIZEOF(self->thread_stack),
        _owner_thread, self, NULL, NULL, CONFIG_ECBM_ZEPHYR_THREAD_PRIORITY, 0, K_FOREVER);
    ASSERT(NULL != self->tid, ER_1);

    if (NULL != config->thread_name) {
        TRYs_PASS(k_thread_name_set(self->tid, config->thread_name));
    }
    else {
        TRYs_PASS(k_thread_name_set(self->tid, "ecbm"));
    }

    k_thread_start(self->tid);

    LOG_INF("Init.");

 finally:

    return rc;
}

int ecbm_zephyr__read(struct EcbmZephyr* const self, EcbmAddr const addr, EcbmDataId const data_id,
    uint8_t* const buf, uint16_t const buf_size, uint16_t* const data_size, EcbmTimeoutMs const timeout_ms,
    uint8_t const retries) {
    int rc = 0;

    ASSERT(NULL != self, ER_INVAL);

    TRY(k_mutex_lock(&self->api_lock, K_FOREVER));
    // Not TRY: a positive rc is the slave error code result, not a local failure.
    rc = ecbm__read(&self->ecbm, addr, data_id, buf, buf_size, data_size, timeout_ms, retries);
    k_mutex_unlock(&self->api_lock);

 finally:

    return rc;
}

int ecbm_zephyr__write(struct EcbmZephyr* const self, EcbmAddr const addr, EcbmDataId const data_id,
    uint8_t const* const data, uint16_t const data_size, EcbmTimeoutMs const timeout_ms, uint8_t const retries) {
    int rc = 0;

    ASSERT(NULL != self, ER_INVAL);

    TRY(k_mutex_lock(&self->api_lock, K_FOREVER));
    // Not TRY: a positive rc is the slave error code result, not a local failure.
    rc = ecbm__write(&self->ecbm, addr, data_id, data, data_size, timeout_ms, retries);
    k_mutex_unlock(&self->api_lock);

 finally:

    return rc;
}

int ecbm_zephyr__write_no_answer(struct EcbmZephyr* const self, EcbmAddr const addr, EcbmDataId const data_id,
    uint8_t const* const data, uint16_t const data_size) {
    int rc = 0;

    ASSERT(NULL != self, ER_INVAL);

    TRY(k_mutex_lock(&self->api_lock, K_FOREVER));
    TRY(ecbm__write_no_answer(&self->ecbm, addr, data_id, data, data_size));
    k_mutex_unlock(&self->api_lock);

 finally:

    return rc;
}
