/*
 * Copyright (c) 2013-2022 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * From CMSIS-DAP, SW_DP.c's SWD_Transfer and DAP.c's DAP_SWD_TransferBlock. Changed by the
 * Escapement Project: the data phase rewritten, the block loop inlined around the transfer.
 */
/** ARM's SWD transfer, with the data phase written to cost less per bit.
 *
 * ARM's loop spends four operations on each data bit where two will do: it accumulates a running
 * parity, shifts the word down and ors the new bit into the top. The parity is only ever used one
 * bit wide at the end, so it can be taken from the finished word instead of carried; and with the
 * loop unrolled the bit can go straight to its own position, which the barrel shifter folds into
 * the or. Measured on the emulator at the delay-free setting: a read bit goes from 7 cycles to 5
 * and a write bit from 5 to 4, against 3 for a packet bit, which is the floor.
 *
 * Everything else is ARM's, transcribed: the packet phase, the turnaround lengths, the
 * acknowledge handling, the WAIT and FAULT data phases, the protocol-error back-off, the
 * timestamp and the idle cycles. SW_DP.c still supplies SWJ_Sequence and SWD_Sequence, and still
 * compiles unchanged -- CMakeLists.txt renames only its SWD_Transfer out of the way.
 *
 * **The wire must not move.** tools/equiv.py drives this build and a build of ARM's against the
 * same 528 cases and requires identical wire events and identical replies from both. Run it after
 * touching anything here; the point of this file is to be faster, not different.
 */
#include "DAP_config.h"

#include "DAP.h"
#include "hot.h"
#include "swd_dp.h"
#include "swd_transfer.h"

#ifdef SWD_CODEGEN
#include "swd_gen.h"
#else

// The same shape as SW_DP.c's, so the two can be read side by side.
#define PIN_SWCLK_SET PIN_SWCLK_TCK_SET
#define PIN_SWCLK_CLR PIN_SWCLK_TCK_CLR

#define SW_CLOCK_CYCLE() \
    PIN_SWCLK_CLR();     \
    PIN_DELAY();         \
    PIN_SWCLK_SET();     \
    PIN_DELAY()

#define SW_WRITE_BIT(bit) \
    PIN_SWDIO_OUT(bit);   \
    PIN_SWCLK_CLR();      \
    PIN_DELAY();          \
    PIN_SWCLK_SET();      \
    PIN_DELAY()

#define SW_READ_BIT(bit)  \
    PIN_SWCLK_CLR();      \
    PIN_DELAY();          \
    bit = PIN_SWDIO_IN(); \
    PIN_SWCLK_SET();      \
    PIN_DELAY()

#define PIN_DELAY() PIN_DELAY_SLOW(clock_delay)

/** Shift one data bit to its own position in the word, so the or folds the shift in. */
#define SW_READ_DATA_BIT(n) \
    SW_READ_BIT(bit);       \
    val |= bit << (n)

#define SW_WRITE_DATA_BIT(n) SW_WRITE_BIT(val >> (n))

// 0..31, so the two data phases are straight-line code with constant shifts.
#define SW_DATA_BITS(macro) \
    macro(0);               \
    macro(1);               \
    macro(2);               \
    macro(3);               \
    macro(4);               \
    macro(5);               \
    macro(6);               \
    macro(7);               \
    macro(8);               \
    macro(9);               \
    macro(10);              \
    macro(11);              \
    macro(12);              \
    macro(13);              \
    macro(14);              \
    macro(15);              \
    macro(16);              \
    macro(17);              \
    macro(18);              \
    macro(19);              \
    macro(20);              \
    macro(21);              \
    macro(22);              \
    macro(23);              \
    macro(24);              \
    macro(25);              \
    macro(26);              \
    macro(27);              \
    macro(28);              \
    macro(29);              \
    macro(30);              \
    macro(31)

/** Put the packet parity in bit 6, which CMSIS-DAP does not use.
 *
 * ARM sums the four address bits inside the transfer, so a block recomputed the same parity for
 * every word. The caller knows the request before the loop; the transfer then writes the bit with
 * one more shift of a register it is already holding.
 */
__STATIC_FORCEINLINE uint32_t SWD_WithParity(uint32_t request) {
    uint32_t bits = request & 0x0FU;
    bits ^= bits >> 2;
    bits ^= bits >> 1;
    return request | ((bits & 1U) << 6);
}

