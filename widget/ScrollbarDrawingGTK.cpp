/* -*- Mode: C++; tab-width: 40; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ScrollbarDrawingGTK.h"

#include "mozilla/gfx/Helpers.h"
#include "mozilla/StaticPrefs_widget.h"
#include "nsLayoutUtils.h"
#include "nsNativeTheme.h"

using namespace mozilla;
using namespace mozilla::gfx;
using namespace mozilla::widget;

LayoutDeviceIntSize ScrollbarDrawingGTK::GetMinimumWidgetSize(
    nsPresContext* aPresContext, StyleAppearance aAppearance,
    nsIFrame* aFrame) {
  MOZ_ASSERT(nsNativeTheme::IsWidgetScrollbarPart(aAppearance));
  auto scrollbarSize = GetScrollbarSize(aPresContext, aFrame);
  LayoutDeviceIntSize size{scrollbarSize, scrollbarSize};
  if (aAppearance == StyleAppearance::ScrollbarHorizontal ||
      aAppearance == StyleAppearance::ScrollbarVertical ||
      aAppearance == StyleAppearance::ScrollbarthumbHorizontal ||
      aAppearance == StyleAppearance::ScrollbarthumbVertical) {
    CSSCoord thumbSize(
        StaticPrefs::widget_non_native_theme_gtk_scrollbar_thumb_cross_size());
    const bool isVertical =
        aAppearance == StyleAppearance::ScrollbarVertical ||
        aAppearance == StyleAppearance::ScrollbarthumbVertical;
    auto dpi = GetDPIRatioForScrollbarPart(aPresContext);
    if (isVertical) {
      size.height = thumbSize * dpi;
    } else {
      size.width = thumbSize * dpi;
    }
  }
  return size;
}

/*LayoutDeviceIntSize ScrollbarDrawingGTK::GetMinimumWidgetSize(
    nsPresContext* aPresContext, StyleAppearance aAppearance,
    nsIFrame* aFrame) {
  MOZ_ASSERT(nsNativeTheme::IsWidgetScrollbarPart(aAppearance));

  switch (aAppearance) {
    case StyleAppearance::ScrollbarbuttonUp:
    case StyleAppearance::ScrollbarbuttonDown:
    case StyleAppearance::ScrollbarbuttonLeft:
    case StyleAppearance::ScrollbarbuttonRight:
      // For scrollbar-width:thin, we don't display the buttons.
      if (IsScrollbarWidthThin(aFrame)) {
        return LayoutDeviceIntSize{};
      }
      [[fallthrough]];
    case StyleAppearance::ScrollbarVertical:
    case StyleAppearance::ScrollbarHorizontal:
    case StyleAppearance::ScrollbarthumbVertical:
    case StyleAppearance::ScrollbarthumbHorizontal: {
      // TODO: for short scrollbars it could be nice if the thumb could shrink
      // under this size.
      auto relevantSize = GetScrollbarSize(aPresContext, aFrame);
      const bool isHorizontal =
          aAppearance == StyleAppearance::ScrollbarHorizontal ||
          aAppearance == StyleAppearance::ScrollbarthumbHorizontal ||
          aAppearance == StyleAppearance::ScrollbarbuttonLeft ||
          aAppearance == StyleAppearance::ScrollbarbuttonRight;
      auto size = LayoutDeviceIntSize{relevantSize, relevantSize};
      if (aAppearance == StyleAppearance::ScrollbarHorizontal ||
          aAppearance == StyleAppearance::ScrollbarVertical) {
        // Always reserve some space in the right direction. Historically we've
        // reserved 2 times the size in the other axis (for the buttons).
        // We do this even when painting thin scrollbars just for consistency,
        // though there just one would probably do there.
        if (isHorizontal) {
          size.width *= 2;
        } else {
          size.height *= 2;
        }
      }
      return size;
    }
    default:
      return LayoutDeviceIntSize{};
  }
}*/

