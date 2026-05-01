#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "ds18b20.h"

// Pinii hardware
#define LED_PIN_AUTO  17
#define LED_PIN_MANUAL 20
#define BTN_MODE_PIN  18  // Schimba modul: manual <-> auto
#define BTN_RELAY_PIN 19  // Porneste/opreste releul (doar in modul manual)
#define RELAY_PIN     16
#define DS18B20_PIN   15

// Pragul de temperatura se poate schimba dinamic
volatile float temp_threshold=23.0f;

volatile bool auto_mode = false;      
volatile bool relay_state = false;
volatile float ultima_temperatura = 0.0f;
volatile bool temperatura_valida = false;

float change_temp(float temp){
    if(temp==30.0){
        temp=10.0f;
    }
    else{
        temp+=1.0f;
    }

    return temp;
}

// Core 1 ruleaza citirea de temperatura separat de input-ul butoanelor
void core1_temperature_task() {
    ds18b20_init(DS18B20_PIN);

    while (true) {
        if (!auto_mode) {
            sleep_ms(100); // In modul manual, doar asteptam
            continue;
        }

        float temp;
        if (ds18b20_read_temperature(DS18B20_PIN, &temp)) {
            ultima_temperatura = temp;
            temperatura_valida = true;
            printf("Temperatura: %.2f °C\n", temp);

            // Controlul automat al releului pe baza temperaturii
            if (temp > temp_threshold && !relay_state) {
                relay_state = true;
                gpio_put(RELAY_PIN, 0); // LOW = pornit (Active-LOW)
                printf("AUTOMAT: Temperatura > %.1f°C -> Releu ON\n", temp_threshold);
            }
            else if (temp <= temp_threshold && relay_state) {
                relay_state = false;
                gpio_put(RELAY_PIN, 1); // HIGH = oprit (Active-LOW)
                printf("AUTOMAT: Temperatura <= %.1f°C -> Releu OFF\n", temp_threshold);
            }
        } else {
            temperatura_valida = false;
            printf("EROARE: Senzorul DS18B20 nu raspunde!\n");
        }

        // Pauza de 2 secunde intre citiri pentru a rezolva bug-ul blocarii microprocesorului la schimbari rapide intre moduri
        for (int i = 0; i < 20 && auto_mode; i++) {
            sleep_ms(100); 
        }
    }
}

// Core principal, butoane
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
    printf("Senzor DS18B20: GP%d | Prag temperatura: %.1f°C\n", DS18B20_PIN, temp_threshold);

    // led intial
    gpio_put(LED_PIN_MANUAL, 1);

    printf("Astept apasari...\n");

    multicore_launch_core1(core1_temperature_task);
    printf("Core 1 lansat: asteapta modul AUTO.\n");

    while (true) {

        // buton schimbare mod
        if (!gpio_get(BTN_MODE_PIN)) {
            sleep_ms(50);
            if (!gpio_get(BTN_MODE_PIN)) {
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

                // Asteptam eliberarea butonului
                while (!gpio_get(BTN_MODE_PIN)) {
                    sleep_ms(10);
                }
                sleep_ms(50);
            }
        }

        // buton manual
        if (!gpio_get(BTN_RELAY_PIN)) {
            sleep_ms(50);
            if (!gpio_get(BTN_RELAY_PIN)) {
                if (!auto_mode) {
                    relay_state = !relay_state;
                    if (relay_state) {
                        gpio_put(RELAY_PIN, 0); // Active-LOW: 0 = relay ON
                    } else {
                        gpio_put(RELAY_PIN, 1); // Active-LOW: 1 = relay OFF
                    }
                    printf("BTN_RELAY: apasat -> Releu = %s (manual)\n", relay_state ? "ON" : "OFF");
                } else {
                    temp_threshold = change_temp(temp_threshold);
                    printf("THRESHOLD: schimbat -> %.1f°C\n", temp_threshold);
                }

                // Asteptam eliberarea butonului
                while (!gpio_get(BTN_RELAY_PIN)) {
                    sleep_ms(10);
                }
                sleep_ms(50);
            }
        }

        sleep_ms(10);
    }

    return 0;
}