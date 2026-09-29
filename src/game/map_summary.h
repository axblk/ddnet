#ifndef GAME_MAP_SUMMARY_H
#define GAME_MAP_SUMMARY_H

class CJsonWriter;
class IMap;

/**
 * Writes what a map says about itself as attributes of the object `Writer`
 * is in: its `author`, `version`, `credits` and `license` where it names
 * them, how many server `settings` it brings, the `width` and `height` of
 * its game layer in tiles, and how many `groups`, `layers`, `images`,
 * `sounds` and `envelopes` it has.
 *
 * @param Writer Inside an object.
 * @param Map A loaded map.
 */
void WriteMapSummary(CJsonWriter &Writer, IMap &Map);

#endif
