/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layout_RemotePrintJobParent_h
#define mozilla_layout_RemotePrintJobParent_h

#include "mozilla/RefPtr.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/gfx/CrossProcessPaint.h"
#include "mozilla/layout/PRemotePrintJobParent.h"
#include "mozilla/layout/printing/DrawEventRecorder.h"
#include "nsCOMArray.h"
#include "nsCOMPtr.h"

class nsDeviceContext;
class nsIPrintSettings;
class nsIWebProgressListener;

namespace mozilla::layout {

class PrintTranslator;

class RemotePrintJobParent final : public PRemotePrintJobParent {
 public:
  NS_INLINE_DECL_REFCOUNTING(RemotePrintJobParent);

  explicit RemotePrintJobParent(nsIPrintSettings* aPrintSettings);

  void ActorDestroy(ActorDestroyReason aWhy) final;

  mozilla::ipc::IPCResult RecvInitializePrint(
      const nsAString& aDocumentTitle, const uint64_t& aBrowsingContextId,
      const int32_t& aStartPage, const int32_t& aEndPage) final;

  mozilla::ipc::IPCResult RecvProcessPage(const int32_t& aWidthInPoints,
                                          const int32_t& aHeightInPoints,
                                          nsTArray<uint64_t>&& aDeps) final;

  mozilla::ipc::IPCResult RecvFinalizePrint() final;

  mozilla::ipc::IPCResult RecvAbortPrint(const nsresult& aRv) final;

  mozilla::ipc::IPCResult RecvProgressChange(
      const long& aCurSelfProgress, const long& aMaxSelfProgress,
      const long& aCurTotalProgress, const long& aMaxTotalProgress) final;

  mozilla::ipc::IPCResult RecvStatusChange(const nsresult& aStatus) final;

  /**
   * Register a progress listener to receive print progress updates.
   *
   * @param aListener the progress listener to register. Must not be null.
   */
  void RegisterListener(nsIWebProgressListener* aListener);

  /**
   * @return the print settings for this remote print job.
   */
  already_AddRefed<nsIPrintSettings> GetPrintSettings();

 private:
  ~RemotePrintJobParent() final;

  void InitializePrint(const nsAString& aDocumentTitle,
                       const int32_t& aStartPage, const int32_t& aEndPage);

  void FailInitialization(nsresult);

  nsresult InitializePrintDevice(const nsAString& aDocumentTitle,
                                 const int32_t& aStartPage,
                                 const int32_t& aEndPage);

  nsresult PrepareNextPageFD(FileDescriptor* aFd);

  nsresult PrintPage(
      const gfx::IntSize& aSizeInPoints, PRFileDescStream& aRecording,
      gfx::CrossProcessPaint::ResolvedFragmentMap* aFragments = nullptr);
  void FinishProcessingPage(
      const gfx::IntSize& aSizeInPoints,
      gfx::CrossProcessPaint::ResolvedFragmentMap* aFragments = nullptr);

  /**
   * Called to notify our corresponding RemotePrintJobChild once we've
   * finished printing a page.
   */
  void PageDone(nsresult aResult);

  nsCOMPtr<nsIPrintSettings> mPrintSettings;
  RefPtr<nsDeviceContext> mPrintDeviceContext;
  UniquePtr<PrintTranslator> mPrintTranslator;
  nsCOMArray<nsIWebProgressListener> mPrintProgressListeners;
  PRFileDescStream mCurrentPageStream;

  // Once initialized, these are the various IDs of the page we're printing.
  // Note that static documents don't navigate around so the BC vs WC
  // distinction isn't particularly relevant here.
  // TODO(emilio): It'd be more consistent to use the WindowGlobal id
  // consistently, probably, but that percolates into our a11y code.
  uint64_t mBrowsingContextId{0};
  uint64_t mWindowContextId{0};
  dom::TabId mTabId{0};

  nsresult mStatus = NS_ERROR_UNEXPECTED;
  bool mIsDoingPrinting = false;
  // True after RecvInitializePrint is called.
  bool mInitializeReceived = false;
};

}  // namespace mozilla::layout

#endif  // mozilla_layout_RemotePrintJobParent_h