/** The turnaround period, with its range stated so the compiler stops testing for the rest.
 *
 * `DAP_SWD_Configure` stores `(value & 3) + 1` and `DAP_Setup` stores 1, so the field is 1 to 4
 * and nothing else can write it. Read as a plain `uint8_t` the compiler has to allow 0 to 255,
 * and it spends a zero test and a three-way dispatch per turnaround discovering what it already
 * is. **Anything that writes this field outside those two places breaks the assumption here**,
 * which is the one way this goes wrong.
 */
__STATIC_FORCEINLINE uint32_t SWD_Turnaround(void) {
    const uint32_t cycles = DAP_Data.swd_conf.turnaround;
    if ((cycles - 1U) > 3U) {
        __builtin_unreachable();
    }
    return cycles;
}

#define SWD_TransferFunction(speed)                                                                \
    __STATIC_FORCEINLINE uint8_t SWD_Transfer##speed(uint32_t request, uint32_t* data) {           \
        uint32_t ack;                                                                              \
        uint32_t bit;                                                                              \
        uint32_t val;                                                                              \
        uint32_t n;                                                                                \
        /* Read once. The paced copy's PIN_DELAY expands to this, and reading DAP_Data each time  \
         * is two loads a bit: GCC hoists it into a register by itself, clang does not and the    \
         * bit costs two cycles more. Naming it is what makes the two toolchains agree. */        \
        const uint32_t clock_delay = DAP_Data.clock_delay;                                        \
        (void)clock_delay;                                                                        \
        const uint32_t primask = SWD_MASK_BEGIN();                                                 \
        (void)primask;                                                                             \
                                                                                                   \
        /* Packet request. The parity is in bit 6, put there by the caller. */                     \
        SW_WRITE_BIT(1U);           /* Start */                                                    \
        SW_WRITE_BIT(request >> 0); /* APnDP */                                                    \
        SW_WRITE_BIT(request >> 1); /* RnW */                                                      \
        SW_WRITE_BIT(request >> 2); /* A2 */                                                       \
        SW_WRITE_BIT(request >> 3); /* A3 */                                                       \
        SW_WRITE_BIT(request >> 6); /* Parity */                                                   \
        SW_WRITE_BIT(0U);           /* Stop */                                                     \
        SW_WRITE_BIT(1U);           /* Park */                                                     \
                                                                                                   \
        /* Turnaround */                                                                           \
        PIN_SWDIO_OUT_DISABLE();                                                                   \
        for (n = SWD_Turnaround(); n; n--) {                                           \
            SW_CLOCK_CYCLE();                                                                      \
        }                                                                                          \
                                                                                                   \
        /* Acknowledge */                                                                          \
        SW_READ_BIT(bit);                                                                          \
        ack = bit << 0;                                                                            \
        SW_READ_BIT(bit);                                                                          \
        ack |= bit << 1;                                                                           \
        SW_READ_BIT(bit);                                                                          \
        ack |= bit << 2;                                                                           \
                                                                                                   \
        if (ack == DAP_TRANSFER_OK) {                                                              \
            if (request & DAP_TRANSFER_RnW) {                                                      \
                /* Read data. The parity comes off the finished word rather than a running sum. */ \
                val = 0U;                                                                          \
                SW_DATA_BITS(SW_READ_DATA_BIT);                                                    \
                SW_READ_BIT(bit); /* Parity */                                                     \
                if ((__builtin_parity(val) ^ bit) & 1U) {                                          \
                    ack = DAP_TRANSFER_ERROR;                                                      \
                }                                                                                  \
                if (data) {                                                                        \
                    *data = val;                                                                   \
                }                                                                                  \
                /* Turnaround */                                                                   \
                for (n = SWD_Turnaround(); n; n--) {                                   \
                    SW_CLOCK_CYCLE();                                                              \
                }                                                                                  \
                PIN_SWDIO_OUT_ENABLE();                                                            \
            } else {                                                                               \
                /* Turnaround */                                                                   \
                for (n = SWD_Turnaround(); n; n--) {                                   \
                    SW_CLOCK_CYCLE();                                                              \
                }                                                                                  \
                PIN_SWDIO_OUT_ENABLE();                                                            \
                /* Write data */                                                                   \
                val = *data;                                                                       \
                SW_DATA_BITS(SW_WRITE_DATA_BIT);                                                   \
                SW_WRITE_BIT(__builtin_parity(val)); /* Parity */                                  \
            }                                                                                      \
            /* Capture timestamp */                                                                \
            if (request & DAP_TRANSFER_TIMESTAMP) {                                                \
                DAP_Data.timestamp = TIMESTAMP_GET();                                              \
            }                                                                                      \
            /* Idle cycles */                                                                      \
            n = DAP_Data.transfer.idle_cycles;                                                     \
            if (n) {                                                                               \
                PIN_SWDIO_OUT(0U);                                                                 \
                for (; n; n--) {                                                                   \
                    SW_CLOCK_CYCLE();                                                              \
                }                                                                                  \
            }                                                                                      \
            PIN_SWDIO_OUT(1U);                                                                     \
            SWD_MASK_END(primask);                                                                 \
            return ((uint8_t)ack);                                                                 \
        }                                                                                          \
                                                                                                   \
        if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {                           \
            if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) != 0U)) {            \
                for (n = 32U + 1U; n; n--) {                                                       \
                    SW_CLOCK_CYCLE(); /* Dummy read */                                             \
                }                                                                                  \
            }                                                                                      \
            /* Turnaround */                                                                       \
            for (n = SWD_Turnaround(); n; n--) {                                       \
                SW_CLOCK_CYCLE();                                                                  \
            }                                                                                      \
            PIN_SWDIO_OUT_ENABLE();                                                                \
            if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) == 0U)) {            \
                PIN_SWDIO_OUT(0U);                                                                 \
                for (n = 32U + 1U; n; n--) {                                                       \
                    SW_CLOCK_CYCLE(); /* Dummy write */                                            \
                }                                                                                  \
            }                                                                                      \
            PIN_SWDIO_OUT(1U);                                                                     \
            SWD_MASK_END(primask);                                                                 \
            return ((uint8_t)ack);                                                                 \
        }                                                                                          \
                                                                                                   \
        /* Protocol error: back off the data phase */                                              \
        for (n = SWD_Turnaround() + 32U + 1U; n; n--) {                                \
            SW_CLOCK_CYCLE();                                                                      \
        }                                                                                          \
        PIN_SWDIO_OUT_ENABLE();                                                                    \
        PIN_SWDIO_OUT(1U);                                                                         \
        SWD_MASK_END(primask);                                                                     \
        return ((uint8_t)ack);                                                                     \
    }

