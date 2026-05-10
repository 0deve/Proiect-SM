#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/mutex.h"
#include "pico/cyw43_arch.h"
#include "ds18b20.h"
#include "ssd1306.h"
#include "wifi_server.h"
#include "hardware/adc.h"

// Pinii hardware
#define LED_PIN_AUTO  17
#define LED_PIN_MANUAL 20
#define BTN_MODE_PIN  18  // Schimba modul: manual <-> auto
#define BTN_RELAY_PIN 19  // Porneste/opreste releul (doar in modul manual)
#define RELAY_PIN     16
#define DS18B20_PIN   15
#define LIGHT_PIN     26  // KY-018 senzor lumina (ADC0)

// Pini I2C pentru OLED SSD1306
#define OLED_SDA_PIN  4
#define OLED_SCL_PIN  5

// Praguri configurabile (se pot schimba din aplicatie)
volatile float temp_threshold_day   = 25.0f;  // Temperatura de pornire releu ZIUA
volatile float temp_threshold_night = 20.0f;  // Temperatura de pornire releu NOAPTEA
volatile uint16_t light_threshold   = 50;     // Peste 50 = intuneric (noapte), sub 50 = lumina (zi)

volatile bool auto_mode = false;      
volatile bool relay_state = false;
volatile float ultima_temperatura = 0.0f;
volatile bool temperatura_valida = false;
volatile uint16_t ultima_lumina = 0;         // Ultima valoare citita de la senzorul de lumina
volatile bool is_daytime = true;             // true = zi, false = noapte

// Display OLED global
ssd1306_t oled;

// Mutex pentru protejarea variabilelor partajate intre core-uri
mutex_t state_mutex;

// Flag pentru actualizarea OLED-ului doar din Core 0 (I2C nu e thread-safe)
volatile bool oled_needs_update = false;

// Actualizeaza display-ul OLED cu modul curent
void oled_update_display() {
    ssd1306_clear(&oled);

    // Chenar decorativ
    ssd1306_rect(&oled, 0, 0, 128, 64, true);

    // Titlu centrat ("Smart Home" ~ 10 caractere * 6px = 60px, centrat la (34, 4))
    ssd1306_string(&oled, 34, 4, "Smart Home");

    // Linie separatoare sub titlu
    ssd1306_hline(&oled, 4, 14, 120, true);

    // Modul curent
    if (auto_mode) {
        ssd1306_string(&oled, 16, 24, "Modul: AUTO");
    } else {
        ssd1306_string(&oled, 16, 24, "Modul: MANUAL");
    }

    // Linie separatoare
    ssd1306_hline(&oled, 4, 38, 120, true);

    // Temperatura si praguri
    if (auto_mode && temperatura_valida) {
        char temp_buf[32];
        snprintf(temp_buf, sizeof(temp_buf), "T: %.1f C", ultima_temperatura);
        ssd1306_string(&oled, 4, 42, temp_buf);

        char thr_buf[32];
        snprintf(thr_buf, sizeof(thr_buf), "TRD:%.0f TRN:%.0f", temp_threshold_day, temp_threshold_night);
        ssd1306_string(&oled, 4, 54, thr_buf);
    } else {
        // In modul manual afisam IP-ul dispozitivului
        const char *ip = wifi_server_get_ip();
        if (ip && strcmp(ip, "0.0.0.0") != 0) {
            ssd1306_string(&oled, 4, 42, "IP:");
            ssd1306_string(&oled, 4, 54, ip);
        } else {
            ssd1306_string(&oled, 16, 46, "WiFi: OFF");
        }
    }

    ssd1306_render(&oled);
}

// Ecran special: afisare IP dupa conectare WiFi
void oled_show_wifi_status(const char *line1, const char *line2) {
    ssd1306_clear(&oled);
    ssd1306_rect(&oled, 0, 0, 128, 64, true);
    ssd1306_string(&oled, 34, 4, "Smart Home");
    ssd1306_hline(&oled, 4, 14, 120, true);
    ssd1306_string(&oled, 10, 28, line1);
    if (line2) {
        ssd1306_string(&oled, 10, 44, line2);
    }
    ssd1306_render(&oled);
}

float change_temp(float temp){
    if(temp>=30.0f){
        temp=10.0f;
    }
    else{
        temp+=1.0f;
    }

    return temp;
}

