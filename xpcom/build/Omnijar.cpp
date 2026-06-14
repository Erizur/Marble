/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Omnijar.h"
#include "brightwork-abi.h"  // this is a generated header from variables.py under mach!

#include "nsDirectoryService.h"
#include "nsDirectoryServiceDefs.h"
#include "mozilla/GeckoArgs.h"
#include "mozilla/ipc/ProcessChild.h"
#include "nsIFile.h"
#include "nsZipArchive.h"
#include "nsNetUtil.h"

#include "mozilla/Debug.h"
#include "nsTArray.h"
#include "prenv.h"
#include "prio.h"

namespace mozilla {

StaticRefPtr<nsIFile> Omnijar::sPath[2];
StaticRefPtr<nsZipArchive> Omnijar::sReader[2];
StaticRefPtr<nsZipArchive> Omnijar::sOuterReader[2];
bool Omnijar::sInitialized = false;
bool Omnijar::sIsUnified = false;
bool Omnijar::sBrightworkActive[2] = {false, false};

static const char* sProp[2] = {NS_GRE_DIR, NS_XPCOM_CURRENT_PROCESS_DIR};

#define SPROP(Type) ((Type == mozilla::Omnijar::GRE) ? sProp[GRE] : sProp[APP])

// BEGIN BRIGHTWORK.
//
// A custom jar is only accepted when its embedded brightwork.abi check declares an
// ABI equal to MOZ_BRIGHTWORK_ABI. That constant is defined once in
// build/variables.py (emitted to the generated brightwork-abi.h) and shared
// with the front end via AppConstants, so the compatibility check has a single
// source of truth. Any mismatch, missing token, or I/O error falls back to the bundled jar.

// Parse the brightwork.abi entry and returns
// false if it is missing or malformed.
static bool ReadBrightworkAbi(nsZipArchive* aReader, uint32_t& aAbi) {
  nsZipItemPtr<char> item(aReader, "brightwork.abi"_ns);
  if (!item) {
    return false;
  }
  nsDependentCSubstring data(item.Buffer(), item.Length());

  int32_t nl = data.FindChar('\n');
  nsCString line(
      Substring(data, 0, nl == kNotFound ? data.Length() : nl));
  line.Trim(" \t\r\n");

  nsresult rv;
  aAbi = line.ToInteger(&rv);
  return NS_SUCCEEDED(rv);
}

static bool IsSafePackageId(const nsACString& aId) {
  return !aId.IsEmpty() && aId.FindChar('/') < 0 && aId.FindChar('\\') < 0 &&
         aId.Find(".."_ns) == kNotFound;
}

// Read the active package id from the profile's prefs.js. The native loader runs before
// the pref service exists, so we parse the file directly. A bunch of boilerplate just for this!
// Mr. T. (Take Me Higher) - Risky Men feat. Asuka M.
static bool ReadActiveId(nsIFile* aPrefsJs, nsACString& aId) {
  PRFileDesc* fd = nullptr;
  if (NS_FAILED(aPrefsJs->OpenNSPRFileDesc(PR_RDONLY, 0, &fd)) || !fd) {
    return false;
  }
  nsAutoCString contents;
  char buf[4096];
  int32_t n;
  const uint32_t kMax = 4 * 1024 * 1024;  // memory cap
  while ((n = PR_Read(fd, buf, sizeof(buf))) > 0) {
    contents.Append(buf, n);
    if (contents.Length() > kMax) {
      break;
    }
  }
  PR_Close(fd);

  constexpr auto kKey = "\"browser.brightwork.active\""_ns;
  int32_t at = contents.Find(kKey);
  if (at < 0) {
    return false;
  }
  
  const int32_t len = static_cast<int32_t>(contents.Length());
  int32_t pos = at + static_cast<int32_t>(kKey.Length());
  while (pos < len && contents[pos] != ',') {
    pos++;
  }
  pos++;  // past comma
  while (pos < len && (contents[pos] == ' ' || contents[pos] == '\t')) {
    pos++;
  }
  if (pos >= len || contents[pos] != '"') {
    return false;
  }
  pos++;  // past opening quote
  int32_t start = pos;
  while (pos < len && contents[pos] != '"') {
    pos++;
  }
  if (pos >= len) {
    return false;
  }
  nsAutoCString id(Substring(contents, start, pos - start));
  id.Trim(" \t\r\n");
  if (!IsSafePackageId(id)) {
    return false;
  }
  aId = id;
  return true;
}

// Resolve the directory that should contain the active custom omni.jas.
// Returns nullptr when nothing is opted in. Every directory-service lookup is
// guarded so absent state (e.g. no profile yet) simply falls through.
static already_AddRefed<nsIFile> ResolveBrightworkPackageDir(
    nsIFile* aProfileOverride) {
  // Environment override
  const char* env = PR_GetEnv("MOZ_BRIGHTWORK_DIR");
  if (env && *env) {
    nsCOMPtr<nsIFile> dir;
    if (NS_SUCCEEDED(NS_NewNativeLocalFile(nsDependentCString(env),
                                           getter_AddRefs(dir)))) {
      return dir.forget();
    }
    return nullptr;
  }

  // Profile-managed active package. Two callers reach this with the profile dir available at different
  // times: the omnijar loader runs after SetProfile, so the directory service
  // can supply it (aProfileOverride is null). The early compatibility /
  // startup-cache check runs before SetProfile, so it passes the profile dir
  // explicitly. Either way the pref service does not exist yet, so we read the
  // persisted value from prefs.js directly.
  nsCOMPtr<nsIFile> profDir = aProfileOverride;
  if (!profDir && nsDirectoryService::gService) {
    nsDirectoryService::gService->Get(NS_APP_USER_PROFILE_50_DIR,
                                      NS_GET_IID(nsIFile),
                                      getter_AddRefs(profDir));
  }
  if (profDir) {
    nsCOMPtr<nsIFile> prefsJs;
    profDir->Clone(getter_AddRefs(prefsJs));
    if (prefsJs) {
      prefsJs->AppendNative("prefs.js"_ns);
      nsAutoCString id;
      if (ReadActiveId(prefsJs, id)) {
        nsCOMPtr<nsIFile> pkg;
        profDir->Clone(getter_AddRefs(pkg));
        pkg->AppendNative("brightwork"_ns);
        pkg->AppendNative("packages"_ns);
        pkg->AppendNative(id);
        return pkg.forget();
      }
    }
  }

  return nullptr;
}

static already_AddRefed<nsIFile> ResolveBrightworkCandidate(
    mozilla::Omnijar::Type aType, nsIFile* aProfileOverride) {
  nsCOMPtr<nsIFile> dir = ResolveBrightworkPackageDir(aProfileOverride);
  if (!dir) {
    return nullptr;
  }
  constexpr auto kOmnijarName = nsLiteralCString{MOZ_STRINGIFY(OMNIJAR_NAME)};
  nsCOMPtr<nsIFile> file;
  dir->Clone(getter_AddRefs(file));
  if (!file) {
    return nullptr;
  }
  // GRE jar at <dir>/omni.ja and the browser jar at <dir>/browser/omni.ja.
  // this is a hardcoded lock but i find it consistent with how the layout gets produced
  // under normal circumstances, so lets keep it like that here lol.
  if (aType == mozilla::Omnijar::APP) {
    file->AppendNative("browser"_ns);
  }
  file->AppendNative(kOmnijarName);
  return file.forget();
}

static already_AddRefed<nsIFile> TryBrightwork(mozilla::Omnijar::Type aType) {
  nsCOMPtr<nsIFile> file = ResolveBrightworkCandidate(aType, nullptr);
  if (!file) {
    return nullptr;
  }
  bool isFile = false;
  if (NS_FAILED(file->IsFile(&isFile)) || !isFile) {
    return nullptr;
  }
  RefPtr<nsZipArchive> reader = nsZipArchive::OpenArchive(file);
  if (!reader) {
    return nullptr;
  }
  uint32_t abi = 0;
  if (!ReadBrightworkAbi(reader, abi) || abi != MOZ_BRIGHTWORK_ABI) {
    printf_stderr(
        "brightwork: rejecting custom %s omni.ja (abi %u, need %u); using "
        "bundled\n",
        aType == mozilla::Omnijar::GRE ? "GRE" : "APP", abi,
        static_cast<unsigned>(MOZ_BRIGHTWORK_ABI));
    return nullptr;
  }
  printf_stderr("brightwork: using custom %s omni.ja (abi %u)\n",
                aType == mozilla::Omnijar::GRE ? "GRE" : "APP", abi);
  return file.forget();
}

void Omnijar::CleanUpOne(Type aType) {
  if (sReader[aType]) {
    sReader[aType] = nullptr;
  }
  if (sOuterReader[aType]) {
    sOuterReader[aType] = nullptr;
  }
  sPath[aType] = nullptr;
  sBrightworkActive[aType] = false;
}

void Omnijar::ComputeBrightworkFingerprint(nsIFile* aProfileDir,
                                           nsACString& aResult) {
  aResult.Truncate();
  nsCOMPtr<nsIFile> dir = ResolveBrightworkPackageDir(aProfileDir);
  if (!dir) {
    return;
  }
  nsAutoCString leaf;
  if (NS_SUCCEEDED(dir->GetNativeLeafName(leaf))) {
    aResult.Assign(leaf);
  }
}

nsresult Omnijar::InitOne(nsIFile* aPath, Type aType) {
  constexpr auto kOmnijarName = nsLiteralCString{MOZ_STRINGIFY(OMNIJAR_NAME)};
  nsCOMPtr<nsIFile> file;
  if (aPath) {
    file = aPath;
  } else if (nsCOMPtr<nsIFile> brightwork = TryBrightwork(aType)) {
    file = brightwork;
    sBrightworkActive[aType] = true;
  } else {
    nsCOMPtr<nsIFile> dir;
    MOZ_TRY(nsDirectoryService::gService->Get(SPROP(aType), NS_GET_IID(nsIFile),
                                              getter_AddRefs(dir)));
    MOZ_TRY(dir->Clone(getter_AddRefs(file)));
    MOZ_TRY(file->AppendNative(kOmnijarName));
  }

  bool isFile = false;
  if (NS_FAILED(file->IsFile(&isFile)) || !isFile) {
    if ((aType == APP) && (!sPath[GRE])) {
      nsCOMPtr<nsIFile> greDir, appDir;
      bool equals;
      nsDirectoryService::gService->Get(sProp[GRE], NS_GET_IID(nsIFile),
                                        getter_AddRefs(greDir));
      nsDirectoryService::gService->Get(sProp[APP], NS_GET_IID(nsIFile),
                                        getter_AddRefs(appDir));
      if (NS_SUCCEEDED(greDir->Equals(appDir, &equals)) && equals) {
        sIsUnified = true;
      }
    }
    return NS_OK;
  }

  // If we're using omni.jar on both GRE and APP and their path
  // is the same, we're also in the unified case.
  bool equals;
  if ((aType == APP) && (sPath[GRE]) &&
      NS_SUCCEEDED(sPath[GRE]->Equals(file, &equals)) && equals) {
    // If we're using omni.jar on both GRE and APP and their path
    // is the same, we're in the unified case.
    sIsUnified = true;
    return NS_OK;
  }

  RefPtr<nsZipArchive> zipReader = nsZipArchive::OpenArchive(file);
  if (!zipReader) {
    // As file has been checked to exist as file above, any error indicates
    // that it is somehow corrupted internally.
    return NS_ERROR_FILE_CORRUPTED;
  }

  RefPtr<nsZipArchive> outerReader;
  RefPtr<nsZipHandle> handle;
  // If we find a wrapped OMNIJAR, unwrap it.
  if (NS_SUCCEEDED(
          nsZipHandle::Init(zipReader, kOmnijarName, getter_AddRefs(handle)))) {
    outerReader = zipReader;
    zipReader = nsZipArchive::OpenArchive(handle);
    if (!zipReader) {
      return NS_ERROR_FILE_CORRUPTED;
    }
  }

  CleanUpOne(aType);
  sReader[aType] = zipReader;
  sOuterReader[aType] = outerReader;
  sPath[aType] = file;

  return NS_OK;
}

nsresult Omnijar::FallibleInit(nsIFile* aGrePath, nsIFile* aAppPath) {
  // Even on error we do not want to come here again.
  sInitialized = true;

  // Let's always try to init both before returning any error for the benefit
  // of callers that do not handle the error at all.
  nsresult rvGRE = InitOne(aGrePath, GRE);
  nsresult rvAPP = InitOne(aAppPath, APP);
  MOZ_TRY(rvGRE);
  MOZ_TRY(rvAPP);

  return NS_OK;
}

void Omnijar::Init(nsIFile* aGrePath, nsIFile* aAppPath) {
  nsresult rv = FallibleInit(aGrePath, aAppPath);
  if (NS_FAILED(rv)) {
    MOZ_CRASH_UNSAFE_PRINTF("Omnijar::Init failed: %s",
                            mozilla::GetStaticErrorName(rv));
  }
}

void Omnijar::CleanUp() {
  CleanUpOne(GRE);
  CleanUpOne(APP);
  sInitialized = false;
}

already_AddRefed<nsZipArchive> Omnijar::GetReader(nsIFile* aPath) {
  MOZ_ASSERT(IsInitialized(), "Omnijar not initialized");

  bool equals;
  nsresult rv;

  if (sPath[GRE]) {
    rv = sPath[GRE]->Equals(aPath, &equals);
    if (NS_SUCCEEDED(rv) && equals) {
      return IsNested(GRE) ? GetOuterReader(GRE) : GetReader(GRE);
    }
  }
  if (sPath[APP]) {
    rv = sPath[APP]->Equals(aPath, &equals);
    if (NS_SUCCEEDED(rv) && equals) {
      return IsNested(APP) ? GetOuterReader(APP) : GetReader(APP);
    }
  }
  return nullptr;
}

already_AddRefed<nsZipArchive> Omnijar::GetInnerReader(
    nsIFile* aPath, const nsACString& aEntry) {
  MOZ_ASSERT(IsInitialized(), "Omnijar not initialized");

  if (!aEntry.EqualsLiteral(MOZ_STRINGIFY(OMNIJAR_NAME))) {
    return nullptr;
  }

  bool equals;
  nsresult rv;

  if (sPath[GRE]) {
    rv = sPath[GRE]->Equals(aPath, &equals);
    if (NS_SUCCEEDED(rv) && equals) {
      return IsNested(GRE) ? GetReader(GRE) : nullptr;
    }
  }
  if (sPath[APP]) {
    rv = sPath[APP]->Equals(aPath, &equals);
    if (NS_SUCCEEDED(rv) && equals) {
      return IsNested(APP) ? GetReader(APP) : nullptr;
    }
  }
  return nullptr;
}

nsresult Omnijar::GetURIString(Type aType, nsACString& aResult) {
  MOZ_ASSERT(IsInitialized(), "Omnijar not initialized");

  aResult.Truncate();

  // Return an empty string for APP in the unified case.
  if ((aType == APP) && sIsUnified) {
    return NS_OK;
  }

  nsAutoCString omniJarSpec;
  if (sPath[aType]) {
    nsresult rv = NS_GetURLSpecFromActualFile(sPath[aType], omniJarSpec);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    aResult = "jar:";
    if (IsNested(aType)) {
      aResult += "jar:";
    }
    aResult += omniJarSpec;
    aResult += "!";
    if (IsNested(aType)) {
      aResult += "/" MOZ_STRINGIFY(OMNIJAR_NAME) "!";
    }
  } else {
    nsCOMPtr<nsIFile> dir;
    nsDirectoryService::gService->Get(SPROP(aType), NS_GET_IID(nsIFile),
                                      getter_AddRefs(dir));
    nsresult rv = NS_GetURLSpecFromActualFile(dir, aResult);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }
  aResult += "/";
  return NS_OK;
}

#if defined(MOZ_WIDGET_ANDROID) && defined(MOZ_DIAGNOSTIC_ASSERT_ENABLED)
#  define ANDROID_DIAGNOSTIC_CRASH_OR_EXIT(_msg) MOZ_CRASH(_msg)
#elif defined(MOZ_WIDGET_ANDROID)
#  define ANDROID_DIAGNOSTIC_CRASH_OR_EXIT(_msg) ipc::ProcessChild::QuickExit()
#else
#  define ANDROID_DIAGNOSTIC_CRASH_OR_EXIT(_msg)
#endif

void Omnijar::ChildProcessInit(int& aArgc, char** aArgv) {
  nsCOMPtr<nsIFile> greOmni, appOmni;

  // Android builds are always packaged, so if we can't find anything for
  // greOmni, then this content process is useless, so kill it immediately.
  // On release, we do this via QuickExit() because the crash volume is so
  // high. See bug 1915788.
  if (auto greOmniStr = geckoargs::sGREOmni.Get(aArgc, aArgv)) {
    if (NS_WARN_IF(NS_FAILED(
            XRE_GetFileFromPath(*greOmniStr, getter_AddRefs(greOmni))))) {
      ANDROID_DIAGNOSTIC_CRASH_OR_EXIT("XRE_GetFileFromPath failed");
      greOmni = nullptr;
    }
  } else {
    ANDROID_DIAGNOSTIC_CRASH_OR_EXIT("sGREOmni.Get failed");
  }
  if (auto appOmniStr = geckoargs::sAppOmni.Get(aArgc, aArgv)) {
    if (NS_WARN_IF(NS_FAILED(
            XRE_GetFileFromPath(*appOmniStr, getter_AddRefs(appOmni))))) {
      appOmni = nullptr;
    }
  }

  // If we're unified, then only the -greomni flag is present
  // (reflecting the state of sPath in the parent process) but that
  // path should be used for both (not nullptr, which will try to
  // invoke the directory service, which probably isn't up yet.)
  if (!appOmni) {
    appOmni = greOmni;
  }

  if (greOmni) {
    Init(greOmni, appOmni);
  } else {
    // We should never have an appOmni without a greOmni.
    MOZ_ASSERT(!appOmni);
  }
}

#undef ANDROID_DIAGNOSTIC_CRASH_OR_EXIT

} /* namespace mozilla */
