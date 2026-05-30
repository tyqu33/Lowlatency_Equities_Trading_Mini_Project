# Sanitizers.cmake
# Applies AddressSanitizer + UndefinedBehaviorSanitizer globally when
# -DHFT_ENABLE_SANITIZERS=ON. Useful for shaking out memory/UB bugs in the
# lock-free rings and the order book.

if(HFT_ENABLE_SANITIZERS AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Note: ThreadSanitizer (-fsanitize=thread) is mutually exclusive with ASan.
# Enable it manually (a separate build dir) when chasing data races across the
# SPSC/SPMC rings:  cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
