cmake_minimum_required(VERSION 3.20)
project(oreslang_llvm_legacy_regressions LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(LLVM CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(oreslang-llvmc
  source/src/main/cpp/oreslang/main.cpp
  source/src/main/cpp/oreslang/frontend/Parser.cpp
  source/src/main/cpp/oreslang/backend/LlvmCodegen.cpp)
target_include_directories(oreslang-llvmc PRIVATE source/src/main/cpp ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(ORE_LLVM_LIBS core support)
target_link_libraries(oreslang-llvmc PRIVATE ${ORE_LLVM_LIBS})
enable_testing()
set(FIX ${CMAKE_CURRENT_SOURCE_DIR}/source/tests/fixtures)
set(CONTRACT ${CMAKE_CURRENT_SOURCE_DIR}/source/tests/contract.py)
function(reject name fixture expected)
  add_test(NAME ${name} COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
    --compiler $<TARGET_FILE:oreslang-llvmc>
    --fixture ${FIX}/${fixture}
    --diagnostic "${expected}"
    --diagnostic-not "actor instance data must be declared in one state map")
endfunction()
reject(reject_duplicate_state_key actor-duplicate-field.ores "duplicate actor state key")
reject(reject_unsupported_state_type actor-unsupported-field.ores "expected 'int'")
reject(reject_unknown_lexical_variable actor-unknown-field.ores "unknown parameter")
reject(reject_actor_generated_abi_collision actor-abi-actor-collision.ores "conflicting generated actor symbol")
reject(reject_function_generated_abi_collision actor-abi-func-collision.ores "conflicting generated actor symbol")
function(accept_method name fixture symbol)
  add_test(NAME ${name} COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
    --compiler $<TARGET_FILE:oreslang-llvmc>
    --fixture ${FIX}/${fixture}
    --ir "${symbol}"
    --ir-not "call .*@ores_actor_i64_heap_field_write")
endfunction()
accept_method(method_only_historical_acceptance unsupported-actor.ores "oreslang_actor_Worker_run")
accept_method(method_only_current_acceptance actor-method-only-positive.ores "oreslang_actor_MethodOnly_increment")
# Negative canary: even -O Python must fail when the forbidden reason appears.
add_test(NAME optimized_forbidden_diagnostic_canary
  COMMAND ${Python3_EXECUTABLE} -O ${CONTRACT}
    --compiler $<TARGET_FILE:oreslang-llvmc>
    --fixture ${FIX}/actor-raw-field-reject.ores
    --diagnostic "actor instance data must be declared in one state map"
    --diagnostic-not "actor instance data must be declared in one state map")
set_tests_properties(optimized_forbidden_diagnostic_canary PROPERTIES WILL_FAIL TRUE)

# Ordinary actor methods: real compiler lowering of synchronous scoped loans.
function(method_ir name fixture needle)
 add_test(NAME ${name} COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
   --compiler $<TARGET_FILE:oreslang-llvmc>
   --fixture ${FIX}/${fixture}
   --ir "${needle}")
endfunction()
function(method_reject name fixture diagnostic)
 add_test(NAME ${name} COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
   --compiler $<TARGET_FILE:oreslang-llvmc>
   --fixture ${FIX}/${fixture}
   --diagnostic "${diagnostic}")
endfunction()
method_ir(method_state_write_codegen actor-method-state-write.ores "oreslang_actor_Counter_increment")
method_ir(method_state_read_codegen actor-method-state-read.ores "oreslang_actor_Counter_current")
method_ir(method_state_write_only_codegen actor-method-state-write-only.ores "oreslang_actor_Counter_reset")
method_reject(method_missing_effects actor-method-state-no-effects-reject.ores "actor state method requires explicit with effects")
method_reject(method_read_is_immutable actor-method-state-read-mutation-reject.ores "read-only actor state loan cannot mutate state")
method_reject(method_read_effect_missing actor-method-state-underdeclared-reject.ores "state.read operation not permitted by with effects")
method_reject(method_cannot_suspend actor-method-state-suspend-reject.ores "state read/write loan cannot suspend or nest")
method_reject(method_no_loan_escape actor-method-state-escape-reject.ores "actor state reference cannot escape handle_state callback")
add_test(NAME method_read_IR_no_field_write COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
    --compiler $<TARGET_FILE:oreslang-llvmc> --fixture ${FIX}/actor-method-state-read.ores
    --ir-not "call .*@ores_actor_i64_heap_field_write")
add_test(NAME method_write_IR_has_field_write COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
    --compiler $<TARGET_FILE:oreslang-llvmc> --fixture ${FIX}/actor-method-state-write.ores
    --ir "call void @ores_actor_i64_heap_field_write")

find_program(ORE_CLANG NAMES clang clang-19 clang-18 clang-17 REQUIRED)
add_test(NAME compiled_actor_method_state_native_heap
 COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc>
 --fixture ${FIX}/actor-method-state-runtime.ores --run 43
 --clang ${ORE_CLANG} --runs 3
 --native-harness ${CMAKE_CURRENT_SOURCE_DIR}/source/tests/native_actor_method_state_harness.c
 --actor-runtime ${CMAKE_CURRENT_SOURCE_DIR}/source/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
add_test(NAME optimized_actor_method_write_only_no_reads
 COMMAND ${Python3_EXECUTABLE} -O ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc>
 --fixture ${FIX}/actor-method-state-write-only.ores
 --ir "oreslang_actor_Counter_reset"
 --ir-not "call .*@ores_actor_i64_heap_field_read")
add_test(NAME sanitizer_actor_method_state_native_heap
 COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc>
 --fixture ${FIX}/actor-method-state-runtime.ores --run 43
 --clang ${ORE_CLANG} --runs 3 --sanitize
 --native-harness ${CMAKE_CURRENT_SOURCE_DIR}/source/tests/native_actor_method_state_harness.c
 --actor-runtime ${CMAKE_CURRENT_SOURCE_DIR}/source/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
set_tests_properties(compiled_actor_method_state_native_heap sanitizer_actor_method_state_native_heap PROPERTIES TIMEOUT 100)

add_test(NAME historical_multikey_actor_codegen
 COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc>
 --fixture ${FIX}/actor-owned-fields.ores
 --ir "oreslang_actor_Counter_total_value")
add_test(NAME historical_multikey_actor_execution
 COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc> --fixture ${FIX}/actor-owned-fields.ores
 --run 43 --clang ${ORE_CLANG} --runs 3
 --native-harness ${CMAKE_CURRENT_SOURCE_DIR}/source/tests/native_actor_owned_state_harness.c
 --actor-runtime ${CMAKE_CURRENT_SOURCE_DIR}/source/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
add_test(NAME historical_multikey_actor_asan_ubsan
 COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc> --fixture ${FIX}/actor-owned-fields.ores
 --run 43 --clang ${ORE_CLANG} --runs 3 --sanitize
 --native-harness ${CMAKE_CURRENT_SOURCE_DIR}/source/tests/native_actor_owned_state_harness.c
 --actor-runtime ${CMAKE_CURRENT_SOURCE_DIR}/source/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
add_test(NAME historical_immutable_read_grant_reject
 COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc> --fixture ${FIX}/actor-immutable-write.ores
 --diagnostic "read-only actor state loan cannot mutate state"
 --diagnostic-not "actor instance data must be declared in one state map")
add_test(NAME historical_unknown_write_reject
 COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc> --fixture ${FIX}/actor-unknown-write.ores
 --diagnostic "state callback writes unknown or immutable key"
 --diagnostic-not "actor instance data must be declared in one state map")
add_test(NAME historical_lexical_state_shadow_accept
 COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc> --fixture ${FIX}/actor-field-shadow.ores
 --ir "oreslang_actor_Counter_read")
set_tests_properties(historical_multikey_actor_execution historical_multikey_actor_asan_ubsan PROPERTIES TIMEOUT 100)

# Historical guest DATA send, native mailbox lifecycle, and typed 3-actor
# pipeline, with original behavioral evidence, not just IR admission.
set(RUNTIME ${CMAKE_CURRENT_SOURCE_DIR}/source/src/main/cpp/oreslang/runtime)
function(guest_runtime name fixture)
 add_test(NAME ${name} COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc> --fixture ${FIX}/${fixture}
 --run 43 --clang ${ORE_CLANG} --runs 2
 --actor-runtime ${RUNTIME}/ActorOwnedI64Heap.c
 --scheduler-runtime ${RUNTIME}/NativeActorScheduler.c
 --source-actor-runtime ${RUNTIME}/SourceActorBootstrap.c ${ARGN})
 set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()
guest_runtime(historical_spawn_stop_guest source-actor-spawn-stop.ores
 --stderr "SOURCE_ACTOR_SPAWN_STOP_VERIFIED")
guest_runtime(historical_send_guest source-actor-send.ores
 --stderr "processed=2 last=3")
guest_runtime(historical_five_workers_guest source-actor-five-workers.ores
 --stderr "processed=2 last=3" --stderr-count 5)
guest_runtime(historical_three_actor_typed_roundtrip source-actor-pipeline-bidirectional.ores
 --stderr "SOURCE_PIPELINE_ROUNDTRIP_PASS actors=3 typed=1 forward=2 backward=2 result=19"
 --pipeline-three-actor-summary --pipeline-context-summary)
add_test(NAME historical_three_actor_asan_ubsan
 COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc>
 --fixture ${FIX}/source-actor-pipeline-bidirectional.ores
 --run 43 --clang ${ORE_CLANG} --runs 2 --sanitize
 --actor-runtime ${RUNTIME}/ActorOwnedI64Heap.c
 --scheduler-runtime ${RUNTIME}/NativeActorScheduler.c
 --source-actor-runtime ${RUNTIME}/SourceActorBootstrap.c
 --stderr "SOURCE_PIPELINE_ROUNDTRIP_PASS actors=3 typed=1 forward=2 backward=2 result=19"
 --pipeline-three-actor-summary --pipeline-context-summary)
set_tests_properties(historical_three_actor_asan_ubsan PROPERTIES TIMEOUT 120)

guest_runtime(historical_nominal_payloadless_guest source-tagged-payloadless-dispatch.ores
 --stderr "processed=3 last=14")
guest_runtime(historical_nominal_int_payload_guest source-tagged-int-payload-dispatch.ores
 --stderr "processed=3 last=23")
guest_runtime(historical_signed_i64_messagepack_guest source-tagged-int64-canonical-boundaries.ores
 --stderr "processed=9 last=-4294999972")
function(sanitized_tagged name fixture expected)
 add_test(NAME ${name} COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc> --fixture ${FIX}/${fixture}
 --run 43 --clang ${ORE_CLANG} --runs 2 --sanitize
 --actor-runtime ${RUNTIME}/ActorOwnedI64Heap.c
 --scheduler-runtime ${RUNTIME}/NativeActorScheduler.c
 --source-actor-runtime ${RUNTIME}/SourceActorBootstrap.c
 --stderr "${expected}")
 set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()
sanitized_tagged(historical_nominal_payloadless_asan source-tagged-payloadless-dispatch.ores "processed=3 last=14")
sanitized_tagged(historical_nominal_int_payload_asan source-tagged-int-payload-dispatch.ores "processed=3 last=23")
sanitized_tagged(historical_signed_i64_asan source-tagged-int64-canonical-boundaries.ores "processed=9 last=-4294999972")

add_test(NAME historical_boolean_state_copy_phi
 COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc> --fixture ${FIX}/message-receive-bool-match.ores
 --ir "bool.state.loan.copy")
guest_runtime(historical_boolean_received_guest message-receive-bool-match.ores
 --stderr "processed=2 last=3")
add_test(NAME historical_boolean_received_asan_ubsan
 COMMAND ${Python3_EXECUTABLE} ${CONTRACT}
 --compiler $<TARGET_FILE:oreslang-llvmc> --fixture ${FIX}/message-receive-bool-match.ores
 --run 43 --clang ${ORE_CLANG} --runs 2 --sanitize
 --actor-runtime ${RUNTIME}/ActorOwnedI64Heap.c
 --scheduler-runtime ${RUNTIME}/NativeActorScheduler.c
 --source-actor-runtime ${RUNTIME}/SourceActorBootstrap.c
 --stderr "processed=2 last=3")
set_tests_properties(historical_boolean_received_asan_ubsan PROPERTIES TIMEOUT 120)
