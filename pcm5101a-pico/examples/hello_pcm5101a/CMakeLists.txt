add_executable(hello_pcm5101a main.c)

target_link_libraries(hello_pcm5101a PRIVATE
    pico_stdlib
    pcm5101a_i2s
)

pico_enable_stdio_usb(hello_pcm5101a 1)
pico_enable_stdio_uart(hello_pcm5101a 0)

pico_add_extra_outputs(hello_pcm5101a)