// Controlul automat al releului: porneste/opreste pe baza temperaturii si a pragului activ
void auto_relay_control(float temp, float threshold) {
    mutex_enter_blocking(&state_mutex);
    if (temp > threshold && !relay_state) {
        relay_state = true;
        gpio_put(RELAY_PIN, 0); // LOW = pornit (Active-LOW)
        printf("AUTOMAT: Temperatura %.1f > %.1f°C -> Releu ON\n", temp, threshold);
    }
    else if (temp <= threshold && relay_state) {
        relay_state = false;
        gpio_put(RELAY_PIN, 1); // HIGH = oprit (Active-LOW)
        printf("AUTOMAT: Temperatura %.1f <= %.1f°C -> Releu OFF\n", temp, threshold);
    }
    mutex_exit(&state_mutex);
}

// Core 1 ruleaza citirea de temperatura separat de input-ul butoanelor
void core1_temperature_task() {
    ds18b20_init(DS18B20_PIN);

    while (true) {
        // Citim senzorul de lumina (ADC0) indiferent de mod
        adc_select_input(0);
        uint16_t light_raw = adc_read();

        mutex_enter_blocking(&state_mutex);
        ultima_lumina = light_raw;
        is_daytime = (light_raw < light_threshold);
        mutex_exit(&state_mutex);

        printf("[LUMINA] ADC: %u -> %s\n", light_raw, is_daytime ? "ZI" : "NOAPTE");

        if (!auto_mode) {
            sleep_ms(2000); // In modul manual, citim lumina la 2s
            continue;
        }

        // Selectam pragul de temperatura activ in functie de zi/noapte
        float active_threshold = is_daytime ? temp_threshold_day : temp_threshold_night;

        float temp;
        if (ds18b20_read_temperature(DS18B20_PIN, &temp)) {
            mutex_enter_blocking(&state_mutex);
            ultima_temperatura = temp;
            temperatura_valida = true;
            mutex_exit(&state_mutex);

            printf("Temp: %.2f °C | Lumina: %u (%s) | Prag activ: %.1f°C\n",
                   temp, light_raw, is_daytime ? "ZI" : "NOAPTE", active_threshold);

            // Semnalam Core 0 sa actualizeze OLED-ul (I2C nu e thread-safe)
            oled_needs_update = true;

            // Controlul automat al releului cu pragul selectat
            auto_relay_control(temp, active_threshold);
        } else {
            temperatura_valida = false;
            printf("EROARE: Senzorul DS18B20 nu raspunde!\n");
        }

        // Pauza de 2 secunde intre citiri
        for (int i = 0; i < 20 && auto_mode; i++) {
            sleep_ms(100); 
        }
    }
}