Maybe<nsITheme::Transparency> ScrollbarDrawingGTK::GetScrollbarPartTransparency(
    nsIFrame* aFrame, StyleAppearance aAppearance) {
  if (!aFrame->PresContext()->UseOverlayScrollbars() &&
      (aAppearance == StyleAppearance::ScrollbarVertical ||
       aAppearance == StyleAppearance::ScrollbarHorizontal) &&
      IsScrollbarTrackOpaque(aFrame)) {
    return Some(nsITheme::eOpaque);
  }

  return Nothing();
}

template <typename PaintBackendData>
bool ScrollbarDrawingGTK::DoPaintScrollbarThumb(
    PaintBackendData& aPaintData, const LayoutDeviceRect& aRect,
    ScrollbarKind aScrollbarKind, nsIFrame* aFrame, const ComputedStyle& aStyle,
    const ElementState& aElementState, const DocumentState& aDocumentState,
    const Colors& aColors, const DPIRatio& aDpiRatio) {
  sRGBColor thumbColor = ComputeScrollbarThumbColor(
      aFrame, aStyle, aElementState, aDocumentState, aColors);

  LayoutDeviceRect thumbRect(aRect);

  const bool horizontal = aScrollbarKind == ScrollbarKind::Horizontal;
  if (aFrame->PresContext()->UseOverlayScrollbars() &&
      !ScrollbarDrawing::IsParentScrollbarHoveredOrActive(aFrame)) {
    if (horizontal) {
      thumbRect.height *= 0.5;
      thumbRect.y += thumbRect.height;
    } else {
      thumbRect.width *= 0.5;
      if (aScrollbarKind == ScrollbarKind::VerticalRight) {
        thumbRect.x += thumbRect.width;
      }
    }
  }

  {
    float factor = std::max(
        0.0f,
        1.0f - StaticPrefs::widget_non_native_theme_gtk_scrollbar_thumb_size());
    thumbRect.Deflate((horizontal ? thumbRect.height : thumbRect.width) *
                      factor);
  }

  LayoutDeviceCoord radius =
      StaticPrefs::widget_non_native_theme_gtk_scrollbar_round_thumb()
          ? (horizontal ? thumbRect.height : thumbRect.width) / 2.0f
          : 0.0f;

  ThemeDrawing::PaintRoundedRectWithRadius(aPaintData, thumbRect, thumbColor,
                                           sRGBColor(), 0, radius / aDpiRatio,
                                           aDpiRatio);
  return true;
}

bool ScrollbarDrawingGTK::PaintScrollbarThumb(
    DrawTarget& aDrawTarget, const LayoutDeviceRect& aRect,
    ScrollbarKind aScrollbarKind, nsIFrame* aFrame, const ComputedStyle& aStyle,
    const ElementState& aElementState, const DocumentState& aDocumentState,
    const Colors& aColors, const DPIRatio& aDpiRatio) {
  return DoPaintScrollbarThumb(aDrawTarget, aRect, aScrollbarKind, aFrame,
                               aStyle, aElementState, aDocumentState, aColors,
                               aDpiRatio);
}

bool ScrollbarDrawingGTK::PaintScrollbarThumb(
    WebRenderBackendData& aWrData, const LayoutDeviceRect& aRect,
    ScrollbarKind aScrollbarKind, nsIFrame* aFrame, const ComputedStyle& aStyle,
    const ElementState& aElementState, const DocumentState& aDocumentState,
    const Colors& aColors, const DPIRatio& aDpiRatio) {
  return DoPaintScrollbarThumb(aWrData, aRect, aScrollbarKind, aFrame, aStyle,
                               aElementState, aDocumentState, aColors,
                               aDpiRatio);
}

bool ScrollbarDrawingGTK::ShouldDrawScrollbarButtons() {
  if (StaticPrefs::widget_non_native_theme_enabled()) {
    return StaticPrefs::widget_non_native_theme_gtk_scrollbar_allow_buttons();
  }
  return true;
}

void ScrollbarDrawingGTK::RecomputeScrollbarParams() {
  uint32_t defaultSize = 12;
  uint32_t overrideSize =
      StaticPrefs::widget_non_native_theme_scrollbar_size_override();
  if (overrideSize > 0) {
    defaultSize = overrideSize;
  }
  ConfigureScrollbarSize(defaultSize);
}
