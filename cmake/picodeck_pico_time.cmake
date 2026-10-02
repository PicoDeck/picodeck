# pico_time's default alarm pool — a fixed copy of the SDK's time.c (issue #58).
#
# In SDK 2.1.0 to 2.3.0 (release builds pin 2.2.0), alarm_pool_irq_handler's
# cancellation pass moves each cancelled entry to the front of the ordered
# list through `prev`, the link that points at it. When the head itself is
# cancelled it stays where it is, but `prev` is left pointing at
# pool->ordered_head instead of the head's own `next`, so the next cancelled
# entry in the same pass is unlinked by overwriting ordered_head: the old head
# drops out of every list and its pool slot is gone until a reboot. Any two
# alarms at the front of the list cancelled before the handler runs do it
# (the CYW43 driver's cross-core wake-ups cancel all the time, and other
# cancels pair with them). On the device the pool lost a few of its 16 slots
# per video session with WiFi on; once it was full the keyboard bus engine
# could not arm its timeout (it has its own hardware alarm now), the USB
# console's one-shot alarms failed so dev-command transfers crawled and
# aborted, and (by reading; not seen) the CYW43 driver's async context, which
# retries its alarm in a loop, would spin in its low-priority interrupt.
# SDK 2.3.1 rewrote that pass (raspberrypi/pico-sdk#3127): the fix below is
# the one line it needs, `prev` stepping past a head that stays.
#
# picodeck_patch_pico_time() compiles a copy of the SDK's time.c with that
# line added (written under CMAKE_BINARY_DIR/sdk_patched; the SDK is never
# modified) in place of the original, plus picodeck_alarm_pool_census() for
# the `alarmpool` dev command. Line endings are normalised first (a CRLF
# checkout matches too). An SDK older than 2.3.1 without the expected text
# fails the configure, so a leaking pool never ships; 2.3.1 and later build
# unpatched. Drop this file once the SDK is past 2.3.0. Call it after
# pico_sdk_init(). tests/unit/test_pico_time_pass.c runs the patched pass,
# cut out of the same generated text (picodeck_pico_time_cut_pass).

