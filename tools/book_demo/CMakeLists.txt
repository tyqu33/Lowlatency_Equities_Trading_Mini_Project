# book_demo — replay a script of orders through hft::matching and print the book after each.
# A demonstration tool, not a component: it exists so the book's behaviour can be seen rather than
# inferred from the test suite. Links both implementations and picks one with --book.
add_executable(hft_book_demo main.cpp)
target_link_libraries(hft_book_demo PRIVATE hft::matching hft::common hft::appkit)
hft_set_warnings(hft_book_demo)
