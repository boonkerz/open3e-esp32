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

/* Read the stored record of what has been announced again on next use. For
 * tests, which swap it underneath the module. */
void ha_disco_ledger_reload(void);

/* The plant schematic as an image entity: the SVG the web interface stored
 * goes out retained on <base>/plant/svg, and discovery announces it, so a
 * picture-elements card can use image_entity and Home Assistant serves the
 * picture itself -- reachable from outside, where the gateway is not. */
void ha_disco_plant_image(bool clear);

/* Which circuits are moving. The picture goes out again only when this
 * changes; idle circuits get their animation switched off inside the SVG. */
void ha_disco_plant_state(bool compressor_running, bool pump_running);
/* A boiler's diverter valve: 0 heating, 2 hot water, anything else unknown or
 * in between. Only the way in use moves in the picture. */
void ha_disco_plant_valve(int position);

/* Boiler: whether the burner is on (1), off (0) or unknown (-1). The flame
 * in the published picture goes grey while it is off. */
void ha_disco_plant_burner(int burning);

/* The picture with the values already in it, rendered by plant_image.c:
 * retained on <base>/plant/live and announced as a second image entity.
 * For dashboards that cannot overlay sensors on a background, Grafana among
 * them. The idle-circuit style is applied here like for the background. */
void ha_disco_plant_live(const char *svg);

/* A circuit's heating curve as a picture (heat_curve.c), next to the two
 * numbers that set it: retained on <base>/curve/<n>, Base64-coded on
 * <base>/curve/<n>/b64, announced as the image entity "Heizkennlinie <name>".
 * `circuit` counts from 1. A NULL picture retracts all three. */
/* False when nothing went out (MQTT off or not connected): the caller keeps
 * its record of what the broker holds as it was. */
bool ha_disco_curve_image(int circuit, const char *name, const char *svg);

#endif /* O3E_HA_DISCO_H */
