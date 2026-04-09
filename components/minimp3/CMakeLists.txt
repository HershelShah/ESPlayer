# minimp3 — single-header MP3 decoder, exposed as an ESP-IDF component.
# The actual decode code lives in minimp3.h; minimp3_impl.c pulls it in
# with MINIMP3_IMPLEMENTATION defined so it compiles exactly once.
idf_component_register(
    SRCS "minimp3_impl.c"
    INCLUDE_DIRS "."
)
