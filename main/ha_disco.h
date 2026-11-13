/* Home Assistant MQTT discovery.
 *
 * Publishes retained config topics so selected datapoints appear as entities
 * without any YAML.  Units come straight from the open3e codec, so the entity
 * carries the same unit the heat pump reports.
 */
#ifndef O3E_HA_DISCO_H
#define O3E_HA_DISCO_H

#include <stdbool.h>

/* Publish (or clear) discovery for the current datapoint selection. */
void ha_disco_publish_all(void);

/* Remove every discovery topic this device published. Called when the feature
 * is switched off, so entities disappear instead of going stale. */
void ha_disco_clear_all(void);

/* How many entities the last discovery run put on the broker, split by kind.
 * A control that never appears is indistinguishable from a read-only
 * datapoint, so the two are worth reporting apart. */
void ha_disco_counts(int *sensors, int *controls);

/* The plant schematic as an image entity: the SVG the web interface stored
 * goes out retained on <base>/plant/svg, and discovery announces it, so a
 * picture-elements card can use image_entity and Home Assistant serves the
 * picture itself -- reachable from outside, where the gateway is not. */
void ha_disco_plant_image(bool clear);

/* Which circuits are moving. The picture goes out again only when this
 * changes; idle circuits get their animation switched off inside the SVG. */
void ha_disco_plant_state(bool compressor_running, bool pump_running);

#endif /* O3E_HA_DISCO_H */
