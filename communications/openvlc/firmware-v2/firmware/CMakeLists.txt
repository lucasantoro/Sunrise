cmake_minimum_required(VERSION 3.16)
project(openvlc_stm32_core C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(OPENVLC_CORE_SOURCES
	openvlc/src/openvlc_crc.c
	openvlc/src/openvlc_frame.c
	openvlc/src/openvlc_linecode.c
	openvlc/src/openvlc_phy.c
	openvlc/src/openvlc_app.c
)

function(configure_openvlc_core target nominal edge_min timing_min hist_max short_max preamble_cells hypotheses local_recovery list_recovery sfd_locks boundary_retry primary_phase)
add_library(${target} ${OPENVLC_CORE_SOURCES})
target_include_directories(${target} PUBLIC openvlc/include)
target_compile_definitions(${target} PUBLIC
	OPENVLC_RX_COMPARATOR=1
	OPENVLC_MAX_PAYLOAD_BYTES=900u
	OPENVLC_SAMPLES_PER_SYMBOL=5u
	OPENVLC_EDGE_MIN_INTERVAL_TICKS=${edge_min}
	OPENVLC_COMP_MIN_HALFCELL_TICKS=${timing_min}
	OPENVLC_COMP_NOMINAL_HALFCELL_TICKS=${nominal}
	OPENVLC_COMP_SPLIT_HIST_MAX_TICKS=${hist_max}
	OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS=${short_max}
	OPENVLC_SFD_SYNC_PREAMBLE_CELLS=${preamble_cells}
	OPENVLC_SFD_SYNC_SEARCH_CELLS=512u
	OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS=1u
	OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_BAD_PAIRS=1u
	OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_SFD_ERRORS=0u
	OPENVLC_SFD_SYNC_MAX_CELL_ERRORS=2u
	OPENVLC_SFD_SYNC_MAX_CONSECUTIVE_BAD_PAIRS=8u
	OPENVLC_SFD_SYNC_BAD_PAIR_INVERT_FIRST=1u
	OPENVLC_SFD_SYNC_HYPOTHESES_MAX=${hypotheses}
	OPENVLC_SFD_SYNC_LOCKS_MAX=${sfd_locks}
	OPENVLC_RX_LOCAL_TIMING=1u
	OPENVLC_RX_LOCAL_SYMBOL_RECOVERY=${local_recovery}
	OPENVLC_RX_LIST_RECOVERY=${list_recovery}
	OPENVLC_RX_THREE_CELL_RETRY_MAX=0u
	OPENVLC_RX_BOUNDARY_RETRY_MAX=${boundary_retry}
	OPENVLC_RX_BOUNDARY_RETRY_TOL_DIV=8u
	OPENVLC_RX_PRIMARY_PHASE_RECOVERY=${primary_phase}
	OPENVLC_TEST_API=1
)
endfunction()

configure_openvlc_core(openvlc_core_fast 32u 20u 12u 256u 96u 12u
	4u 1u 1u 4u 1u 0u)
target_compile_definitions(openvlc_core_fast PUBLIC
	OPENVLC_EDGE_HARD_GLITCH_TICKS=8u
	OPENVLC_EDGE_CONTEXT_MARGIN_TICKS=2u)
add_executable(host_loopback_fast tests/host_loopback.c)
target_compile_definitions(host_loopback_fast PRIVATE
	OPENVLC_TEST_HCELL=32u
	OPENVLC_TEST_DCD_DELAY=8u
	OPENVLC_TEST_DCD_STEP=2u
	OPENVLC_TEST_JITTER=1
	OPENVLC_TEST_GLITCH_MIN=20u
)
target_link_libraries(host_loopback_fast PRIVATE openvlc_core_fast)

configure_openvlc_core(openvlc_core_budget40 26u 12u 12u 192u 80u 12u
	4u 1u 1u 4u 1u 0u)
target_compile_definitions(openvlc_core_budget40 PUBLIC
	OPENVLC_EDGE_HARD_GLITCH_TICKS=6u
	OPENVLC_EDGE_CONTEXT_MARGIN_TICKS=2u)
add_executable(host_loopback_budget40 tests/host_loopback.c)
target_compile_definitions(host_loopback_budget40 PRIVATE
	OPENVLC_TEST_HCELL=26u
	OPENVLC_TEST_DCD_DELAY=8u
	OPENVLC_TEST_DCD_STEP=2u
	OPENVLC_TEST_JITTER=1
	OPENVLC_TEST_GLITCH_MIN=12u
)
target_link_libraries(host_loopback_budget40 PRIVATE openvlc_core_budget40)

configure_openvlc_core(openvlc_core_legacy 16u 8u 8u 96u 28u 12u
	4u 1u 1u 4u 1u 0u)
target_compile_definitions(openvlc_core_legacy PUBLIC
	OPENVLC_EDGE_HARD_GLITCH_TICKS=3u
	OPENVLC_EDGE_CONTEXT_MARGIN_TICKS=1u)
add_executable(host_loopback_legacy tests/host_loopback.c)
target_compile_definitions(host_loopback_legacy PRIVATE
	OPENVLC_TEST_HCELL=16u
	OPENVLC_TEST_DCD_DELAY=4u
	OPENVLC_TEST_DCD_STEP=1u
	OPENVLC_TEST_JITTER=1
	OPENVLC_TEST_GLITCH_MIN=8u
)
target_link_libraries(host_loopback_legacy PRIVATE openvlc_core_legacy)

enable_testing()
add_test(NAME host_loopback_fast COMMAND host_loopback_fast)
add_test(NAME host_loopback_budget40 COMMAND host_loopback_budget40)
add_test(NAME host_loopback_legacy COMMAND host_loopback_legacy)

# Exact RX recovery policy used by the saturated STM32 1-Mbit/s profile.
configure_openvlc_core(openvlc_core_fast_realtime
	32u 12u 12u 256u 96u 12u 1u 0u 0u 2u 1u 1u)
target_compile_definitions(openvlc_core_fast_realtime PUBLIC
	OPENVLC_RX_PHASE_MAX_EDITS=5u
	OPENVLC_RX_PHASE_REPAIR_BEFORE_DECODE=1u
	OPENVLC_RX_PAIR_TIMING_MODE=2u
	OPENVLC_RX_DIFFERENTIAL_FALLBACK=0u
	OPENVLC_RX_FUZZY_SFD_MIN_LOCK_CELL=350u
	OPENVLC_SFD_SYNC_MIN_LOCK_CELL=256u
	OPENVLC_RX_EDGE_FILTER_MODE=1u
	OPENVLC_EDGE_HARD_GLITCH_TICKS=8u
	OPENVLC_EDGE_CONTEXT_MARGIN_TICKS=2u)
configure_openvlc_core(openvlc_core_fast_realtime_test
	32u 12u 12u 256u 96u 12u 1u 0u 0u 2u 1u 1u)
target_compile_definitions(openvlc_core_fast_realtime_test PUBLIC
	OPENVLC_RX_PHASE_MAX_EDITS=5u
	OPENVLC_RX_PHASE_REPAIR_BEFORE_DECODE=1u
	OPENVLC_RX_PAIR_TIMING_MODE=2u
	OPENVLC_RX_DIFFERENTIAL_FALLBACK=0u
	OPENVLC_RX_FUZZY_SFD_MIN_LOCK_CELL=350u
	OPENVLC_RX_EDGE_FILTER_MODE=1u
	OPENVLC_EDGE_HARD_GLITCH_TICKS=8u
	OPENVLC_EDGE_CONTEXT_MARGIN_TICKS=2u)
add_executable(host_loopback_fast_realtime tests/host_loopback.c)
target_compile_definitions(host_loopback_fast_realtime PRIVATE
	OPENVLC_TEST_HCELL=32u
	OPENVLC_TEST_DCD_DELAY=8u
	OPENVLC_TEST_DCD_STEP=2u
	OPENVLC_TEST_JITTER=1
	OPENVLC_TEST_GLITCH_MIN=20u
)
target_link_libraries(host_loopback_fast_realtime PRIVATE
	openvlc_core_fast_realtime_test)
add_test(NAME host_loopback_fast_realtime COMMAND host_loopback_fast_realtime)
add_executable(failure_replay_fast_realtime tests/failure_replay.c)
target_link_libraries(failure_replay_fast_realtime PRIVATE
	openvlc_core_fast_realtime)

# Offline-only policy matrix. Enable it in a separate build directory when a
# raw capture corpus must identify which bounded recovery feature contributes
# useful frames. These targets never enter the STM32 image.
option(OPENVLC_BUILD_RX_EVAL_VARIANTS "Build RX replay policy matrix" OFF)
if(OPENVLC_BUILD_RX_EVAL_VARIANTS)
	function(add_rx_eval_variant suffix hypotheses local_recovery
		 list_recovery sfd_locks boundary_retry primary_phase
		 repair_before_decode edge_filter pair_timing)
		configure_openvlc_core(openvlc_core_eval_${suffix}
			32u 12u 12u 256u 96u 12u ${hypotheses}
			${local_recovery} ${list_recovery} ${sfd_locks}
			${boundary_retry} ${primary_phase})
		target_compile_definitions(openvlc_core_eval_${suffix} PUBLIC
			OPENVLC_RX_PHASE_MAX_EDITS=5u
			OPENVLC_RX_PHASE_REPAIR_BEFORE_DECODE=${repair_before_decode}
			OPENVLC_RX_EDGE_FILTER_MODE=${edge_filter}
			OPENVLC_RX_PAIR_TIMING_MODE=${pair_timing}
			OPENVLC_SFD_SYNC_MIN_LOCK_CELL=256u
			OPENVLC_EDGE_HARD_GLITCH_TICKS=8u
			OPENVLC_EDGE_CONTEXT_MARGIN_TICKS=2u)
		add_executable(failure_replay_eval_${suffix}
			tests/failure_replay.c)
		target_link_libraries(failure_replay_eval_${suffix} PRIVATE
			openvlc_core_eval_${suffix})
	endfunction()

	# Every row starts from the deployed profile-1000 policy: legacy raw-edge
	# gate at 12 ticks, pair timing mode 2, one SFD hypothesis, two locks,
	# bounded boundary retry and proactive phase repair.  A target below changes
	# only the feature named by its suffix unless it explicitly combines names.
	add_rx_eval_variant(h4          4u 0u 0u 2u 1u 1u 1u 1u 2u)
	add_rx_eval_variant(local       1u 1u 0u 2u 1u 1u 1u 1u 2u)
	add_rx_eval_variant(list        1u 0u 1u 2u 1u 1u 1u 1u 2u)
	add_rx_eval_variant(locks4      1u 0u 0u 4u 1u 1u 1u 1u 2u)
	add_rx_eval_variant(boundary    1u 0u 0u 2u 1u 1u 1u 1u 2u)
	add_rx_eval_variant(no_boundary 1u 0u 0u 2u 0u 1u 1u 1u 2u)
	add_rx_eval_variant(pair_general 1u 0u 0u 2u 1u 1u 1u 1u 1u)
	# Colleague-style event decoder reference: classify each edge interval
	# independently as an integer number of cells.  Framing and CRC/RS remain
	# enabled so its result is directly comparable with the deployed decoder.
	add_rx_eval_variant(independent  1u 0u 0u 2u 1u 1u 1u 1u 0u)
	add_rx_eval_variant(diff_fallback 1u 0u 0u 2u 1u 1u 1u 1u 2u)
	target_compile_definitions(openvlc_core_eval_diff_fallback PUBLIC
		OPENVLC_RX_DIFFERENTIAL_FALLBACK=1u)
	add_rx_eval_variant(exact_sfd     1u 0u 0u 2u 1u 1u 1u 1u 2u)
	target_compile_definitions(openvlc_core_eval_exact_sfd PUBLIC
		OPENVLC_RX_EXACT_SFD=1u)
	add_rx_eval_variant(fuzzy384      1u 0u 0u 2u 1u 1u 1u 1u 2u)
	target_compile_definitions(openvlc_core_eval_fuzzy384 PUBLIC
		OPENVLC_RX_FUZZY_SFD_MIN_LOCK_CELL=384u)
	add_rx_eval_variant(fuzzy350      1u 0u 0u 2u 1u 1u 1u 1u 2u)
	target_compile_definitions(openvlc_core_eval_fuzzy350 PUBLIC
		OPENVLC_RX_FUZZY_SFD_MIN_LOCK_CELL=350u)
	add_rx_eval_variant(phase_lazy  1u 0u 0u 2u 1u 1u 0u 1u 2u)
	add_rx_eval_variant(stream      1u 0u 0u 2u 1u 0u 1u 1u 2u)
	add_rx_eval_variant(contextual  1u 0u 0u 2u 1u 1u 1u 2u 2u)
	add_rx_eval_variant(h4local     4u 1u 0u 2u 1u 1u 1u 1u 2u)
	add_rx_eval_variant(h4list      4u 0u 1u 2u 1u 1u 1u 1u 2u)
endif()

add_executable(capture_decode_fast tests/capture_decode.c)
target_link_libraries(capture_decode_fast PRIVATE openvlc_core_fast)
if(NOT MSVC)
	target_link_libraries(capture_decode_fast PRIVATE m)
endif()

add_executable(failure_replay_fast tests/failure_replay.c)
target_link_libraries(failure_replay_fast PRIVATE openvlc_core_fast)

add_executable(tx_scheduler_test
	tests/tx_scheduler.c
	platform/stm32h7/openvlc_stm32_tx_hal.c
	src/openvlc_tx_compat.c
)
target_include_directories(tx_scheduler_test PRIVATE
	include
	platform/stm32h7
)
target_compile_definitions(tx_scheduler_test PRIVATE
	OPENVLC_TEST_API=1
	OPENVLC_TX_USE_TIMER_OC=1
	OPENVLC_STM32_TX_PROFILE_BUDGET=50u
	OPENVLC_STM32_TX_TIMER_HZ=192000000u
	OPENVLC_STM32_TX_BUDGET40_CELL_TICKS=78u
	OPENVLC_STM32_TX_BUDGET50_CELL_TICKS=96u
	OPENVLC_STM32_TX_BUDGET100_CELL_TICKS=192u
	OPENVLC_STM32_TX_GAP_CELLS=32u
	OPENVLC_STM32_TX_WARMUP_CELLS=384u
	OPENVLC_STM32_TX_SLOT_COUNT=3u
)
target_link_libraries(tx_scheduler_test PRIVATE openvlc_core_budget40)
add_test(NAME tx_scheduler_fifo COMMAND tx_scheduler_test)

# ---------------------------------------------------------------------------
# Device-faithful replay target.
#
# The other cores above are historical A/B configurations. This one mirrors the
# values the Pi-HAT firmware actually compiles (extracted from Core/Inc/
# openvlc_board.h with the ARM preprocessor), so an offline replay result is
# valid evidence about the deployed build. Do NOT reuse configure_openvlc_core
# here: that helper pins SFD_SYNC_SEARCH_CELLS to 512, while the firmware needs
# 2560 once the TX idle keep-alive prepends idle cells to every burst.
# Re-extract and update these whenever openvlc_board.h changes.
# ---------------------------------------------------------------------------
add_library(openvlc_core_device ${OPENVLC_CORE_SOURCES})
target_include_directories(openvlc_core_device PUBLIC openvlc/include)
target_compile_definitions(openvlc_core_device PUBLIC
	OPENVLC_RX_COMPARATOR=1
	OPENVLC_RX_BBB_PRU_COMPAT_ONLY=1
	OPENVLC_MAX_PAYLOAD_BYTES=900u
	OPENVLC_BEAGLEBONE_RS_ECC_BYTES=16u
	OPENVLC_PREAMBLE_BYTES=8u
	OPENVLC_SAMPLES_PER_SYMBOL=5u
	OPENVLC_EDGE_MIN_INTERVAL_TICKS=11u
	OPENVLC_COMP_MIN_HALFCELL_TICKS=12u
	OPENVLC_COMP_NOMINAL_HALFCELL_TICKS=32u
	OPENVLC_COMP_SPLIT_HIST_MAX_TICKS=256u
	OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS=96u
	OPENVLC_RX_EDGE_FILTER_MODE=1u
	OPENVLC_EDGE_HARD_GLITCH_TICKS=8u
	OPENVLC_EDGE_CONTEXT_MARGIN_TICKS=2u
	OPENVLC_RX_TIMING_SEARCH_EDGES=1536u
	OPENVLC_SFD_SYNC_PREAMBLE_CELLS=12u
	OPENVLC_SFD_SYNC_SEARCH_CELLS=2560u
	OPENVLC_SFD_SYNC_MIN_LOCK_CELL=256u
	OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS=1u
	OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_BAD_PAIRS=1u
	OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_SFD_ERRORS=0u
	OPENVLC_SFD_SYNC_MAX_CELL_ERRORS=2u
	OPENVLC_SFD_SYNC_MAX_CONSECUTIVE_BAD_PAIRS=8u
	OPENVLC_SFD_SYNC_BAD_PAIR_INVERT_FIRST=1u
	OPENVLC_SFD_SYNC_HYPOTHESES_MAX=2u
	OPENVLC_SFD_SYNC_LOCKS_MAX=2u
	OPENVLC_RX_LOCAL_TIMING=1u
	OPENVLC_RX_LOCAL_SYMBOL_RECOVERY=0u
	OPENVLC_RX_LIST_RECOVERY=0u
	OPENVLC_RX_THREE_CELL_RETRY_MAX=0u
	OPENVLC_RX_BOUNDARY_RETRY_MAX=1u
	OPENVLC_RX_BOUNDARY_RETRY_TOL_DIV=8u
	OPENVLC_RX_PRIMARY_PHASE_RECOVERY=1u
	OPENVLC_TEST_API=1
)
add_executable(failure_replay_device tests/failure_replay.c)
target_link_libraries(failure_replay_device PRIVATE openvlc_core_device)