/* No interrupt can make a sample taken before its rising edge wrong: arriving first, it only
 * delays the sample, and the bit stays readable until well after the edge. */
#define SWD_MASK_BEGIN() 0U
#define SWD_MASK_END(primask) ((void)(primask))

#undef PIN_DELAY
#define PIN_DELAY() PIN_DELAY_FAST()
SWD_TransferFunction(Fast)

#undef PIN_DELAY
#define PIN_DELAY() PIN_DELAY_SLOW(clock_delay)

#ifdef SWD_SAMPLE_AFTER_EDGE
/* Sample just after the rising edge rather than just before it. The bit the target launched on
 * the previous edge stays readable until the new one has made the round trip through the buffers
 * and the target, a few cycles after this store, while sampling before the edge leaves only what
 * the period has to spare beyond that trip: under a cycle at the 16.67 MHz setting. vendor 0x87
 * measured both edges of the window; BENCH.md has it.
 *
 * **The sample must follow the store with nothing in between.** An interrupt there makes the
 * sample late enough to read the next bit, so every transfer on this path runs masked. Before the
 * edge an interrupt only delays a sample that has time to spare, which is why the delay-free path,
 * still sampling there, needs no mask. */
#undef SW_READ_BIT
#define SW_READ_BIT(bit)  \
    PIN_SWCLK_CLR();      \
    PIN_DELAY();          \
    PIN_SWCLK_SET();      \
    bit = PIN_SWDIO_IN(); \
    PIN_DELAY()
#undef SWD_MASK_BEGIN
#undef SWD_MASK_END
#define SWD_MASK_BEGIN() SWD_MaskBegin()
#define SWD_MASK_END(primask) __set_PRIMASK(primask)
__STATIC_FORCEINLINE uint32_t SWD_MaskBegin(void) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}
#endif
    SWD_TransferFunction(Slow)

        uint8_t SWD_Transfer(uint32_t request, uint32_t* data) {
    request = SWD_WithParity(request);
    if (DAP_Data.fast_clock) {
        return SWD_TransferFast(request, data);
    } else {
        return SWD_TransferSlow(request, data);
    }
}

