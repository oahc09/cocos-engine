# native/tests/benchmarks/CMakeLists.txt
# M1-S3: TypeRegistry + AssetRefManager micro-benchmarks (G-15)

set(CWD ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# ── bench_type_registry ──────────────────────────────────────────────────
# Links against the full engine (cocos_engine) because TypeRegistry depends
# on ccstd containers and CCObject hierarchy.
add_executable(bench_type_registry bench_type_registry.cpp)

target_include_directories(bench_type_registry PRIVATE
    ${CWD}
    ${CWD}/cocos
)

target_link_libraries(bench_type_registry PRIVATE
    ${ENGINE_NAME}
)

# ── bench_ref_manager ────────────────────────────────────────────────────
# Self-contained: embeds a minimal mock Asset + AssetRefManager for isolated
# micro-benchmarking. No engine linkage needed.
add_executable(bench_ref_manager bench_ref_manager.cpp)
