#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/cyw43_arch.h"
#include "lwip/ip4_addr.h"
#include "ds18b20.h"
#include "ssd1306.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "secrets.h"
#include "http_server.h"

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

// Stocare IP WiFi global pentru afisare pe OLED
char global_ip_str[20] = "";
bool wifi_connected = false;
volatile bool oled_needs_update = false;

// Flag-uri setate din intreruperea GPIO (butoane)
volatile bool btn_mode_pressed = false;
volatile bool btn_relay_pressed = false;

// Timestamp-uri pentru debounce (in microsecunde)
#define DEBOUNCE_US 200000  // 200ms debounce
static volatile uint64_t last_mode_irq_time = 0;
static volatile uint64_t last_relay_irq_time = 0;

// Citim 8 esantioane prin DMA si le mediem pentru a reduce zgomotul senzorului de lumina
#define ADC_DMA_SAMPLES 8
static uint16_t adc_dma_buffer[ADC_DMA_SAMPLES];
static int adc_dma_channel = -1;

// Initializare ADC FIFO + canal DMA
void adc_dma_init() {
    // Configuram ADC FIFO: scriem in FIFO, activam DREQ pentru DMA
    adc_fifo_setup(
        true,   // Scriem rezultatele in FIFO
        true,   // Activam DMA data request (DREQ)
        1,      // Prag DREQ: genereaza cerere DMA dupa fiecare esantion
        false,  // Nu shiftam rezultatul (pastram 12 biti)
        false   // Nu folosim mod 8-bit
    );
    adc_set_clkdiv(0); // Viteza maxima de esantionare (~500ksps)

    // Revendicam un canal DMA liber
    adc_dma_channel = dma_claim_unused_channel(true);

    // Configurare canal DMA
    dma_channel_config cfg = dma_channel_get_default_config(adc_dma_channel);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);  // Transferuri de 16 biti (ADC = 12 biti)
    channel_config_set_read_increment(&cfg, false);            // Citim mereu de la aceeasi adresa (ADC FIFO)
    channel_config_set_write_increment(&cfg, true);            // Scriem secvential in buffer
    channel_config_set_dreq(&cfg, DREQ_ADC);                   // Sincronizare cu ADC (un transfer per esantion)

    dma_channel_configure(
        adc_dma_channel,
        &cfg,
        adc_dma_buffer,       // Destinatie: buffer-ul nostru
        &adc_hw->fifo,        // Sursa: registrul FIFO al ADC-ului
        ADC_DMA_SAMPLES,      // Numar de transferuri
        false                 // Nu pornim inca
    );

    printf("[DMA] Canal %d configurat pentru ADC (%d esantioane per citire)\n",
           adc_dma_channel, ADC_DMA_SAMPLES);
}

// Citeste ADC prin DMA: colecteaza N esantioane si returneaza media
uint16_t adc_dma_read_avg() {
    // Golim FIFO-ul de date vechi
    adc_fifo_drain();

    // Reconfiguram DMA pentru un nou transfer
    dma_channel_set_write_addr(adc_dma_channel, adc_dma_buffer, false);
    dma_channel_set_trans_count(adc_dma_channel, ADC_DMA_SAMPLES, true); // true = start

    // Pornim ADC in mod free-running
    adc_run(true);

    // Asteptam ca DMA sa termine transferul tuturor esantioanelor
    dma_channel_wait_for_finish_blocking(adc_dma_channel);

    // Oprim ADC si golim FIFO-ul
    adc_run(false);
    adc_fifo_drain();

    // Calculam media esantioanelor pentru a reduce zgomotul
    uint32_t sum = 0;
    for (int i = 0; i < ADC_DMA_SAMPLES; i++) {
        sum += adc_dma_buffer[i];
    }
    return (uint16_t)(sum / ADC_DMA_SAMPLES);
}

// Display OLED global
ssd1306_t oled;

void trigger_oled_update() {
    oled_needs_update = true;
}

