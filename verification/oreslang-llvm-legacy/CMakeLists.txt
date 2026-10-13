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
