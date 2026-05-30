# hft::protocol — header-only wire structs (OUCH / ITCH / risk + MsgHeader).
add_library(hft_protocol INTERFACE)
add_library(hft::protocol ALIAS hft_protocol)
target_include_directories(hft_protocol INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hft_protocol INTERFACE cxx_std_20)
target_link_libraries(hft_protocol INTERFACE hft::common)