/** DAP_Transfer, expanded once per clock path.
 *
 * ARM's DAP_SWD_Transfer calls SWD_Transfer once per transfer, and SWD_Transfer picks between the
 * two copies on every call. Expanding the command next to each copy lets the transfer inline into
 * it, so a transfer's register prologue is paid once per command rather than once per transfer.
 * The block command is arranged the same way below.
 *
 * This is not the throughput path -- a host reads memory in blocks -- but it is most of what an
 * attach is made of, and of anything touching one register at a time.
 */
#define SWD_XFER_FAST(request, data) SWD_TransferFast(SWD_WithParity(request), data)
#define SWD_XFER_SLOW(request, data) SWD_TransferSlow(SWD_WithParity(request), data)

// clang-format off: each of these expands to a function definition.
// The attribute goes on a declaration because the definitions come out of a macro, which is
// generated from ARM's DAP.c and should stay a faithful copy of it.
static uint32_t SWD_TransferCommandFast(const uint8_t* request, uint8_t* response) HOT_PATH;
static uint32_t SWD_TransferCommandSlow(const uint8_t* request, uint8_t* response) HOT_PATH;
SWD_TRANSFER_FUNCTION(SWD_TransferCommandFast, SWD_XFER_FAST)
SWD_TRANSFER_FUNCTION(SWD_TransferCommandSlow, SWD_XFER_SLOW)
// clang-format on

COMMAND_PATH uint32_t SWD_TransferCommand(const uint8_t* request, uint8_t* response) {
    if (DAP_Data.fast_clock) {
        return SWD_TransferCommandFast(request, response);
    }
    return SWD_TransferCommandSlow(request, response);
}

/** ARM's DAP_SWD_TransferBlock, with the transfer inlined into the loop.
 *
 * A transfer saves eight registers on entry and restores them on exit, and reloads the two GPIO
 * base addresses and the pin masks each time. Called once per word from DAP.c that is about 30
 * cycles of dead wire between one transfer and the next, which at the delay-free setting is a
 * tenth of a read. Inlining it into the loop pays for all of that once per block instead.
 *
 * The logic is ARM's, transcribed. The fast and slow choice is hoisted to the top so each copy
 * inlines one transfer variant rather than both.
 */
