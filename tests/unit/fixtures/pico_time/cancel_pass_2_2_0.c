/*
 * The default alarm pool's cancellation pass from the Raspberry Pi Pico SDK
 * 2.2.0, src/common/pico_time/time.c (alarm_pool_irq_handler), verbatim:
 * tests/unit/CMakeLists.txt patches it with cmake/picodeck_pico_time.cmake
 * when PICO_SDK_PATH has no SDK time.c to patch (the CI unit job).
 *
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
        if (pool->has_pending_cancellations) {
            pool->has_pending_cancellations = false;
            __compiler_memory_barrier();
            int16_t *prev = &pool->ordered_head;
            // set target for canceled items to -1, and move to front of the list
            for(int16_t index = pool->ordered_head; index != -1; ) {
                alarm_pool_entry_t *entry = &pool->entries[index];
                int16_t next = entry->next;
                if ((int16_t)entry->sequence < 0) {
                    // mark for deletion
                    entry->target = -1;
                    if (index != pool->ordered_head) {
                        // move to start of queue
                        *prev = entry->next;
                        entry->next = pool->ordered_head;
                        pool->ordered_head = index;
                    }
                } else {
                    prev = &entry->next;
                }
                index = next;
            }
        }
