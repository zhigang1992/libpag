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

#include "FilterRenderer.h"
#include "base/utils/MatrixUtil.h"
#include "gpu/Surface.h"
#include "gpu/opengl/GLContext.h"
#include "rendering/caches/LayerCache.h"
#include "rendering/caches/RenderCache.h"
#include "rendering/filters/DisplacementMapFilter.h"
#include "rendering/filters/FilterModifier.h"
#include "rendering/filters/LayerStylesFilter.h"
#include "rendering/filters/MotionBlurFilter.h"
#include "rendering/filters/utils/FilterBuffer.h"
#include "rendering/filters/utils/FilterHelper.h"

namespace pag {
#define FAST_BLUR_MAX_SCALE_FACTOR 0.1f

float GetScaleFactorLimit(Layer* layer) {
  auto scaleFactorLimit = layer->type() == LayerType::Image ? 1.0f : FLT_MAX;
  return scaleFactorLimit;
}

bool DoesProcessVisibleAreaOnly(Layer* layer) {
  for (auto& effect : layer->effects) {
    if (!effect->processVisibleAreaOnly()) {
      return false;
    }
  }
  return true;
}

std::unique_ptr<FilterList> FilterRenderer::MakeFilterList(const FilterModifier* modifier) {
  auto filterList = std::unique_ptr<FilterList>(new FilterList());
  auto layer = modifier->layer;
  auto layerFrame = modifier->layerFrame;
  filterList->layer = layer;
  filterList->layerFrame = layerFrame;
  auto contentFrame = layerFrame - layer->startTime;
  filterList->layerMatrix = LayerCache::Get(layer)->getTransform(contentFrame)->matrix;
  filterList->scaleFactorLimit = GetScaleFactorLimit(filterList->layer);
  filterList->processVisibleAreaOnly = DoesProcessVisibleAreaOnly(filterList->layer);
  for (auto& effect : layer->effects) {
    if (effect->visibleAt(layerFrame)) {
      filterList->effects.push_back(effect);
    }
  }
  for (auto& layerStyle : layer->layerStyles) {
    if (layerStyle->visibleAt(layerFrame)) {
      filterList->layerStyles.push_back(layerStyle);
    }
  }
  // 当存在非只处理可见区域的Effect滤镜时，对于Shape和Text图层会直接取父级Composition的的尺寸，
  // 应用图层本身的matrix之后再作为
  // 输入纹理，超出部分截断。而对于Solid，Image，PreCompose按内容实际尺寸作为输入纹理，并且不包含图层本身
  // 的matrix。
  auto needParentSizeInput = !layer->effects.empty() && (layer->type() == LayerType::Shape ||
                                                         layer->type() == LayerType::Text);
  filterList->useParentSizeInput = !filterList->processVisibleAreaOnly && needParentSizeInput;
  if (!filterList->useParentSizeInput) {
    // LayerStyle本应该在图层matrix之后应用的，但是我们为了简化渲染改到了matrix之前，因此需要反向排除图层matrix的缩放值。
    filterList->layerStyleScale = GetScaleFactor(filterList->layerMatrix, 1.0f, true);
    if (needParentSizeInput) {
      // 含有本应该在图层matrix之后应用的Effect，但是我们为了简化渲染改到了matrix之前，因此需要反向排除图层matrix
      // 的缩放值。
      filterList->effectScale = filterList->layerStyleScale;
    }
  }

  return filterList;
}

Rect FilterRenderer::GetParentBounds(const FilterList* filterList) {
  auto layer = filterList->layer;
  float width, height;
  switch (layer->type()) {
    case LayerType::Shape:  // fall through
    case LayerType::Text:
      // 对于Shape和Text一定有containingComposition。
      width = layer->containingComposition->width;
      height = layer->containingComposition->height;
      break;
    case LayerType::Solid:
      width = static_cast<SolidLayer*>(layer)->width;
      height = static_cast<SolidLayer*>(layer)->height;
      break;
    case LayerType::Image:
      width = static_cast<ImageLayer*>(layer)->imageBytes->width;
      height = static_cast<ImageLayer*>(layer)->imageBytes->height;
      break;
    case LayerType::PreCompose:
      width = static_cast<PreComposeLayer*>(layer)->composition->width;
      height = static_cast<PreComposeLayer*>(layer)->composition->height;
      break;
    default:
      width = height = 0;
      break;
  }
  return Rect::MakeXYWH(0, 0, width, height);
}

Rect FilterRenderer::GetContentBounds(const FilterList* filterList,
                                      std::shared_ptr<Graphic> content) {
  Rect contentBounds = Rect::MakeEmpty();
  if (filterList->processVisibleAreaOnly) {
    content->measureBounds(&contentBounds);
    contentBounds.roundOut();
  } else {
    contentBounds = GetParentBounds(filterList);
  }
  return contentBounds;
}

void TransformFilterBounds(Rect* filterBounds, const FilterList* filterList) {
  // 滤镜应用顺序：Effects->motionBlur->LayerStyles
  for (auto& effect : filterList->effects) {
    effect->transformBounds(filterBounds, filterList->effectScale, filterList->layerFrame);
    filterBounds->roundOut();
  }

  if (filterList->layer->motionBlur) {
    MotionBlurFilter::TransformBounds(filterBounds, filterList->effectScale, filterList->layer,
                                      filterList->layerFrame);
  }

  if (!filterList->layerStyles.empty()) {
    LayerStylesFilter::TransformBounds(filterBounds, filterList);
  }
}

void FilterRenderer::MeasureFilterBounds(Rect* bounds, const FilterModifier* modifier) {
  auto filterList = MakeFilterList(modifier);
  if (filterList->processVisibleAreaOnly) {
    bounds->roundOut();
  } else {
    *bounds = GetParentBounds(filterList.get());
  }
  TransformFilterBounds(bounds, filterList.get());
  if (filterList->useParentSizeInput) {
    Matrix inverted = Matrix::I();
    filterList->layerMatrix.invert(&inverted);
    inverted.mapRect(bounds);
  }
}

Rect GetClipBounds(Canvas* canvas, const FilterList* filterList) {
  auto clip = canvas->getGlobalClip();
  auto matrix = canvas->getMatrix();
  if (filterList->useParentSizeInput) {
    Matrix inverted = Matrix::I();
    filterList->layerMatrix.invert(&inverted);
    matrix.preConcat(inverted);
  }
  Matrix inverted = Matrix::I();
  matrix.invert(&inverted);
  clip.transform(inverted);
  return clip.getBounds();
}

std::shared_ptr<Graphic> GetDisplacementMapGraphic(const FilterList* filterList, Layer* mapLayer,
                                                   Rect* mapBounds) {
  // DisplacementMap只支持引用视频序列帧或者位图序列帧图层。
  // TODO(domrjchen): DisplacementMap 支持所有图层
  auto preComposeLayer = static_cast<PreComposeLayer*>(mapLayer);
  auto composition = preComposeLayer->composition;
  mapBounds->setXYWH(0, 0, static_cast<float>(composition->width),
                     static_cast<float>(composition->height));
  auto contentFrame = filterList->layerFrame - mapLayer->startTime;
  auto layerCache = LayerCache::Get(mapLayer);
  auto content = layerCache->getContent(contentFrame);
  return static_cast<GraphicContent*>(content)->graphic;
}

static bool MakeLayerStyleNode(std::vector<FilterNode>& filterNodes, Rect& clipBounds,
                               const FilterList* filterList, RenderCache* renderCache,
                               Rect& filterBounds) {
  if (!filterList->layerStyles.empty()) {
    auto filter = renderCache->getLayerStylesFilter(filterList->layer);
    if (nullptr == filter) {
      return false;
    }
    auto layerStyleScale = filterList->layerStyleScale;
    auto oldBounds = filterBounds;
    LayerStylesFilter::TransformBounds(&filterBounds, filterList);
    filterBounds.roundOut();
    filter->update(filterList, oldBounds, filterBounds, layerStyleScale);
    if (!filterBounds.intersect(clipBounds)) {
      return false;
    }
    filterNodes.emplace_back(filter, filterBounds);
  }
  return true;
}

static bool MakeMotionBlurNode(std::vector<FilterNode>& filterNodes, Rect& clipBounds,
                               const FilterList* filterList, RenderCache* renderCache,
                               Rect& filterBounds, Point& effectScale) {
  if (filterList->layer->motionBlur) {
    auto filter = renderCache->getMotionBlurFilter();
    if (filter && filter->updateLayer(filterList->layer, filterList->layerFrame)) {
      auto oldBounds = filterBounds;
      MotionBlurFilter::TransformBounds(&filterBounds, effectScale, filterList->layer,
                                        filterList->layerFrame);
      filterBounds.roundOut();
      filter->update(filterList->layerFrame, oldBounds, filterBounds, effectScale);
      if (!filterBounds.intersect(clipBounds)) {
        return false;
      }
      filterNodes.emplace_back(filter, filterBounds);
    }
  }
  return true;
}

bool FilterRenderer::MakeEffectNode(std::vector<FilterNode>& filterNodes, Rect& clipBounds,
                                    const FilterList* filterList, RenderCache* renderCache,
                                    Rect& filterBounds, Point& effectScale, int clipIndex) {
  auto effectIndex = 0;
  for (auto& effect : filterList->effects) {
    auto filter = renderCache->getFilterCache(effect);
    if (filter) {
      auto oldBounds = filterBounds;
      effect->transformBounds(&filterBounds, effectScale, filterList->layerFrame);
      filterBounds.roundOut();
      filter->update(filterList->layerFrame, oldBounds, filterBounds, effectScale);
      if (effect->type() == EffectType::DisplacementMap) {
        auto mapEffect = static_cast<DisplacementMapEffect*>(effect);
        auto mapFilter = static_cast<DisplacementMapFilter*>(filter);
        auto mapBounds = Rect::MakeEmpty();
        auto graphic =
            GetDisplacementMapGraphic(filterList, mapEffect->displacementMapLayer, &mapBounds);
        mapBounds.roundOut();
        mapFilter->updateMapTexture(renderCache, graphic.get(), mapBounds);
      }
      if (effectIndex >= clipIndex && !filterBounds.intersect(clipBounds)) {
        return false;
      }
      filterNodes.emplace_back(filter, filterBounds);
    }
    effectIndex++;
  }
  return true;
}

std::vector<FilterNode> FilterRenderer::MakeFilterNodes(const FilterList* filterList,
                                                        RenderCache* renderCache,
                                                        Rect* contentBounds, const Rect& clipRect) {
  // 滤镜应用顺序：Effects->PAGFilter->motionBlur->LayerStyles
  std::vector<FilterNode> filterNodes = {};
  int clipIndex = -1;
  for (int i = static_cast<int>(filterList->effects.size()) - 1; i >= 0; i--) {
    auto effect = filterList->effects[i];
    if (!effect->processVisibleAreaOnly()) {
      clipIndex = i;
      break;
    }
  }
  auto clipBounds = clipRect;
  auto filterBounds = *contentBounds;
  auto effectScale = filterList->effectScale;
  // MotionBlur Fragment Shader中需要用到裁切区域外的像素，
  // 因此默认先把裁切区域过一次TransformBounds
  if (filterList->layer->motionBlur) {
    MotionBlurFilter::TransformBounds(&clipBounds, effectScale, filterList->layer,
                                      filterList->layerFrame);
    clipBounds.roundOut();
  }
  if (clipIndex == -1 && !contentBounds->intersect(clipBounds)) {
    return {};
  }

  if (!MakeEffectNode(filterNodes, clipBounds, filterList, renderCache, filterBounds, effectScale,
                      clipIndex)) {
    return {};
  }

  if (!MakeMotionBlurNode(filterNodes, clipBounds, filterList, renderCache, filterBounds,
                          effectScale)) {
    return {};
  }

  if (!MakeLayerStyleNode(filterNodes, clipBounds, filterList, renderCache, filterBounds)) {
    return {};
  }
  return filterNodes;
}

void ApplyFilters(Context* context, std::vector<FilterNode> filterNodes, const Rect& contentBounds,
                  FilterSource* filterSource, FilterTarget* filterTarget) {
  GLStateGuard stateGuard(context);
  auto gl = GLContext::Unwrap(context);
  auto scale = filterSource->scale;
  std::shared_ptr<FilterBuffer> freeBuffer = nullptr;
  std::shared_ptr<FilterBuffer> lastBuffer = nullptr;
  std::shared_ptr<FilterSource> lastSource = nullptr;
  auto lastBounds = contentBounds;
  auto lastUsesMSAA = false;
  auto size = static_cast<int>(filterNodes.size());
  for (int i = 0; i < size; i++) {
    auto& node = filterNodes[i];
    auto source = lastSource == nullptr ? filterSource : lastSource.get();
    if (i == size - 1) {
      node.filter->draw(context, source, filterTarget);
      break;
    }
    std::shared_ptr<FilterBuffer> currentBuffer = nullptr;
    if (freeBuffer && node.bounds.width() == lastBounds.width() &&
        node.bounds.height() == lastBounds.height() && node.filter->needsMSAA() == lastUsesMSAA) {
      currentBuffer = freeBuffer;
    } else {
      currentBuffer = FilterBuffer::Make(
          context, static_cast<int>(ceilf(node.bounds.width() * scale.x)),
          static_cast<int>(ceilf(node.bounds.height() * scale.y)), node.filter->needsMSAA());
    }
    if (currentBuffer == nullptr) {
      return;
    }
    currentBuffer->clearColor(gl);
    auto offsetMatrix = Matrix::MakeTrans((lastBounds.left - node.bounds.left) * scale.x,
                                          (lastBounds.top - node.bounds.top) * scale.y);
    auto currentTarget = currentBuffer->toFilterTarget(offsetMatrix);
    node.filter->draw(context, source, currentTarget.get());
    currentBuffer->resolve(context);
    lastSource = currentBuffer->toFilterSource(scale);
    freeBuffer = lastBuffer;
    lastBuffer = currentBuffer;
    lastBounds = node.bounds;
    lastUsesMSAA = currentBuffer->usesMSAA();
  }
}

std::unique_ptr<FilterTarget> GetDirectFilterTarget(Canvas* parentCanvas,
                                                    const FilterList* filterList,
                                                    const std::vector<FilterNode>& filterNodes,
                                                    const Rect& contentBounds,
                                                    const Point& sourceScale) {
  // 在高分辨率下，模糊滤镜的开销会增大，需要降采样降低开销；当模糊为最后一个滤镜时，需要离屏绘制
  if (!filterList->effects.empty() && filterList->effects.back()->type() == EffectType::FastBlur) {
    return nullptr;
  }
  if (filterNodes.back().filter->needsMSAA()) {
    return nullptr;
  }
  // 是否能直接上屏，应该用没有经过裁切的transformBounds来判断，
  // 因为计算filter的顶点位置的bounds都是没有经过裁切的
  auto transformBounds = contentBounds;
  TransformFilterBounds(&transformBounds, filterList);
  if (parentCanvas->hasComplexPaint(transformBounds) != PaintKind::None) {
    return nullptr;
  }
  auto surface = parentCanvas->getSurface();
  auto totalMatrix = parentCanvas->getMatrix();
  if (totalMatrix.getSkewX() != 0 || totalMatrix.getSkewY() != 0) {
    return nullptr;
  }
  auto secondToLastBounds =
      filterNodes.size() > 1 ? filterNodes[filterNodes.size() - 2].bounds : contentBounds;
  totalMatrix.preTranslate(secondToLastBounds.left, secondToLastBounds.top);
  totalMatrix.preScale(1.0f / sourceScale.x, 1.0f / sourceScale.y);
  return ToFilterTarget(surface, totalMatrix);
}

std::unique_ptr<FilterTarget> GetOffscreenFilterTarget(Surface* surface,
                                                       const std::vector<FilterNode>& filterNodes,
                                                       const Rect& contentBounds,
                                                       const Point& sourceScale) {
  auto finalBounds = filterNodes.back().bounds;
  auto secondToLastBounds =
      filterNodes.size() > 1 ? filterNodes[filterNodes.size() - 2].bounds : contentBounds;
  auto totalMatrix = Matrix::MakeTrans((secondToLastBounds.left - finalBounds.left) * sourceScale.x,
                                       (secondToLastBounds.top - finalBounds.top) * sourceScale.y);
  return ToFilterTarget(surface, totalMatrix);
}

std::unique_ptr<FilterSource> ToFilterSource(Canvas* canvas) {
  auto surface = canvas->getSurface();
  auto texture = surface->getTexture();
  Point scale = {};
  scale.x = scale.y = GetMaxScaleFactor(canvas->getMatrix());
  return ToFilterSource(texture.get(), scale);
}

void FilterRenderer::ProcessFastBlur(FilterList* filterList) {
  // 注意：含有layerStyle的情况下，不能走SingleImage的优化模式，因为目前DropShadowFilter还不是shader模式，
  // 无法实现textureMatrix的绘制，等DropShadowFilter改为shader模式后去掉这个限制。
  // 在高分辨率下，模糊滤镜的开销会增大，需要降采样降低开销；当模糊为最后一个滤镜时，需要离屏绘制
  for (auto effect : filterList->effects) {
    if (effect->type() == EffectType::FastBlur) {
      auto blurEffect = static_cast<FastBlurEffect*>(effect);
      // 当模糊度不变化时，使用缩放提高性能
      if (!blurEffect->blurriness->animatable()) {
        filterList->scaleFactorLimit = FAST_BLUR_MAX_SCALE_FACTOR;
      }
      break;
    }
  }
}

void FilterRenderer::DrawWithFilter(Canvas* parentCanvas, RenderCache* cache,
                                    const FilterModifier* modifier,
                                    std::shared_ptr<Graphic> content) {
  auto filterList = MakeFilterList(modifier);
  auto contentBounds = GetContentBounds(filterList.get(), content);
  // 相对于content Bounds的clip Bounds
  auto clipBounds = GetClipBounds(parentCanvas, filterList.get());
  auto filterNodes = MakeFilterNodes(filterList.get(), cache, &contentBounds, clipBounds);
  if (filterNodes.empty()) {
    content->draw(parentCanvas, cache);
    return;
  }
  if (filterList->useParentSizeInput) {
    Matrix inverted = Matrix::I();
    filterList->layerMatrix.invert(&inverted);
    parentCanvas->concat(inverted);
  }
  ProcessFastBlur(filterList.get());
  auto contentSurface =
      parentCanvas->makeContentSurface(contentBounds, filterList->scaleFactorLimit);
  if (contentSurface == nullptr) {
    return;
  }
  auto contentCanvas = contentSurface->getCanvas();
  if (filterList->useParentSizeInput) {
    contentCanvas->concat(filterList->layerMatrix);
  }
  content->draw(contentCanvas, cache);
  auto filterSource = ToFilterSource(contentCanvas);
  std::shared_ptr<Surface> targetSurface = nullptr;
  std::unique_ptr<FilterTarget> filterTarget = GetDirectFilterTarget(
      parentCanvas, filterList.get(), filterNodes, contentBounds, filterSource->scale);
  if (filterTarget == nullptr) {
    // 需要离屏绘制
    targetSurface =
        parentCanvas->makeContentSurface(filterNodes.back().bounds, filterList->scaleFactorLimit,
                                         filterNodes.back().filter->needsMSAA());
    if (targetSurface == nullptr) {
      return;
    }
    filterTarget = GetOffscreenFilterTarget(targetSurface.get(), filterNodes, contentBounds,
                                            filterSource->scale);
  }

  // 必须要flush，要不然framebuffer还没真正画到canvas，就被其他图层的filter串改了该framebuffer
  parentCanvas->flush();
  auto context = parentCanvas->getContext();
  ApplyFilters(context, filterNodes, contentBounds, filterSource.get(), filterTarget.get());

  if (targetSurface) {
    Matrix drawingMatrix = {};
    auto targetCanvas = targetSurface->getCanvas();
    if (!targetCanvas->getMatrix().invert(&drawingMatrix)) {
      drawingMatrix.setIdentity();
    }
    auto targetTexture = targetSurface->getTexture();
    parentCanvas->drawTexture(targetTexture.get(), drawingMatrix);
  }
}
}  // namespace pag
