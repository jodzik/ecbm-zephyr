#ifndef ECBM_ZEPHYR_H_
#define ECBM_ZEPHYR_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <ecbm.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

/** @brief ECB master on a Zephyr UART(async API), see ecbm.h for the protocol API.
 *
 * One instance owns one UART: init spawns the RX owner thread, which drains the
 * UART RX events into the core, all the ecbm_zephyr__read()/ecbm_zephyr__write()/
 * ecbm_zephyr__write_no_answer() calls are serialized internally and may be called
 * from any thread context. The #EcbmPubHandler is called from the owner thread and
 * must be fast and must not call this API.
 */
typedef struct EcbmZephyr {
    // Core, kept first: the advanced user may reach it directly, but never from
    // two contexts at once and never bypassing the api_lock serialization.

    struct Ecbm ecbm;

    // Transport state, self->uart and rx_next_buf are immutable after init.

    struct device const *uart;
    uint32_t uart_rx_timeout_us;
    uint8_t *rx_next_buf; // RX double buffer chunk not owned by the UART driver

    // RX path: UART events(ISR) produce into the ring, the owner thread consumes.

    struct k_spinlock rx_lock;
    struct ring_buf rx_ring;
    uint8_t rx_ring_mem[CONFIG_ECBM_ZEPHYR_RX_RING_BUF_SIZE];
    uint8_t rx_uart_bufs[2][CONFIG_ECBM_ZEPHYR_RX_CHUNK_SIZE];
    struct k_sem rx_sem;

    // TX path: serialized by api_lock, completion state is set from the ISR.

    atomic_t tx_state;
    struct k_mutex api_lock;

    // Owner thread.

    K_KERNEL_STACK_MEMBER(thread_stack, CONFIG_ECBM_ZEPHYR_THREAD_STACK_SIZE);
    struct k_thread thread;
    k_tid_t tid;
} EcbmZephyr;

typedef struct EcbmZephyrConfig {
    struct device const *uart; // UART device with the async API support
    uint32_t uart_rx_timeout_us; // inactivity timeout for the #UART_RX_RDY event, us
    char const *thread_name; // optional(may be NULL) owner thread name
} EcbmZephyrConfig;

/** @brief Init the master, start the UART RX and spawn the owner thread.
 *
 * @param[in] pub_handler optional(may be NULL) handler for the slaves PUB_DATA
 *                  packets, called in the owner thread context, if NULL such
 *                  packets are dropped.
 * @param[in] pub_user_data opaque pointer for the pub_handler, may be NULL.
 *
 * @return 0 or #ErrorCodes: #ER_INVAL, #ER_NO_DEV, UART errors.
 */
int ecbm_zephyr__init(
    struct EcbmZephyr *self,
    struct EcbmZephyrConfig const *config,
    EcbmPubHandler pub_handler,
    void *pub_user_data);

/** @brief See ecbm__read(), blocking. Thread safe, calls from different threads
 * are serialized.
 */
int ecbm_zephyr__read(
    struct EcbmZephyr *self,
    EcbmAddr addr,
    EcbmDataId data_id,
    uint8_t *buf,
    uint16_t buf_size,
    uint16_t *data_size,
    EcbmTimeoutMs timeout_ms,
    uint8_t retries);

/** @brief See ecbm__write(), blocking. Thread safe, calls from different threads
 * are serialized.
 */
int ecbm_zephyr__write(
    struct EcbmZephyr *self,
    EcbmAddr addr,
    EcbmDataId data_id,
    uint8_t const *data,
    uint16_t data_size,
    EcbmTimeoutMs timeout_ms,
    uint8_t retries);

/** @brief See ecbm__write_no_answer(). Thread safe, calls from different threads
 * are serialized.
 */
int ecbm_zephyr__write_no_answer(
    struct EcbmZephyr *self,
    EcbmAddr addr,
    EcbmDataId data_id,
    uint8_t const *data,
    uint16_t data_size);

#ifdef __cplusplus
}
#endif

#endif // ECBM_ZEPHYR_H_
