# executabilul principal
add_executable(smart_home_gateway
    main.c
    ds18b20.c
    ssd1306.c
)

# includem directorul curent (pentru secrets.h)
target_include_directories(smart_home_gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# librariile pico sdk necesare
target_link_libraries(smart_home_gateway
    pico_stdlib                 # gpio, uart, timer
    pico_multicore              # dual-core (core1 pentru senzor temperatura)
    hardware_gpio               # control gpio (relay, led, butoane)
    hardware_i2c                # comunicatie i2c (oled ssd1306)
    # pico_cyw43_arch_lwip_poll # wifi + lwip (mod polling, non-blocant)
    hardware_adc                # citire adc (senzor lumina)
    # hardware_dma              # transfer dma (oled framebuffer)
    # hardware_timer            # alarme si timere hardware
)

# activam usb serial pentru debug (printf -> usb)
pico_enable_stdio_usb(smart_home_gateway 1)

# dezactivam uart serial (nu il folosim)
pico_enable_stdio_uart(smart_home_gateway 0)

# generam fisierele .uf2, .hex, .bin pe langa .elf
pico_add_extra_outputs(smart_home_gateway)