#include "plant_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "ha_disco.h"
#include "mqtt_pub.h"
#include "poller.h"

static const char *TAG = "plant";

#define PLANT_PERIOD_MS   30000
#define DID_COMPRESSOR    2346   /* CompressorSpeedPercent, on the outdoor unit */
#define DID_PUMP          381    /* CentralHeatingPump {State, TargetValue, Actual} */

/* The controller addresses come from the probe the web interface stored:
 * "roles": {"indoor": "0x680", "outdoor": "0x68C"}. */
static bool plant_roles(uint16_t *indoor, uint16_t *outdoor)
{
    char *raw = app_config_read_file(CFG_PLANT_PATH);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!root) {
        return false;
    }
    const cJSON *roles = cJSON_GetObjectItemCaseSensitive(root, "roles");
    const cJSON *ji = roles ? cJSON_GetObjectItemCaseSensitive(roles, "indoor") : NULL;
    const cJSON *jo = roles ? cJSON_GetObjectItemCaseSensitive(roles, "outdoor") : NULL;
    bool ok = cJSON_IsString(ji) && cJSON_IsString(jo);
    if (ok) {
        *indoor = (uint16_t)strtol(ji->valuestring, NULL, 0);
        *outdoor = (uint16_t)strtol(jo->valuestring, NULL, 0);
        ok = *indoor && *outdoor;
    }
    cJSON_Delete(root);
    return ok;
}

/* One decoded value as a number: a bare number, or `field` of an object. */
static bool read_number(uint16_t ecu, uint16_t did, const char *field, double *out)
{
    char err[96];
    char *json = poller_read_now(ecu, did, err, sizeof(err));
    if (!json) {
        return false;
    }
    cJSON *v = cJSON_Parse(json);
    free(json);
    if (!v) {
        return false;
    }
    const cJSON *n = field && cJSON_IsObject(v) ? cJSON_GetObjectItemCaseSensitive(v, field) : v;
    bool ok = cJSON_IsNumber(n);
    if (ok) {
        *out = n->valuedouble;
    }
    cJSON_Delete(v);
    return ok;
}

static void plant_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PLANT_PERIOD_MS));
        if (!mqtt_pub_connected()) {
            continue;
        }
        /* No picture stored, nothing to keep in step. */
        FILE *f = fopen(CFG_WWW_DIR "/plant.svg", "rb");
        if (!f) {
            continue;
        }
        fclose(f);
        uint16_t indoor, outdoor;
        if (!plant_roles(&indoor, &outdoor)) {
            continue;
        }
        double comp = 0, pump = 0;
        bool have_comp = read_number(outdoor, DID_COMPRESSOR, NULL, &comp);
        bool have_pump = read_number(indoor, DID_PUMP, "Actual", &pump);
        if (!have_comp && !have_pump) {
            continue;
        }
        /* A value that could not be read keeps the circuit moving: a stale
         * picture that moves is less misleading than one that stops on a
         * bus hiccup. */
        ha_disco_plant_state(have_comp ? comp > 0 : true, have_pump ? pump > 0 : true);
    }
}

void plant_image_start(void)
{
    if (xTaskCreate(plant_task, "plant", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the plant image task");
    }
}