set(_PICODECK_PICO_TIME_FROM
"                        pool->ordered_head = index;
                    }
                } else {
                    prev = &entry->next;")
set(_PICODECK_PICO_TIME_TO
"                        pool->ordered_head = index;
                    } else {
                        // PicoDeck (cmake/picodeck_pico_time.cmake): a head
                        // that stays put is passed like any other entry, or
                        // the next cancelled one overwrites ordered_head and
                        // the head leaks out of every list.
                        prev = &entry->next;
                    }
                } else {
                    prev = &entry->next;")
# On the pool's core (core 0), task context; it returns false on the other.
# The pool's spin lock masks this core's interrupts, so its IRQ handler cannot
# move entries mid-walk, and the other core only touches the free and new
# lists under that lock. An entry a core is adding between its two lock
# sections reads as lost for that moment (the dev command reads again).
set(_PICODECK_PICO_TIME_CENSUS [=[

#if !PICO_TIME_DEFAULT_ALARM_POOL_DISABLED
// PicoDeck (cmake/picodeck_pico_time.cmake): the default pool's slots for the
// `alarmpool` dev command: free, and lost (in no list; only a leak puts one
// there). The pool's core (core 0), task context; false on the other core,
// where its interrupt handler could move entries under the walk.
bool picodeck_alarm_pool_census(int *free_slots, int *lost_slots);
bool picodeck_alarm_pool_census(int *free_slots, int *lost_slots) {
    alarm_pool_t *pool = alarm_pool_get_default();
    if (get_core_num() != pool->core_num)
        return false;
    int cap = pool->num_entries, listed = 0, nfree = 0;
    uint32_t save = spin_lock_blocking(pool->lock);
    for (int16_t i = pool->ordered_head; i >= 0 && listed <= cap; i = pool->entries[i].next)
        listed++;
    for (int16_t i = pool->new_head; i >= 0 && listed <= cap; i = pool->entries[i].next)
        listed++;
    for (int16_t i = pool->free_head; i >= 0 && nfree <= cap; i = pool->entries[i].next)
        nfree++;
    spin_unlock(pool->lock, save);
    *free_slots = nfree;
    *lost_slots = cap - listed - nfree;
    return true;
}
#endif
]=])

# picodeck_pico_time_patch_text(<text_var> <applied_var>): <text_var> holds a
# pico_time time.c (or a part with its cancellation pass). Its line endings
# become LF and, when it has the 2.1.0-2.3.0 pass, the fix is applied and
# <applied_var> is TRUE.
function(picodeck_pico_time_patch_text _var _applied)
    string(REPLACE "\r\n" "\n" _text "${${_var}}")
    string(FIND "${_text}" "${_PICODECK_PICO_TIME_FROM}" _at)
    if(_at EQUAL -1)
        set(${_applied} FALSE PARENT_SCOPE)
    else()
        string(REPLACE "${_PICODECK_PICO_TIME_FROM}" "${_PICODECK_PICO_TIME_TO}"
               _text "${_text}")
        set(${_applied} TRUE PARENT_SCOPE)
    endif()
    set(${_var} "${_text}" PARENT_SCOPE)
endfunction()

# picodeck_pico_time_cut_pass(<text> <out_var>): the cancellation pass, from
# `if (pool->has_pending_cancellations) {` to its closing brace (LF text).
function(picodeck_pico_time_cut_pass _text _out)
    string(FIND "${_text}" "if (pool->has_pending_cancellations) {" _a)
    if(_a EQUAL -1)
        message(FATAL_ERROR "picodeck_pico_time_cut_pass: no cancellation pass")
    endif()
    string(SUBSTRING "${_text}" ${_a} -1 _rest)
    set(_end "                index = next;\n            }\n        }")
    string(FIND "${_rest}" "${_end}" _b)
    if(_b EQUAL -1)
        message(FATAL_ERROR "picodeck_pico_time_cut_pass: the pass has no end")
    endif()
    string(LENGTH "${_end}" _n)
    math(EXPR _len "${_b} + ${_n}")
    string(SUBSTRING "${_rest}" 0 ${_len} _pass)
    set(${_out} "${_pass}\n" PARENT_SCOPE)
endfunction()

# Write only when the content changes, so rebuilds stay incremental.
function(_picodeck_pico_time_write _dst _text)
    if(EXISTS "${_dst}")
        file(READ "${_dst}" _old)
        if("${_old}" STREQUAL "${_text}")
            return()
        endif()
    endif()
    file(WRITE "${_dst}" "${_text}")
endfunction()

function(picodeck_patch_pico_time)
    if(NOT TARGET pico_time)
        message(FATAL_ERROR "picodeck_patch_pico_time: no pico_time target; "
                            "call it after pico_sdk_init()")
    endif()
    get_target_property(_srcs pico_time INTERFACE_SOURCES)
    set(_src "")
    foreach(_s IN LISTS _srcs)
        if(_s MATCHES "/pico_time/time\\.c$")
            set(_src "${_s}")
        endif()
    endforeach()
    if(NOT _src OR NOT EXISTS "${_src}")
        message(FATAL_ERROR "picodeck_patch_pico_time: pico_time has no time.c "
                            "(${_srcs}); update cmake/picodeck_pico_time.cmake")
    endif()
    file(READ "${_src}" _text)
    picodeck_pico_time_patch_text(_text _applied)
    if(NOT _applied)
        if(NOT DEFINED PICO_SDK_VERSION_STRING OR
           PICO_SDK_VERSION_STRING VERSION_LESS 2.3.1)
            message(FATAL_ERROR
                "picodeck_patch_pico_time: ${_src} (SDK "
                "${PICO_SDK_VERSION_STRING}) does not have the cancellation "
                "pass this patch fixes, but SDKs before 2.3.1 leak alarm pool "
                "slots (issue #58). Update cmake/picodeck_pico_time.cmake.")
        endif()
        message(STATUS "picodeck_patch_pico_time: SDK ${PICO_SDK_VERSION_STRING} "
                       "fixed the alarm pool's cancellation pass; time.c unpatched")
        return()
    endif()
    set(_text "/* PicoDeck: patched copy of ${_src}\n   generated by cmake/picodeck_pico_time.cmake (issue #58) — do not edit. */\n${_text}\n${_PICODECK_PICO_TIME_CENSUS}")
    set(_dst "${CMAKE_BINARY_DIR}/sdk_patched/pico_time/time.c")
    _picodeck_pico_time_write("${_dst}" "${_text}")
    list(REMOVE_ITEM _srcs "${_src}")
    list(APPEND _srcs "${_dst}")
    set_property(TARGET pico_time PROPERTY INTERFACE_SOURCES "${_srcs}")
    # Re-run the patch when the SDK's file changes.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
    message(STATUS "picodeck_patch_pico_time: alarm pool cancellation fix applied")
endfunction()