#define SWD_TransferBlockFunction(speed)                                                                  \
    /* The last read of a block is the only one that can use a different request, so leaving it    \
     * inside the loop makes the packet request loop-variant and GCC re-derives five shifted bits  \
     * and spills them on every word. Peeling it out fixes that. **It has to stay out of line.**   \
     * An inlined second transfer costs one register in the loop's data phase, and one register    \
     * there is one cycle on each of 32 data bits, which is more than the peel saves. That was     \
     * measured, not guessed: src/hot.h and BENCH.md have the four builds. */                      \
    static uint8_t SWD_TransferTail##speed(uint32_t request, uint32_t* data) HOT_PATH;                    \
    static uint8_t SWD_TransferTail##speed(uint32_t request, uint32_t* data) {                            \
        return SWD_Transfer##speed(request, data);                                                        \
    }                                                                                                     \
                                                                                                          \
    static uint32_t SWD_TransferBlock##speed(const uint8_t* request, uint8_t* response) {                 \
        uint32_t request_count;                                                                           \
        uint32_t request_value;                                                                           \
        uint32_t response_count = 0U;                                                                     \
        uint32_t response_value = 0U;                                                                     \
        uint8_t* response_head = response;                                                                \
        uint32_t retry;                                                                                   \
        uint32_t data = 0U;                                                                               \
                                                                                                          \
        response += 3;                                                                                    \
        DAP_TransferAbort = 0U;                                                                           \
        request++; /* Ignore DAP index */                                                                 \
                                                                                                          \
        request_count = (uint32_t)(*(request + 0) << 0) | (uint32_t)(*(request + 1) << 8);                \
        request += 2;                                                                                     \
        if (request_count == 0U) {                                                                        \
            goto end;                                                                                     \
        }                                                                                                 \
                                                                                                          \
        request_value = SWD_WithParity(*request++);                                                       \
        if ((request_value & DAP_TRANSFER_RnW) != 0U) {                                                   \
            if ((request_value & DAP_TRANSFER_APnDP) != 0U) {                                             \
                /* Post AP read */                                                                        \
                retry = DAP_Data.transfer.retry_count;                                                    \
                do {                                                                                      \
                    response_value = SWD_Transfer##speed(request_value, NULL);                            \
                } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);         \
                if (response_value != DAP_TRANSFER_OK) {                                                  \
                    goto read_done;                                                                       \
                }                                                                                         \
            }                                                                                             \
            while (--request_count) {                                                                     \
                retry = DAP_Data.transfer.retry_count;                                                    \
                do {                                                                                      \
                    response_value = SWD_Transfer##speed(request_value, &data);                           \
                } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);         \
                if (response_value != DAP_TRANSFER_OK) {                                                  \
                    goto read_done;                                                                       \
                }                                                                                         \
                /* ARM writes the four bytes separately. GCC merges them back into one store              \
                 * but rebuilds the word from its own bytes first, ten instructions to copy               \
                 * a register, so take the little-endian store directly. */                               \
                __builtin_memcpy(response, &data, sizeof(data));                                          \
                response += sizeof(data);                                                                 \
            }                                                                                             \
            /* A block posted to an access port reads its last word out of RDBUFF. */                     \
            if ((request_value & DAP_TRANSFER_APnDP) != 0U) {                                             \
                request_value = SWD_WithParity(DP_RDBUFF | DAP_TRANSFER_RnW);                             \
            }                                                                                             \
            retry = DAP_Data.transfer.retry_count;                                                        \
            do {                                                                                          \
                response_value = SWD_TransferTail##speed(request_value, &data);                           \
            } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);             \
            if (response_value == DAP_TRANSFER_OK) {                                                      \
                __builtin_memcpy(response, &data, sizeof(data));                                          \
                response += sizeof(data);                                                                 \
            }                                                                                             \
        read_done:                                                                                        \
            /* One word is four bytes, so the count the reply carries is the distance the                 \
             * response pointer moved. Keeping a separate counter cost a load, an add and a               \
             * store a word, because the unrolled data phase leaves it nowhere but the stack. */          \
            response_count = (uint32_t)(response - response_head - 3) / 4U;                               \
            goto end;                                                                                     \
        } else {                                                                                          \
            while (request_count--) {                                                                     \
                data = (uint32_t)(*(request + 0) << 0) | (uint32_t)(*(request + 1) << 8) |                \
                       (uint32_t)(*(request + 2) << 16) | (uint32_t)(*(request + 3) << 24);               \
                request += 4;                                                                             \
                retry = DAP_Data.transfer.retry_count;                                                    \
                do {                                                                                      \
                    response_value = SWD_Transfer##speed(request_value, &data);                           \
                } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);         \
                if (response_value != DAP_TRANSFER_OK) {                                                  \
                    goto end;                                                                             \
                }                                                                                         \
                response_count++;                                                                         \
            }                                                                                             \
            /* Check last write */                                                                        \
            retry = DAP_Data.transfer.retry_count;                                                        \
            do {                                                                                          \
                response_value = SWD_Transfer##speed(SWD_WithParity(DP_RDBUFF | DAP_TRANSFER_RnW), NULL); \
            } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);             \
        }                                                                                                 \
                                                                                                          \
    end:                                                                                                  \
        *(response_head + 0) = (uint8_t)(response_count >> 0);                                            \
        *(response_head + 1) = (uint8_t)(response_count >> 8);                                            \
        *(response_head + 2) = (uint8_t)response_value;                                                   \
                                                                                                          \
        return ((uint32_t)(response - response_head));                                                    \
    }

static uint32_t SWD_TransferBlockFast(const uint8_t* request, uint8_t* response) HOT_PATH;
static uint32_t SWD_TransferBlockSlow(const uint8_t* request, uint8_t* response) HOT_PATH;
SWD_TransferBlockFunction(Fast) SWD_TransferBlockFunction(Slow)

    /** The same return shape DAP.c's own DAP_TransferBlock produces. */
    COMMAND_PATH uint32_t SWD_TransferBlock(const uint8_t* request, uint8_t* response) {
    uint32_t num;

    if (DAP_Data.fast_clock) {
        num = SWD_TransferBlockFast(request, response);
    } else {
        num = SWD_TransferBlockSlow(request, response);
    }

    if ((*(request + 3) & DAP_TRANSFER_RnW) != 0U) {
        num |= 4U << 16;
    } else {
        num |= (4U + (((uint32_t)(*(request + 1)) | (uint32_t)(*(request + 2) << 8)) * 4)) << 16;
    }
    return num;
}
#endif

