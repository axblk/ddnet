#ifndef ENGINE_GFX_PNG_DECODE_ZLIB_H
#define ENGINE_GFX_PNG_DECODE_ZLIB_H

class CByteBufferReader;
class CImageInfo;

/**
 * Reads a PNG with zlib alone: `CImageLoader::LoadPng` in browser builds with
 * WEB_PNG, elsewhere only compared with libpng by the tests.
 *
 * The result is what png_decode_libpng.cpp gives: the same pixels, the same
 * files refused with the same errors.
 *
 * @param Reader The bytes of the file.
 * @param pContextName The name of the file, for messages.
 * @param Image Takes the image on success, untouched otherwise.
 * @param PngliteIncompatible Set to the `CImageLoader::PNGLITE_*` flags of what
 * pnglite could not read.
 *
 * @return `true` on success.
 */
bool LoadPngWithZlib(CByteBufferReader &Reader, const char *pContextName, CImageInfo &Image, int &PngliteIncompatible);

#endif
