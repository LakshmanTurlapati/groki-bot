// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include <lgfx/utility/result.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace lgfx {
std::uint32_t micros()
{
    static std::uint32_t time = 0;
    return time += 100;
}
std::uint32_t millis() { return micros() / 1000; }
} // namespace lgfx

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

enum class error_t { invalid_arg, mode_mismatch, connection_lost };
constexpr int LGFX_I2C_PORT_NUM = 1;
constexpr std::uint32_t I2C_ACK_ERR_INT_RAW_M = 1;
constexpr std::uint32_t I2C_TIME_OUT_INT_RAW_M = 2;
constexpr std::uint32_t I2C_END_DETECT_INT_RAW_M = 4;
constexpr std::uint32_t I2C_ARBITRATION_LOST_INT_RAW_M = 8;
constexpr std::uint32_t I2C_TRANS_COMPLETE_INT_RAW_M = 16;
constexpr int i2c_cmd_stop = 2, i2c_cmd_read = 3, i2c_cmd_end = 4;

struct InterruptRegister {
    union {
        std::uint32_t val;
        struct {
            std::uint32_t nack_int_raw : 1;
            std::uint32_t timeout : 1;
            std::uint32_t end_detect_int_raw : 1;
        };
    };
};

struct Device {
    InterruptRegister int_raw{};
    struct { std::uint32_t val = 0; } int_clr;
    struct { int trans_start = 0; } ctr;
    struct { int scl_high_period = 2, scl_wait_high_period = 2; } scl_high_period;
    struct { int scl_low_period = 2; } scl_low_period;
    struct { int scl_stop_setup_time = 2; } scl_stop_setup;
    struct { int scl_stop_hold_time = 2; } scl_stop_hold;
    struct { int txfifo_cnt = 0; } sr;
} device;

bool locked = false;
int mutex_depth = 0;
int task_a, task_b, mutex;
void* current_task = &task_a;
void* mutex_owner = nullptr;
int stops = 0, restores = 0, unlocks = 0;
int fifo_count = 0;
std::uint32_t fifo_value = 0x6d;

struct i2c_context_t {
    enum state_t { state_disconnect, state_write, state_read };
    cpp::result<state_t, error_t> state{state_disconnect};
    int wait_ack_stage = 0;
    void* mtx = &mutex;
    std::atomic<void*> owner{nullptr};
    void lock()
    {
        check(!locked || mutex_owner == current_task, "shared-bus mutex belongs to another task");
        locked = true;
        mutex_owner = current_task;
        ++mutex_depth;
    }
    void load_reg(Device*)
    {
        check(locked, "register restore ran after releasing the shared bus");
        ++restores;
    }
    void unlock()
    {
        check(locked, "I2C transaction released its mutex twice");
        check(mutex_owner == current_task, "I2C transaction released another task's mutex");
        check(mutex_depth > 0, "I2C transaction released an unowned recursive mutex");
        if (--mutex_depth == 0) {
            locked = false;
            mutex_owner = nullptr;
        }
        ++unlocks;
    }
} i2c_context[LGFX_I2C_PORT_NUM];

Device* getDev(int) { return &device; }
std::uint32_t* getFifoAddr(int) { return &fifo_value; }
int getRxFifoCount(Device*) { return fifo_count; }
int getFifoLen(int) { return 32; }
std::uint32_t getSourceClock(int) { return 40'000'000; }
bool getBusBusy(Device*) { return false; }
bool isSoftPort(int) { return false; }
cpp::result<void, error_t> soft_i2c_readBytes(int, std::uint8_t*, std::size_t, bool)
{
    return cpp::fail(error_t::invalid_arg);
}
cpp::result<void, error_t> soft_i2c_endTransaction(int) { return cpp::fail(error_t::invalid_arg); }
void i2c_stop(int)
{
    check(locked, "I2C STOP ran without bus ownership");
    ++stops;
}
void i2c_set_cmd(Device*, int, int, int, bool = false) {}
void updateDev(Device*) {}
void taskYIELD() {}
void delayMicroseconds(int) {}
void* xTaskGetCurrentTaskHandle() { return current_task; }
using lgfx::millis;

#define ESP_LOGV(...) ((void)0)
#define ESP_LOGW(...) ((void)0)

// CMake extracts these bodies from the checked-out M5GFX source, so this
// executes the patched production error paths rather than a copied model.
#include "m5gfx_i2c_wait.inc"
#include "m5gfx_i2c_read.inc"
#include "m5gfx_i2c_end.inc"

