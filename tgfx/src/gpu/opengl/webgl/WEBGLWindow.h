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

#pragma once

#include "WEBGLDevice.h"
#include "gpu/Window.h"

namespace pag {
class WEBGLWindow : public Window {
 public:
  /**
   * Creates a new window from a canvas.
   */
  static std::shared_ptr<WEBGLWindow> MakeFrom(const std::string& canvasID);

 protected:
  std::shared_ptr<Surface> onCreateSurface(Context* context) override;

  void onPresent(Context*, int64_t) override {
  }

 private:
  std::string canvasID;

  explicit WEBGLWindow(std::shared_ptr<Device> device);
};
}  // namespace pag