#ifdef SWD_CODEGEN
/* The transfers run generated code from SRAMX (src/swd_gen.c), at whatever period the host's clock
 * gives. What stays here is ARM's command logic around it: posted reads, the RDBUFF read that ends
 * a block, and WAIT retries, which the generated loop hands back by stopping at the transfer that
 * did not acknowledge OK. */

/** SW_DP.c's own transfer, renamed in CMakeLists.txt: what runs when nothing could be generated. */
extern uint8_t arm_SWD_Transfer(uint32_t request, uint32_t* data);

/** ARM's transfer behind the generated functions' contract, for when they could not be built. */
static uint64_t arm_words(uint32_t packet, uint8_t* words, uint32_t count) {
    const uint32_t request = (packet >> 1) & 0x0FU;
    for (uint32_t i = 0U; i < count; i++) {
        uint32_t data;
        if ((request & DAP_TRANSFER_RnW) == 0U) {
            __builtin_memcpy(&data, words, sizeof data);
        }
        const uint32_t ack = arm_SWD_Transfer(request, &data);
        if (ack != DAP_TRANSFER_OK) {
            return ack | ((uint64_t)(count - i) << 32);
        }
        if ((request & DAP_TRANSFER_RnW) != 0U) {
            __builtin_memcpy(words, &data, sizeof data);
        }
        words += sizeof data;
    }
    return DAP_TRANSFER_OK;
}

/** The read and write functions a command runs, chosen once when it starts.
 *
 * Nothing inside one command can change what the generator built for: the clock, the turnaround
 * and the idle cycles each come from a command of their own. Asking `swd_gen_ready` once per
 * transfer cost a call into flash every time. */
struct transfer_fns {
    swd_gen_fn read;
    swd_gen_fn write;
};

static struct transfer_fns transfer_fns(void) {
    if (!swd_gen_ready()) {
        return (struct transfer_fns){arm_words, arm_words};
    }
    return (struct transfer_fns){swd_gen_read, swd_gen_write};
}

static inline swd_gen_fn transfer_fn(const struct transfer_fns* fns, uint32_t request) {
    return ((request & DAP_TRANSFER_RnW) != 0U) ? fns->read : fns->write;
}

/** `count` transfers of `request`, each retried on WAIT as ARM's block loop retries it. Returns
 * the last acknowledge, and in `done` how many transfers completed. */
static uint32_t transfer_words(const struct transfer_fns* fns, uint32_t request, uint8_t* words, uint32_t count,
                               uint32_t* done) HOT_PATH;
