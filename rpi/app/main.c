#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <mosquitto.h>
#include <pthread.h>

#define UART_DEV "/dev/ttyAMA2"
#define DEVICE_ID "rpi_env01"

// ===== 共享資料 =====
int motion = 0;
int temp = 0, hum = 0, air = 0;
int data_ready = 0;

time_t last_uart_time = 0;
int uart_initialized = 0;

pthread_mutex_t lock;

// ===== PIR thread =====
void *pir_thread(void *arg)
{
    int pir_fd = open("/dev/pir_dev", O_RDONLY);
    char pir_buf[10];

    int last_raw = 0;
    int stable_count = 0;
    int state = 0;

    time_t falling_edge_time = 0;
    int gpio_was_high = 0;

    time_t start_time = time(NULL);

    while (1)
    {
        memset(pir_buf, 0, sizeof(pir_buf));
        read(pir_fd, pir_buf, sizeof(pir_buf));

        int curr = (pir_buf[0] == '1') ? 1 : 0;
        time_t now = time(NULL);

        if (now - start_time < 5)
        {
            motion = 0;
            usleep(100000);
            continue;
        }

        if (curr == last_raw)
            stable_count++;
        else
            stable_count = 0;
        last_raw = curr;

        if (stable_count < 2)
        {
            usleep(100000);
            continue;
        }

        if (curr == 1 && gpio_was_high == 0)
        {
            gpio_was_high = 1;
            falling_edge_time = 0;
        }

        if (curr == 0 && gpio_was_high == 1)
        {
            gpio_was_high = 0;
            falling_edge_time = now;
        }

        if (curr == 1)
        {
            state = 1;
        }
        else if (falling_edge_time > 0 && (now - falling_edge_time < 2))
        {
            state = 1;
        }
        else
        {
            state = 0;
        }

        pthread_mutex_lock(&lock);
        motion = state;
        pthread_mutex_unlock(&lock);

        usleep(100000);
    }
}

// ===== UART thread =====
void *uart_thread(void *arg)
{
    int uart_fd = open(UART_DEV, O_RDONLY | O_NOCTTY);
    char uart_buf[100];

    while (1)
    {
        memset(uart_buf, 0, sizeof(uart_buf));
        int len = read(uart_fd, uart_buf, sizeof(uart_buf) - 1);

        if (len > 0)
        {
            uart_buf[len] = '\0';

            last_uart_time = time(NULL);
            uart_initialized = 1;

            if (strchr(uart_buf, '<') && strchr(uart_buf, '>'))
            {
                int t, h, a;

                if (sscanf(uart_buf, "<temp:%d,hum:%d,air:%d>", &t, &h, &a) == 3)
                {
                    if (t > 0 && t < 100 && h >= 0 && h <= 100 && a > 0 && a < 1000)
                    {
                        pthread_mutex_lock(&lock);
                        temp = t;
                        hum = h;
                        air = a;
                        data_ready = 1;
                        pthread_mutex_unlock(&lock);
                    }
                }
            }
        }
    }
}

// ===== MQTT thread =====
void *mqtt_thread(void *arg)
{
    struct mosquitto *mosq;

    mosquitto_lib_init();
    mosq = mosquitto_new(NULL, true, NULL);

    if (!mosq)
    {
        printf("MQTT init failed\n");
        return NULL;
    }

    mosquitto_connect(mosq, "localhost", 1883, 60);

    char *topic_sensor = "device/" DEVICE_ID "/sensor";
    char *topic_event  = "device/" DEVICE_ID "/event";

    int last_occupancy = -1;  // 用來偵測 occupancy 變化，觸發 event

    while (1)
    {
        mosquitto_loop(mosq, 0, 1);

        if (!uart_initialized)
        {
            sleep(1);
            continue;
        }

        pthread_mutex_lock(&lock);
        int m = motion;
        int t = temp;
        int h = hum;
        int a = air;
        pthread_mutex_unlock(&lock);

        time_t now = time(NULL);
        int disconnected = (now - last_uart_time > 5);

        // ===== 發布 sensor topic =====
        char sensor_msg[300];

        if (disconnected)
        {
            // UART 斷線：只回報 occupancy，其餘欄位省略或用 null
            snprintf(sensor_msg, sizeof(sensor_msg),
                "{"
                "\"device_id\":\"%s\","
                "\"timestamp\":%ld,"
                "\"data\":{"
                    "\"temperature_c\":null,"
                    "\"humidity_percent\":null,"
                    "\"air_quality_index\":null,"
                    "\"occupancy\":%s"
                "}"
                "}",
                DEVICE_ID, (long)now,
                m ? "true" : "false");
        }
        else
        {
            snprintf(sensor_msg, sizeof(sensor_msg),
                "{"
                "\"device_id\":\"%s\","
                "\"timestamp\":%ld,"
                "\"data\":{"
                    "\"temperature_c\":%d,"
                    "\"humidity_percent\":%d,"
                    "\"air_quality_index\":%d,"
                    "\"occupancy\":%s"
                "}"
                "}",
                DEVICE_ID, (long)now,
                t, h, a,
                m ? "true" : "false");
        }

        mosquitto_publish(mosq, NULL, topic_sensor, strlen(sensor_msg), sensor_msg, 0, false);
        printf("[sensor] %s\n", sensor_msg);

        // ===== 偵測 occupancy 變化，發布 event topic =====
        if (m != last_occupancy && last_occupancy != -1)
        {
            char event_msg[200];
            snprintf(event_msg, sizeof(event_msg),
                "{"
                "\"device_id\":\"%s\","
                "\"timestamp\":%ld,"
                "\"event\":{"
                    "\"type\":\"occupancy_change\","
                    "\"value\":%s"
                "}"
                "}",
                DEVICE_ID, (long)now,
                m ? "true" : "false");

            mosquitto_publish(mosq, NULL, topic_event, strlen(event_msg), event_msg, 0, false);
            printf("[event]  %s\n", event_msg);
        }
        last_occupancy = m;

        sleep(2);
    }
}

// ===== main =====
int main()
{
    pthread_t t1, t2, t3;

    pthread_mutex_init(&lock, NULL);

    pthread_create(&t1, NULL, pir_thread, NULL);
    pthread_create(&t2, NULL, uart_thread, NULL);
    pthread_create(&t3, NULL, mqtt_thread, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    return 0;
}
