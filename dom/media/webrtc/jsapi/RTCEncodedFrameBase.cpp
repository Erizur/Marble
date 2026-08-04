/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "jsapi/RTCEncodedFrameBase.h"

#include "js/GCAPI.h"
#include "mozilla/HoldDropJSObjects.h"
#include "nsIGlobalObject.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/RTCRtpScriptTransformer.h"
#include "js/ArrayBuffer.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_CLASS(RTCEncodedFrameBase)
NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(RTCEncodedFrameBase)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mOwner, mGlobal)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mData)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER
NS_IMPL_CYCLE_COLLECTION_UNLINK_END
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(RTCEncodedFrameBase)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mOwner, mGlobal)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END
NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN(RTCEncodedFrameBase)
  NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBERS(mData)
  NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(RTCEncodedFrameBase)
NS_IMPL_CYCLE_COLLECTING_RELEASE(RTCEncodedFrameBase)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(RTCEncodedFrameBase)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

RTCEncodedFrameBase::RTCEncodedFrameBase(
    nsIGlobalObject* aGlobal,
    std::unique_ptr<webrtc::TransformableFrameInterface> aFrame,
    uint64_t aCounter, RTCRtpScriptTransformer* aOwner)
    : mGlobal(aGlobal),
      mFrame(std::move(aFrame)),
      mCounter(aCounter),
      mTimestamp(mFrame->GetTimestamp()),
      mOwner(aOwner) {
  AutoJSAPI jsapi;
  if (NS_WARN_IF(!jsapi.Init(mGlobal))) {
    return;
  }

  mozilla::HoldJSObjects(this);

  const auto& frame = mFrame->GetData();
  if (frame.data()) {
    UniquePtr<void, JS::FreePolicy> data(js_pod_arena_malloc<uint8_t>(
        js::ArrayBufferContentsArena, frame.size()));
    memcpy(data.get(), frame.data(), frame.size());
    mData = JS::NewArrayBufferWithContents(jsapi.cx(), frame.size(),
                                           std::move(data));
  } else {
    mData = JS::NewArrayBuffer(jsapi.cx(), 0);
  }
}

RTCEncodedFrameBase::~RTCEncodedFrameBase() {
  DetachData();
  mozilla::DropJSObjects(this);
}

void RTCEncodedFrameBase::DetachData() {
  // We might have handled this in unlink already
  if (mGlobal && mData) {
    AutoJSAPI jsapi;
    if (NS_WARN_IF(!jsapi.Init(mGlobal))) {
      return;
    }

    JS::Rooted<JSObject*> rootedData(jsapi.cx(), mData);
    if (rootedData) {
      JS::DetachArrayBuffer(jsapi.cx(), rootedData);
    }
  }
}

nsIGlobalObject* RTCEncodedFrameBase::GetParentObject() const {
  return mGlobal;
}

unsigned long RTCEncodedFrameBase::Timestamp() const { return mTimestamp; }

void RTCEncodedFrameBase::SetData(const ArrayBuffer& aData) {
  mData.set(aData.Obj());
  if (mFrame) {
    aData.ProcessData([&](const Span<uint8_t>& aData, JS::AutoCheckCannotGC&&) {
      mFrame->SetData(
          rtc::ArrayView<const uint8_t>(aData.Elements(), aData.Length()));
    });
  }
}

void RTCEncodedFrameBase::GetData(JSContext* aCx, JS::Rooted<JSObject*>* aObj) {
  aObj->set(mData);
}

uint64_t RTCEncodedFrameBase::GetCounter() const { return mCounter; }

std::unique_ptr<webrtc::TransformableFrameInterface>
RTCEncodedFrameBase::TakeFrame() {
  DetachData();
  return std::move(mFrame);
}

size_t RTCEncodedFrameBase::Size() const {
  return GetArrayBufferByteLength(mData);
}

}  // namespace mozilla::dom