static uint32_t transfer_words(const struct transfer_fns* fns, uint32_t request, uint8_t* words, uint32_t count,
                               uint32_t* done) {
    const swd_gen_fn fn = transfer_fn(fns, request);
    const uint32_t packet = swd_gen_packet(request);
    uint32_t ack = DAP_TRANSFER_OK;
    uint32_t n = 0U;
    while (count != 0U) {
        const uint64_t result = fn(packet, words, count);
        const uint32_t ok = count - (uint32_t)(result >> 32);
        ack = (uint32_t)result;
        n += ok;
        words += 4U * ok;
        count -= ok;
        if (ack == DAP_TRANSFER_OK) {
            break;
        }
        uint32_t retry = DAP_Data.transfer.retry_count;
        while ((ack == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort) {
            ack = (uint32_t)fn(packet, words, 1U);
        }
        if (ack != DAP_TRANSFER_OK) {
            break;
        }
        n++;
        words += 4U;
        count--;
    }
    *done = n;
    return ack;
}

static inline uint8_t transfer_one(const struct transfer_fns* fns, uint32_t request, uint32_t* data) {
    const swd_gen_fn fn = transfer_fn(fns, request);
    uint32_t word = 0U;
    if ((request & DAP_TRANSFER_RnW) == 0U) {
        word = *data;
    }
    const uint32_t ack = (uint32_t)fn(swd_gen_packet(request), (uint8_t*)&word, 1U);
    if (ack == DAP_TRANSFER_OK) {
        if (((request & DAP_TRANSFER_RnW) != 0U) && (data != NULL)) {
            *data = word;
        }
        if ((request & DAP_TRANSFER_TIMESTAMP) != 0U) {
            DAP_Data.timestamp = TIMESTAMP_GET();
        }
    }
    return (uint8_t)ack;
}

uint8_t SWD_Transfer(uint32_t request, uint32_t* data) {
    const struct transfer_fns fns = transfer_fns();
    return transfer_one(&fns, request, data);
}

/** The functions the running DAP_Transfer resolved, for the per-transfer call ARM's loop makes.
 * The loop comes out of a macro with a fixed signature, so they cannot travel as an argument. */
static struct transfer_fns command_fns;

#define SWD_XFER_GEN(request, data) transfer_one(&command_fns, request, data)

/* DAP_Transfer and DAP_TransferBlock run from SRAM. In flash, at 11 wait states, ARM's command
 * loop and the calls under it cost several microseconds a transfer, more than the wire does. */
// clang-format off: this expands to a function definition.
static uint32_t SWD_TransferCommandGen(const uint8_t* request, uint8_t* response) HOT_PATH;
SWD_TRANSFER_FUNCTION(SWD_TransferCommandGen, SWD_XFER_GEN)
// clang-format on

COMMAND_PATH uint32_t SWD_TransferCommand(const uint8_t* request, uint8_t* response) {
    command_fns = transfer_fns();
    return SWD_TransferCommandGen(request, response);
}

/** ARM's DAP_SWD_TransferBlock, a block at a time rather than a word at a time. */
static uint32_t SWD_TransferBlockGen(const struct transfer_fns* fns, const uint8_t* request, uint8_t* response)
    HOT_PATH;
static uint32_t SWD_TransferBlockGen(const struct transfer_fns* fns, const uint8_t* request, uint8_t* response) {
    uint32_t response_count = 0U;
    uint32_t response_value = 0U;
    uint8_t* response_head = response;
    uint32_t done;
    uint32_t discard;

    response += 3;
    DAP_TransferAbort = 0U;
    request++; /* Ignore DAP index */

    const uint32_t request_count = (uint32_t)(*(request + 0) << 0) | (uint32_t)(*(request + 1) << 8);
    request += 2;
    if (request_count == 0U) {
        goto end;
    }

    const uint32_t request_value = *request++;
    if ((request_value & DAP_TRANSFER_RnW) != 0U) {
        if ((request_value & DAP_TRANSFER_APnDP) != 0U) {
            /* Post AP read */
            response_value = transfer_words(fns, request_value, (uint8_t*)&discard, 1U, &done);
            if (response_value != DAP_TRANSFER_OK) {
                goto read_done;
            }
        }
        response_value = transfer_words(fns, request_value, response, request_count - 1U, &done);
        response += 4U * done;
        if (response_value != DAP_TRANSFER_OK) {
            goto read_done;
        }
        /* A block posted to an access port reads its last word out of RDBUFF. */
        const uint32_t last =
            ((request_value & DAP_TRANSFER_APnDP) != 0U) ? (DP_RDBUFF | DAP_TRANSFER_RnW) : request_value;
        response_value = transfer_words(fns, last, response, 1U, &done);
        response += 4U * done;
    read_done:
        response_count = (uint32_t)(response - response_head - 3) / 4U;
    } else {
        // The generated write only reads from `words`.
        response_value = transfer_words(fns, request_value, (uint8_t*)(uintptr_t)request, request_count, &done);
        response_count = done;
        if (response_value != DAP_TRANSFER_OK) {
            goto end;
        }
        /* Check last write */
        response_value = transfer_words(fns, DP_RDBUFF | DAP_TRANSFER_RnW, (uint8_t*)&discard, 1U, &done);
    }

end:
    *(response_head + 0) = (uint8_t)(response_count >> 0);
    *(response_head + 1) = (uint8_t)(response_count >> 8);
    *(response_head + 2) = (uint8_t)response_value;

    return ((uint32_t)(response - response_head));
}

/** The same return shape DAP.c's own DAP_TransferBlock produces. */
COMMAND_PATH uint32_t SWD_TransferBlock(const uint8_t* request, uint8_t* response) {
    const struct transfer_fns fns = transfer_fns();
    uint32_t num = SWD_TransferBlockGen(&fns, request, response);

    if ((*(request + 3) & DAP_TRANSFER_RnW) != 0U) {
        num |= 4U << 16;
    } else {
        num |= (4U + (((uint32_t)(*(request + 1)) | (uint32_t)(*(request + 2) << 8)) * 4)) << 16;
    }
    return num;
}
#endif