// Actualizeaza display-ul OLED cu modul curent
void oled_update_display() {
    ssd1306_clear(&oled);

    // Chenar decorativ
    ssd1306_rect(&oled, 0, 0, 128, 64, true);

    // Titlu centrat sau IP-ul conexiunii
    if (wifi_connected) {
        ssd1306_string(&oled, 4, 4, global_ip_str);
    } else {
        ssd1306_string(&oled, 34, 4, "Smart Home");
    }

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
    if (temperatura_valida) {
        char temp_buf[32];
        snprintf(temp_buf, sizeof(temp_buf), "T: %.1f C", ultima_temperatura);
        ssd1306_string(&oled, 4, 42, temp_buf);

        if (auto_mode) {
            char thr_buf[32];
            snprintf(thr_buf, sizeof(thr_buf), "TRD:%.0f TRN:%.0f", temp_threshold_day, temp_threshold_night);
            ssd1306_string(&oled, 4, 54, thr_buf);
        } else {
            ssd1306_string(&oled, 4, 54, "Releu manual");
        }
    } else {
        ssd1306_string(&oled, 16, 46, "Temp: ERR/OFF");
    }

    ssd1306_render(&oled);
}

float change_temp(float temp){
    if(temp==30.0){
        temp=10.0f;
    }
    else{
        temp+=1.0f;
    }

    return temp;
}

// Controlul automat al releului: porneste/opreste pe baza temperaturii si a pragului activ
void auto_relay_control(float temp, float threshold) {
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
}

// Core 1 ruleaza citirea de temperatura separat de input-ul butoanelor
void core1_temperature_task() {
    ds18b20_init(DS18B20_PIN);

    while (true) {
        // Citim senzorul de lumina prin DMA (media a 8 esantioane)
        adc_select_input(0);
        uint16_t light_raw = adc_dma_read_avg();
        ultima_lumina = light_raw;
        is_daytime = (light_raw < light_threshold);
        printf("[LUMINA] ADC+DMA: %u (media %d esantioane) -> %s\n",
               light_raw, ADC_DMA_SAMPLES, is_daytime ? "ZI" : "NOAPTE");

        // Selectam pragul de temperatura activ in functie de zi/noapte
        float active_threshold = is_daytime ? temp_threshold_day : temp_threshold_night;

        float temp;
        if (ds18b20_read_temperature(DS18B20_PIN, &temp)) {
            ultima_temperatura = temp;
            temperatura_valida = true;
            printf("Temp: %.2f °C | Lumina: %u (%s) | Prag activ: %.1f°C\n",
                   temp, light_raw, is_daytime ? "ZI" : "NOAPTE", active_threshold);
            oled_needs_update = true;

            if (auto_mode) {
                // Controlul automat al releului cu pragul selectat
                auto_relay_control(temp, active_threshold);
            }
        } else {
            temperatura_valida = false;
            printf("EROARE: Senzorul DS18B20 nu raspunde!\n");
            oled_needs_update = true;
        }

        // Pauza de 2 secunde intre citiri
        sleep_ms(2000);
    }
}

// callback http
void set_relay_callback(bool on) {
    relay_state = on;
    gpio_put(RELAY_PIN, on ? 0 : 1);  // Active-LOW: 0 = relay ON
    printf("[HTTP] Releu setat: %s\n", on ? "ON" : "OFF");
    oled_needs_update = true;
}

void set_mode_callback(bool auto_on) {
    auto_mode = auto_on;
    if (auto_on) {
        printf("[HTTP] MOD: AUTO activat\n");
        gpio_put(LED_PIN_MANUAL, 0);
        gpio_put(LED_PIN_AUTO, 1);
    } else {
        printf("[HTTP] MOD: MANUAL activat\n");
        gpio_put(LED_PIN_MANUAL, 1);
        gpio_put(LED_PIN_AUTO, 0);
        relay_state = false;
        gpio_put(RELAY_PIN, 1); // Oprim releul la revenirea in manual
    }
    oled_needs_update = true;
}

