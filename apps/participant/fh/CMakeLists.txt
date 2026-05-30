# hft_fh
add_executable(hft_fh main.cpp feed_handler.cpp local_book.cpp)
target_link_libraries(hft_fh PRIVATE hft::common hft::ipc hft::protocol)
hft_set_warnings(hft_fh)
