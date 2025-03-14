/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "VibrancyManager.h"
#include "ViewRegion.h"
#include "nsRegion.h"
#include "ViewRegion.h"

#import <objc/message.h>

#include "nsChildView.h"
#include "mozilla/StaticPrefs_widget.h"

using namespace mozilla;

@interface MOZVibrantView : NSVisualEffectView {
  VibrancyType mType;
}
- (instancetype)initWithFrame:(NSRect)aRect
                 vibrancyType:(VibrancyType)aVibrancyType;
@end

static NSVisualEffectState VisualEffectStateForVibrancyType(
    VibrancyType aType) {
  switch (aType) {
    case VibrancyType::TOOLTIP:
    case VibrancyType::MENU:
    case VibrancyType::HIGHLIGHTED_MENUITEM:
      // Tooltip and menu windows are never "key", so we need to tell the
      // vibrancy effect to look active regardless of window state.
      return NSVisualEffectStateActive;
    default:
      return NSVisualEffectStateFollowsWindowActiveState;
  }
}

static NSVisualEffectMaterial VisualEffectMaterialForVibrancyType(
    VibrancyType aType) {
  switch (aType) {
    case VibrancyType::Titlebar:
      return NSVisualEffectMaterialTitlebar;
  }
}

static NSVisualEffectBlendingMode VisualEffectBlendingModeForVibrancyType(
    VibrancyType aType) {
  switch (aType) {
    case VibrancyType::TOOLTIP:
      return (NSVisualEffectMaterial)NSVisualEffectMaterialToolTip;
    case VibrancyType::MENU:
      return NSVisualEffectMaterialMenu;
    case VibrancyType::SOURCE_LIST:
      return NSVisualEffectMaterialSidebar;
    case VibrancyType::SOURCE_LIST_SELECTION:
      return NSVisualEffectMaterialSelection;
    case VibrancyType::HIGHLIGHTED_MENUITEM:
    case VibrancyType::ACTIVE_SOURCE_LIST_SELECTION:
      *aOutIsEmphasized = YES;
      return NSVisualEffectMaterialSelection;
  }
}

@implementation MOZVibrantView
- (instancetype)initWithFrame:(NSRect)aRect vibrancyType:(VibrancyType)aType {
  self = [super initWithFrame:aRect];
  mType = aType;

  self.appearance = nil;
  self.state = VisualEffectStateForVibrancyType(mType);
  self.material = VisualEffectMaterialForVibrancyType(mType);
  self.blendingMode = VisualEffectBlendingModeForVibrancyType(mType);
  self.emphasized = NO;
  return self;
}

- (NSView*)hitTest:(NSPoint)aPoint {
  // This view must be transparent to mouse events.
  return nil;
}
@end

VibrancyManager::VibrancyManager(const nsChildView& aCoordinateConverter,
                                 NSView* aContainerView)
    : mCoordinateConverter(aCoordinateConverter),
      mContainerView(aContainerView) {}

VibrancyManager::~VibrancyManager() = default;

bool VibrancyManager::UpdateVibrantRegion(
    VibrancyType aType, const LayoutDeviceIntRegion& aRegion) {
  auto& slot = mVibrantRegions[aType];
  if (aRegion.IsEmpty()) {
    bool hadRegion = !!slot;
    slot = nullptr;
    return hadRegion;
  }
  if (!slot) {
    slot = MakeUnique<ViewRegion>();
  }
  return slot->UpdateRegion(aRegion, mCoordinateConverter, mContainerView, ^() {
    return [[MOZVibrantView alloc] initWithFrame:NSZeroRect vibrancyType:aType];
  });
}
