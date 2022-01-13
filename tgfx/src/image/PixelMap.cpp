/////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Tencent is pleased to support the open source community by making libpag available.
//
//  Copyright (C) 2021 THL A29 Limited, a Tencent company. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  unless required by applicable law or agreed to in writing, software distributed under the
//  license is distributed on an "as is" basis, without warranties or conditions of any kind,
//  either express or implied. see the license for the specific language governing permissions
//  and limitations under the license.
//
/////////////////////////////////////////////////////////////////////////////////////////////////

#include "PixelMap.h"
#include "ImageInfo.h"
#include "platform/Platform.h"
#include "skcms.h"

namespace pag {

static inline void* AddOffset(void* pixels, size_t offset) {
  return reinterpret_cast<uint8_t*>(pixels) + offset;
}

static inline const void* AddOffset(const void* pixels, size_t offset) {
  return reinterpret_cast<const uint8_t*>(pixels) + offset;
}

static void CopyRectMemory(const void* src, size_t srcRB, void* dst, size_t dstRB,
                           size_t trimRowBytes, int rowCount) {
  if (trimRowBytes == dstRB && trimRowBytes == srcRB) {
    memcpy(dst, src, trimRowBytes * rowCount);
    return;
  }
  for (int i = 0; i < rowCount; i++) {
    memcpy(dst, src, trimRowBytes);
    dst = AddOffset(dst, dstRB);
    src = AddOffset(src, srcRB);
  }
}

static const std::unordered_map<ColorType, gfx::skcms_PixelFormat> ColorMapper{
    {ColorType::RGBA_8888, gfx::skcms_PixelFormat::skcms_PixelFormat_RGBA_8888},
    {ColorType::BGRA_8888, gfx::skcms_PixelFormat::skcms_PixelFormat_BGRA_8888},
    {ColorType::ALPHA_8, gfx::skcms_PixelFormat::skcms_PixelFormat_A_8},
};

static const std::unordered_map<AlphaType, gfx::skcms_AlphaFormat> AlphaMapper{
    {AlphaType::Unpremultiplied, gfx::skcms_AlphaFormat::skcms_AlphaFormat_Unpremul},
    {AlphaType::Premultiplied, gfx::skcms_AlphaFormat::skcms_AlphaFormat_PremulAsEncoded},
    {AlphaType::Opaque, gfx::skcms_AlphaFormat::skcms_AlphaFormat_Opaque},
};

static void ConvertPixels(const ImageInfo& srcInfo, const void* srcPixels, const ImageInfo& dstInfo,
                          void* dstPixels) {
  if (srcInfo.colorType() == dstInfo.colorType() && srcInfo.alphaType() == dstInfo.alphaType()) {
    CopyRectMemory(srcPixels, srcInfo.rowBytes(), dstPixels, dstInfo.rowBytes(),
                   dstInfo.minRowBytes(), dstInfo.height());
    return;
  }
  auto srcFormat = ColorMapper.at(srcInfo.colorType());
  auto srcAlpha = AlphaMapper.at(srcInfo.alphaType());
  auto dstFormat = ColorMapper.at(dstInfo.colorType());
  auto dstAlpha = AlphaMapper.at(dstInfo.alphaType());
  auto width = dstInfo.width();
  auto height = dstInfo.height();
  for (int i = 0; i < height; i++) {
    gfx::skcms_Transform(srcPixels, srcFormat, srcAlpha, nullptr, dstPixels, dstFormat, dstAlpha,
                         nullptr, width);
    dstPixels = AddOffset(dstPixels, dstInfo.rowBytes());
    srcPixels = AddOffset(srcPixels, srcInfo.rowBytes());
  }
}

bool PixelMap::readPixels(const ImageInfo& dstInfo, void* dstPixels, int srcX, int srcY) const {
  if (_pixels == nullptr || dstPixels == nullptr) {
    return false;
  }
  auto imageInfo = dstInfo.makeIntersect(-srcX, -srcY, _info.width(), _info.height());
  if (imageInfo.isEmpty()) {
    return false;
  }
  auto srcPixels = _info.computeOffset(_pixels, srcX, srcY);
  dstPixels = imageInfo.computeOffset(dstPixels, -srcX, -srcY);
  ConvertPixels(_info, srcPixels, imageInfo, dstPixels);
  return true;
}

void Trace(const PixelMap& pixelMap, const std::string& tag) {
  Platform::Current()->traceImage(pixelMap, tag);
}

}  // namespace pag