// Callback intrerupere GPIO pentru butoane (falling edge = buton apasat)
void gpio_irq_callback(uint gpio, uint32_t events) {
    uint64_t now = time_us_64();

    if (gpio == BTN_MODE_PIN && (events & GPIO_IRQ_EDGE_FALL)) {
        // Debounce: ignoram daca a trecut mai putin de 200ms de la ultima intrerupere
        if (now - last_mode_irq_time > DEBOUNCE_US) {
            last_mode_irq_time = now;
            btn_mode_pressed = true;
        }
    }

    if (gpio == BTN_RELAY_PIN && (events & GPIO_IRQ_EDGE_FALL)) {
        if (now - last_relay_irq_time > DEBOUNCE_US) {
            last_relay_irq_time = now;
            btn_relay_pressed = true;
        }
    }
}

// Core principal
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

    // butoane cu intreruperi GPIO (falling edge = apasare)
    gpio_init(BTN_RELAY_PIN);
    gpio_set_dir(BTN_RELAY_PIN, GPIO_IN);
    gpio_pull_up(BTN_RELAY_PIN);

    gpio_init(BTN_MODE_PIN);
    gpio_set_dir(BTN_MODE_PIN, GPIO_IN);
    gpio_pull_up(BTN_MODE_PIN);

    // Activam intreruperile pe ambele butoane (edge fall = buton apasat)
    gpio_set_irq_enabled_with_callback(BTN_MODE_PIN, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_callback);
    gpio_set_irq_enabled(BTN_RELAY_PIN, GPIO_IRQ_EDGE_FALL, true);
    printf("Intreruperi GPIO activate pe BTN_MODE (GP%d) si BTN_RELAY (GP%d)\n", BTN_MODE_PIN, BTN_RELAY_PIN);

    printf("=== Sistem pornit! Mod: MANUAL ===\n");
    printf("Releu: GP%d | Btn Mode: GP%d | Btn Releu: GP%d\n", RELAY_PIN, BTN_MODE_PIN, BTN_RELAY_PIN);
    printf("Senzor DS18B20: GP%d\n", DS18B20_PIN);
    printf("Prag zi: %.1f°C | Prag noapte: %.1f°C | Prag lumina: %u\n",
           temp_threshold_day, temp_threshold_night, light_threshold);

    // Initializare ADC + DMA pentru senzorul de lumina KY-018 (GPIO 26 = ADC0)
    adc_init();
    adc_gpio_init(LIGHT_PIN);
    adc_select_input(0);  // ADC0 = GPIO 26
    adc_dma_init();       // Configuram DMA pentru citiri ADC
    printf("Senzor lumina KY-018: GP%d (ADC0) cu DMA\n", LIGHT_PIN);

    // Initializare OLED pe I2C0
    ssd1306_init(&oled, i2c0, OLED_SDA_PIN, OLED_SCL_PIN);
    printf("OLED SSD1306 initializat pe SDA=GP%d, SCL=GP%d\n", OLED_SDA_PIN, OLED_SCL_PIN);

    // led intial
    gpio_put(LED_PIN_MANUAL, 1);

    // Afisam starea initiala pe OLED
    oled_update_display();

    // wifi
    bool wifi_ok = false;
    if (cyw43_arch_init()) {
        printf("[WIFI] EROARE: cyw43_arch_init a esuat!\n");
        ssd1306_clear(&oled);
        ssd1306_string(&oled, 4, 28, "WiFi INIT FAIL");
        ssd1306_render(&oled);
        sleep_ms(3000);
    } else {
        printf("[WIFI] CYW43 initializat OK\n");
        cyw43_arch_enable_sta_mode();

        // Incercam conectarea de 3 ori
        const int max_retries = 3;
        for (int attempt = 1; attempt <= max_retries && !wifi_ok; attempt++) {
            printf("[WIFI] Tentativa %d/%d - Conectare la '%s'...\n",
                   attempt, max_retries, WIFI_SSID);

            // Afisam pe OLED tentativa curenta
            ssd1306_clear(&oled);
            ssd1306_string(&oled, 10, 12, "Conectare WiFi");
            ssd1306_string(&oled, 10, 28, WIFI_SSID);
            char attempt_buf[32];
            snprintf(attempt_buf, sizeof(attempt_buf), "Tentativa %d/%d...", attempt, max_retries);
            ssd1306_string(&oled, 10, 44, attempt_buf);
            ssd1306_render(&oled);

            int wifi_err = cyw43_arch_wifi_connect_timeout_ms(
                WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 10000
            );

            if (wifi_err == 0) {
                wifi_ok = true;
                // Obtinem IP-ul
                struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
                char ip_str[20];
                snprintf(ip_str, sizeof(ip_str), "%s", ip4addr_ntoa(netif_ip4_addr(n)));
                printf("[WIFI] Conectat! IP: %s\n", ip_str);

                // Salvam IP-ul in variabila globala
                snprintf(global_ip_str, sizeof(global_ip_str), "IP:%s", ip_str);
                wifi_connected = true;

                // Afisam IP-ul pe OLED
                ssd1306_clear(&oled);
                ssd1306_string(&oled, 10, 12, "WiFi Conectat!");
                ssd1306_string(&oled, 4, 32, "IP:");
                ssd1306_string(&oled, 28, 32, ip_str);
                ssd1306_render(&oled);
                sleep_ms(3000);
            } else {
                printf("[WIFI] Tentativa %d esuata (cod: %d)\n", attempt, wifi_err);
                if (attempt < max_retries) {
                    sleep_ms(2000); // Pauza intre tentative
                }
            }
        }

        if (!wifi_ok) {
            printf("[WIFI] EROARE: Nu s-a putut conecta dupa %d tentative!\n", max_retries);
            ssd1306_clear(&oled);
            ssd1306_string(&oled, 4, 20, "WiFi EROARE!");
            ssd1306_string(&oled, 4, 36, "Fara conexiune");
            ssd1306_render(&oled);
            sleep_ms(3000);
        }
    }

    // pornire server
    if (wifi_ok) {
        http_system_state_t http_state = {
            .temperature       = &ultima_temperatura,
            .temperatura_valida = &temperatura_valida,
            .light             = &ultima_lumina,
            .relay_state       = &relay_state,
            .auto_mode         = &auto_mode,
            .is_daytime        = &is_daytime,
            .threshold_day     = &temp_threshold_day,
            .threshold_night   = &temp_threshold_night,
            .light_threshold   = &light_threshold,
            .set_relay         = set_relay_callback,
            .set_mode          = set_mode_callback,
            .update_display    = trigger_oled_update,
        };
        if (http_server_init(&http_state)) {
            printf("[HTTP] Server pornit pe portul 80\n");
        } else {
            printf("[HTTP] EROARE: serverul nu a pornit!\n");
        }
    }

    // Revenim la afisajul normal
    oled_update_display();

    multicore_launch_core1(core1_temperature_task);
    printf("Core 1 lansat: asteapta modul AUTO.\n");

    while (true) {

        // Buton schimbare mod (flag setat din intrerupere GPIO)
        if (btn_mode_pressed) {
            btn_mode_pressed = false;

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
            }

            oled_needs_update = true;
        }

        // Buton releu/prag (flag setat din intrerupere GPIO)
        if (btn_relay_pressed) {
            btn_relay_pressed = false;

            if (!auto_mode) {
                relay_state = !relay_state;
                if (relay_state) {
                    gpio_put(RELAY_PIN, 0); // Active-LOW: 0 = relay ON
                } else {
                    gpio_put(RELAY_PIN, 1); // Active-LOW: 1 = relay OFF
                }
                printf("BTN_RELAY: apasat -> Releu = %s (manual)\n", relay_state ? "ON" : "OFF");
                oled_needs_update = true;
            } else {
                // In modul auto, butonul schimba threshold-ul activ (zi sau noapte)
                if (is_daytime) {
                    temp_threshold_day = change_temp(temp_threshold_day);
                    printf("THRESHOLD ZI: schimbat -> %.1f°C\n", temp_threshold_day);
                } else {
                    temp_threshold_night = change_temp(temp_threshold_night);
                    printf("THRESHOLD NOAPTE: schimbat -> %.1f°C\n", temp_threshold_night);
                }
                oled_needs_update = true;
            }
        }

        if (oled_needs_update) {
            oled_update_display();
            oled_needs_update = false;
        }

        sleep_ms(10);
    }

    return 0;
}