// Core principal, butoane + WiFi
int main() {
    // configurare releu
    gpio_init(RELAY_PIN);
    gpio_set_dir(RELAY_PIN, GPIO_OUT);
    gpio_put(RELAY_PIN, 1); // HIGH = releu OPRIT (Active-LOW)

    stdio_init_all();

    // Asteptam 2 secunde ca USB-ul sa se initializeze complet
    sleep_ms(2000);

    // led-uri
    gpio_init(LED_PIN_MANUAL);
    gpio_set_dir(LED_PIN_MANUAL, GPIO_OUT);

    gpio_init(LED_PIN_AUTO);
    gpio_set_dir(LED_PIN_AUTO, GPIO_OUT);

    // butoane
    gpio_init(BTN_RELAY_PIN);
    gpio_set_dir(BTN_RELAY_PIN, GPIO_IN);
    gpio_pull_up(BTN_RELAY_PIN);

    gpio_init(BTN_MODE_PIN);
    gpio_set_dir(BTN_MODE_PIN, GPIO_IN);
    gpio_pull_up(BTN_MODE_PIN);

    printf("=== Sistem pornit! Mod: MANUAL ===\n");
    printf("Releu: GP%d | Btn Mode: GP%d | Btn Releu: GP%d\n", RELAY_PIN, BTN_MODE_PIN, BTN_RELAY_PIN);
    printf("Senzor DS18B20: GP%d\n", DS18B20_PIN);
    printf("Prag zi: %.1f°C | Prag noapte: %.1f°C | Prag lumina: %u\n",
           temp_threshold_day, temp_threshold_night, light_threshold);

    // Initializare ADC pentru senzorul de lumina KY-018 (GPIO 26 = ADC0)
    adc_init();
    adc_gpio_init(LIGHT_PIN);
    adc_select_input(0);  // ADC0 = GPIO 26
    printf("Senzor lumina KY-018: GP%d (ADC0)\n", LIGHT_PIN);

    // Initializare OLED pe I2C0
    ssd1306_init(&oled, i2c0, OLED_SDA_PIN, OLED_SCL_PIN);
    printf("OLED SSD1306 initializat pe SDA=GP%d, SCL=GP%d\n", OLED_SDA_PIN, OLED_SCL_PIN);

    // led intial
    gpio_put(LED_PIN_MANUAL, 1);

    // ── Initializare mutex si WiFi ──
    mutex_init(&state_mutex);

    oled_show_wifi_status("Conectare WiFi", "...");

    if (wifi_server_init()) {
        char ip_msg[32];
        snprintf(ip_msg, sizeof(ip_msg), "IP: %s", wifi_server_get_ip());
        oled_show_wifi_status("WiFi conectat!", ip_msg);
        printf("=== Server HTTP activ pe %s ===\n", wifi_server_get_ip());
        sleep_ms(3000); // Afisam IP-ul 3 secunde pe OLED
    } else {
        oled_show_wifi_status("WiFi EROARE!", "Verificati config");
        printf("=== EROARE: WiFi nu s-a conectat! ===\n");
        sleep_ms(3000);
    }

    // Afisam starea initiala pe OLED
    oled_update_display();

    multicore_launch_core1(core1_temperature_task);
    printf("Core 1 lansat: asteapta modul AUTO.\n");

    while (true) {
        // Procesam pachetele WiFi
        wifi_server_poll();

        // Actualizam OLED-ul din Core 0 cand Core 1 semnaleaza (I2C thread-safe)
        if (oled_needs_update) {
            oled_needs_update = false;
            oled_update_display();
        }

        // buton schimbare mod
        if (!gpio_get(BTN_MODE_PIN)) {
            sleep_ms(50);
            if (!gpio_get(BTN_MODE_PIN)) {
                mutex_enter_blocking(&state_mutex);
                auto_mode = !auto_mode;

                if (auto_mode) {
                    printf(">>> MOD: AUTO activat -> senzor pornit\n");
                    gpio_put(LED_PIN_MANUAL, 0);
                    gpio_put(LED_PIN_AUTO, 1);
                } else {
                    printf(">>> MOD: MANUAL activat -> senzor in asteptare, oprim releu temporar\n");
                    gpio_put(LED_PIN_MANUAL, 1);
                    gpio_put(LED_PIN_AUTO, 0);
                    
                    relay_state = false;
                    gpio_put(RELAY_PIN, 1); // Oprim releul la revenirea in manual
                    temperatura_valida = false;
                }
                mutex_exit(&state_mutex);

                // Actualizam OLED-ul cu noul mod
                oled_update_display();

                // Asteptam eliberarea butonului (cu WiFi polling)
                while (!gpio_get(BTN_MODE_PIN)) {
                    wifi_server_poll();
                    sleep_ms(10);
                }
                sleep_ms(50);
            }
        }

        // buton manual
        if (!gpio_get(BTN_RELAY_PIN)) {
            sleep_ms(50);
            if (!gpio_get(BTN_RELAY_PIN)) {
                mutex_enter_blocking(&state_mutex);
                if (!auto_mode) {
                    relay_state = !relay_state;
                    if (relay_state) {
                        gpio_put(RELAY_PIN, 0); // Active-LOW: 0 = relay ON
                    } else {
                        gpio_put(RELAY_PIN, 1); // Active-LOW: 1 = relay OFF
                    }
                    printf("BTN_RELAY: apasat -> Releu = %s (manual)\n", relay_state ? "ON" : "OFF");
                } else {
                    // In modul auto, butonul schimba threshold-ul activ (zi sau noapte)
                    if (is_daytime) {
                        temp_threshold_day = change_temp(temp_threshold_day);
                        printf("THRESHOLD ZI: schimbat -> %.1f°C\n", temp_threshold_day);
                    } else {
                        temp_threshold_night = change_temp(temp_threshold_night);
                        printf("THRESHOLD NOAPTE: schimbat -> %.1f°C\n", temp_threshold_night);
                    }
                }
                mutex_exit(&state_mutex);

                // Actualizam OLED-ul dupa schimbarea starii
                oled_update_display();

                // Asteptam eliberarea butonului (cu WiFi polling)
                while (!gpio_get(BTN_RELAY_PIN)) {
                    wifi_server_poll();
                    sleep_ms(10);
                }
                sleep_ms(50);
            }
        }

        sleep_ms(10);
    }

    return 0;
}