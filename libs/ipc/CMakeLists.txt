# hft::ipc — lock-free SPSC/SPMC rings + shared-memory segment management.
# spsc_ring.hpp / spmc_broadcast.hpp / ring_layout.hpp are header-only (templates).
add_library(hft_ipc STATIC
    src/shared_memory.cpp)
add_library(hft::ipc ALIAS hft_ipc)
target_include_directories(hft_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hft_ipc PUBLIC cxx_std_20)
target_link_libraries(hft_ipc PUBLIC hft::common)
# TODO (later): find_package(Boost) and link Boost::headers for Boost.Interprocess.
hft_set_warnings(hft_ipc)
