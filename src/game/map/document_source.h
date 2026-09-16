#ifndef GAME_MAP_DOCUMENT_SOURCE_H
#define GAME_MAP_DOCUMENT_SOURCE_H

#include <game/map/document/layer.h>
#include <game/map/tile_chunk_cache.h>

#include <memory>

/**
 * Lets the renderer draw a tile layer of the map document.
 *
 * The document keeps what the map file holds, not what it means: a tele layer
 * has a plane of air where its tiles would be, and the indexes that are drawn
 * are worked out from its second plane. That working out is here, because it
 * is a question about drawing rather than about the map - the same layer
 * saved to a file has to come out as it went in.
 *
 * The source holds the version of the layer it was made from, so the version
 * the renderer is drawing cannot be taken away from under it while a later
 * one is being edited. A new version means a new source, and the blocks that
 * changed between the two are the ones to build again - see
 * `CTileStore::ForEachChangedChunk`.
 *
 * @param pLayer The layer to draw, which has to be a tile layer.
 *
 * @return What `CTileChunkCache::Load` wants.
 */
CTileChunkCache::CLayerSource DocumentLayerSource(const std::shared_ptr<const map_document::CLayer> &pLayer);

#endif // GAME_MAP_DOCUMENT_SOURCE_H
