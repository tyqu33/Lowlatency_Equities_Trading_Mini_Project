# hft_risk
add_executable(hft_risk main.cpp risk_engine.cpp)
target_link_libraries(hft_risk PRIVATE hft::common hft::ipc hft::protocol)
hft_set_warnings(hft_risk)