void begin_transaction(int available_bytes, bool nack = false)
{
    check(i2c_context[0].owner.load() == nullptr, "previous I2C failure retained active ownership");
    i2c_context[0].lock();
    i2c_context[0].owner.store(current_task);
    i2c_context[0].state = i2c_context_t::state_read;
    i2c_context[0].wait_ack_stage = 1;
    device.int_raw.val = nack ? I2C_ACK_ERR_INT_RAW_M : I2C_END_DETECT_INT_RAW_M;
    fifo_count = available_bytes;
}

void check_failed_read(bool nack)
{
    const int previous_stops = stops, previous_restores = restores, previous_unlocks = unlocks;
    begin_transaction(0, nack);
    std::uint8_t result = 0xaa;
    auto read = readBytes(0, &result, 1, true);
    check(read.has_error() && read.error() == error_t::connection_lost, "failed receive did not report connection loss");
    check(result == 0xaa, "failed receive changed the caller's output");
    check(!locked && stops == previous_stops + 1 && restores == previous_restores + 1 && unlocks == previous_unlocks + 1,
          "failed receive did not stop, restore registers and release the bus exactly once");
    check(i2c_context[0].state.has_error() && i2c_context[0].wait_ack_stage == 0,
          "failed receive left a pending ACK or lost the error state");
    check(i2c_context[0].owner.load() == nullptr && device.int_clr.val == 0x1ffff,
          "failed receive retained its owner or stale interrupt flags");

    // M5Unified's higher-level read helpers always call endTransaction,
    // including after a failed read has already relinquished bus ownership.
    auto end = endTransaction(0);
    check(end.has_error(), "ending a failed transaction lost its error");
    check(stops == previous_stops + 1 && restores == previous_restores + 1 && unlocks == previous_unlocks + 1,
          "ending a failed transaction cleaned up twice");
}

void check_recovery()
{
    const int previous_unlocks = unlocks;
    begin_transaction(1);
    std::uint8_t result = 0;
    auto read = readBytes(0, &result, 1, true);
    check(read.has_value() && result == fifo_value, "next I2C read did not recover");
    check(locked && unlocks == previous_unlocks, "successful read released bus before endTransaction");
    check(endTransaction(0).has_value(), "recovered transaction could not finish");
    check(!locked && unlocks == previous_unlocks + 1, "recovered transaction leaked its mutex");
}

void check_delayed_end_does_not_interrupt_new_owner()
{
    current_task = &task_a;
    begin_transaction(0);
    std::uint8_t result = 0;
    check(readBytes(0, &result, 1, true).has_error(), "expected initial read timeout");
    check(!locked, "failed reader retained bus ownership");

    current_task = &task_b;
    begin_transaction(1);
    const int previous_stops = stops, previous_restores = restores, previous_unlocks = unlocks;
    current_task = &task_a;
    check(endTransaction(0).has_error(), "old caller ended a new owner's transaction");
    check(locked && mutex_owner == &task_b && stops == previous_stops &&
          restores == previous_restores && unlocks == previous_unlocks,
          "old caller interfered with a new transaction after its failed read");

    current_task = &task_b;
    check(readBytes(0, &result, 1, true).has_value() && result == fifo_value,
          "new owner's read was disrupted");
    check(endTransaction(0).has_value() && !locked, "new owner could not finish its transaction");
    current_task = &task_a;
}

void check_recursive_transaction_failure()
{
    // writeRegister8 holds an outer recursive lock around its read/modify/write
    // sequence. A failed read must relinquish only the inner transaction.
    i2c_context[0].lock();
    begin_transaction(0);
    check(mutex_depth == 2, "read/modify/write did not hold both recursive locks");
    const int previous_unlocks = unlocks, previous_restores = restores;
    std::uint8_t result = 0;
    check(readBytes(0, &result, 1, true).has_error(), "nested read did not fail");
    check(locked && mutex_depth == 1 && mutex_owner == current_task &&
          i2c_context[0].owner.load() == nullptr && unlocks == previous_unlocks + 1 && restores == previous_restores + 1,
          "failed read did not release exactly the inner transaction lock");
    check(endTransaction(0).has_error() && mutex_depth == 1 && unlocks == previous_unlocks + 1,
          "late transaction cleanup released the outer lock");

    begin_transaction(1);
    check(readBytes(0, &result, 1, true).has_value(), "nested retry failed");
    check(endTransaction(0).has_value() && mutex_depth == 1, "nested retry did not preserve outer lock");
    i2c_context[0].unlock();
    check(!locked && mutex_depth == 0, "outer read/modify/write lock leaked");
}

} // namespace

int main()
{
    check_failed_read(false); // Address ACK succeeds, but no byte enters RX FIFO.
    check_recovery();
    check_failed_read(true);  // Address NACK follows the other cleanup branch.
    check_recovery();
    check_delayed_end_does_not_interrupt_new_owner();
    check_recursive_transaction_failure();
    std::puts("M5GFX I2C cleanup tests passed");
}
