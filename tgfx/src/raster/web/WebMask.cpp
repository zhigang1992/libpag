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

#include "WebMask.h"

#include "WebTypeface.h"
#include "gpu/opengl/GLTexture.h"
#include "WebTextBlob.h"

using namespace emscripten;

namespace pag {
std::shared_ptr<Mask> Mask::Make(int width, int height) {
  auto webMaskClass = val::module_property("WebMask");
  if (!webMaskClass.as<bool>()) {
    return nullptr;
  }
  auto webMask = webMaskClass.new_(width, height);
  if (!webMask.as<bool>()) {
    return nullptr;
  }
  return std::make_shared<WebMask>(width, height, webMask);
}

std::shared_ptr<Texture> WebMask::makeTexture(Context* context) const {
  auto texture = Texture::MakeAlpha(context, width(), height(), nullptr, 0);
  if (texture == nullptr) {
    return nullptr;
  }
  auto& glInfo = std::static_pointer_cast<GLTexture>(texture)->getGLInfo();
  const auto* gl = GLContext::Unwrap(context);
  gl->bindTexture(glInfo.target, glInfo.id);
  webMask.call<void>("update", val::module_property("GL"));
  return texture;
}

static void Iterator(PathVerb verb, const Point points[4], void* info) {
  auto path2D = reinterpret_cast<val*>(info);
  switch (verb) {
    case PathVerb::Move:
      path2D->call<void>("moveTo", points[0].x, points[0].y);
      break;
    case PathVerb::Line:
      path2D->call<void>("lineTo", points[1].x, points[1].y);
      break;
    case PathVerb::Quad:
      path2D->call<void>("quadraticCurveTo", points[1].x, points[1].y, points[2].x, points[2].y);
      break;
    case PathVerb::Cubic:
      path2D->call<void>("bezierCurveTo", points[1].x, points[1].y, points[2].x, points[2].y,
                         points[3].x, points[3].y);
      break;
    case PathVerb::Close:
      path2D->call<void>("closePath");
      break;
  }
}

void WebMask::fillPath(const Path& path) {
  if (path.isEmpty()) {
    return;
  }
  auto path2DClass = val::global("Path2D");
  if (!path2DClass.as<bool>()) {
    return;
  }
  auto finalPath = path;
  finalPath.transform(matrix);
  auto path2D = path2DClass.new_();
  finalPath.decompose(Iterator, &path2D);
  webMask.call<val>("fillPath", path2D, path.getFillType());
}

bool WebMask::fillText(const TextBlob* textBlob) {
  return drawText(textBlob);
}

bool WebMask::strokeText(const TextBlob* textBlob, const Stroke& stroke) {
  return drawText(textBlob, &stroke);
}

bool WebMask::drawText(const TextBlob* textBlob, const Stroke* stroke) {
  if (!CanUseAsMask(textBlob)) {
    return false;
  }
  std::vector<std::string> texts{};
  std::vector<Point> points{};
  const auto* webTextBlob = static_cast<const WebTextBlob*>(textBlob);
  webTextBlob->getTextsAndPositions(&texts, &points);
  const auto& font = webTextBlob->getFont();
  if (stroke) {
    webMask.call<val>("strokeText", font.getSize(), font.isFauxBold(), font.isFauxItalic(),
                      font.getTypeface()->fontFamily(), *stroke, texts, points, matrix);
  } else {
    webMask.call<val>("fillText", font.getSize(), font.isFauxBold(), font.isFauxItalic(),
                      font.getTypeface()->fontFamily(), texts, points, matrix);
  }
  return true;
}
}  // namespace pag
