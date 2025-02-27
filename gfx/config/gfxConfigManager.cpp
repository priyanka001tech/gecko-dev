/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/gfx/gfxConfigManager.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/StaticPrefs_layers.h"
#include "gfxConfig.h"
#include "gfxPlatform.h"
#include "nsIGfxInfo.h"
#include "nsXULAppAPI.h"
#include "WebRenderRollout.h"

#ifdef XP_WIN
#  include "mozilla/WindowsVersion.h"
#  include "mozilla/gfx/DeviceManagerDx.h"
#  include "mozilla/gfx/DisplayConfigWindows.h"
#endif

namespace mozilla {
namespace gfx {

void gfxConfigManager::Init() {
  MOZ_ASSERT(XRE_IsParentProcess());

  WebRenderRollout::Init();
  mWrQualifiedOverride = WebRenderRollout::CalculateQualifiedOverride();
  mWrQualified = WebRenderRollout::CalculateQualified();

  EmplaceUserPref("gfx.webrender.compositor", mWrCompositorEnabled);
  mWrForceEnabled = gfxPlatform::WebRenderPrefEnabled();
  mWrForceDisabled = StaticPrefs::gfx_webrender_force_disabled_AtStartup();
  mWrCompositorForceEnabled =
      StaticPrefs::gfx_webrender_compositor_force_enabled_AtStartup();
  mGPUProcessAllowSoftware =
      StaticPrefs::layers_gpu_process_allow_software_AtStartup();
  mWrPartialPresent =
      StaticPrefs::gfx_webrender_max_partial_present_rects_AtStartup() > 0;
#ifdef XP_WIN
  mWrForceAngle = StaticPrefs::gfx_webrender_force_angle_AtStartup();
#  ifdef NIGHTLY_BUILD
  mWrForceAngleNoGPUProcess = StaticPrefs::
      gfx_webrender_enabled_no_gpu_process_with_angle_win_AtStartup();
#  endif
  mWrDCompWinEnabled =
      Preferences::GetBool("gfx.webrender.dcomp-win.enabled", false);
#endif

  mWrEnvForceEnabled = gfxPlatform::WebRenderEnvvarEnabled();
  mWrEnvForceDisabled = gfxPlatform::WebRenderEnvvarDisabled();

#ifdef XP_WIN
  DeviceManagerDx::Get()->CheckHardwareStretchingSupport(mHwStretchingSupport);
  mScaledResolution = HasScaledResolution();
  mIsWin10OrLater = IsWin10OrLater();
  mWrCompositorDCompRequired = true;
#else
  ++mHwStretchingSupport.mBoth;
#endif

#ifdef MOZ_WIDGET_GTK
  mDisableHwCompositingNoWr = true;
  mXRenderEnabled = mozilla::Preferences::GetBool("gfx.xrender.enabled");
#endif

#ifdef NIGHTLY_BUILD
  mIsNightly = true;
#endif
  mSafeMode = gfxPlatform::InSafeMode();

  mGfxInfo = services::GetGfxInfo();

  mFeatureWr = &gfxConfig::GetFeature(Feature::WEBRENDER);
  mFeatureWrQualified = &gfxConfig::GetFeature(Feature::WEBRENDER_QUALIFIED);
  mFeatureWrCompositor = &gfxConfig::GetFeature(Feature::WEBRENDER_COMPOSITOR);
  mFeatureWrAngle = &gfxConfig::GetFeature(Feature::WEBRENDER_ANGLE);
  mFeatureWrDComp = &gfxConfig::GetFeature(Feature::WEBRENDER_DCOMP_PRESENT);
  mFeatureWrPartial = &gfxConfig::GetFeature(Feature::WEBRENDER_PARTIAL);

  mFeatureHwCompositing = &gfxConfig::GetFeature(Feature::HW_COMPOSITING);
#ifdef XP_WIN
  mFeatureD3D11HwAngle = &gfxConfig::GetFeature(Feature::D3D11_HW_ANGLE);
#endif
  mFeatureGPUProcess = &gfxConfig::GetFeature(Feature::GPU_PROCESS);
}

void gfxConfigManager::EmplaceUserPref(const char* aPrefName,
                                       Maybe<bool>& aValue) {
  if (Preferences::HasUserValue(aPrefName)) {
    aValue.emplace(Preferences::GetBool(aPrefName, false));
  }
}

void gfxConfigManager::ConfigureFromBlocklist(long aFeature,
                                              FeatureState* aFeatureState) {
  MOZ_ASSERT(aFeatureState);

  nsCString blockId;
  int32_t status;
  if (!NS_SUCCEEDED(mGfxInfo->GetFeatureStatus(aFeature, blockId, &status))) {
    aFeatureState->Disable(FeatureStatus::BlockedNoGfxInfo, "gfxInfo is broken",
                           "FEATURE_FAILURE_NO_GFX_INFO"_ns);

  } else {
    if (status != nsIGfxInfo::FEATURE_STATUS_OK) {
      aFeatureState->Disable(FeatureStatus::Blocklisted,
                             "Blocklisted by gfxInfo", blockId);
    }
  }
}

bool gfxConfigManager::ConfigureWebRenderQualified() {
  MOZ_ASSERT(mFeatureWrQualified);
  MOZ_ASSERT(mFeatureWrCompositor);

  bool guarded = true;
  mFeatureWrQualified->EnableByDefault();

  if (mWrQualifiedOverride) {
    if (!*mWrQualifiedOverride) {
      mFeatureWrQualified->Disable(
          FeatureStatus::BlockedOverride, "HW qualification pref override",
          "FEATURE_FAILURE_WR_QUALIFICATION_OVERRIDE"_ns);
    }
    return guarded;
  }

  nsCString failureId;
  int32_t status;
  if (NS_FAILED(mGfxInfo->GetFeatureStatus(nsIGfxInfo::FEATURE_WEBRENDER,
                                           failureId, &status))) {
    mFeatureWrQualified->Disable(FeatureStatus::BlockedNoGfxInfo,
                                 "gfxInfo is broken",
                                 "FEATURE_FAILURE_WR_NO_GFX_INFO"_ns);
    return guarded;
  }

  switch (status) {
    case nsIGfxInfo::FEATURE_ALLOW_ALWAYS:
      // We want to honour ALLOW_ALWAYS on beta and release, but on nightly,
      // we still want to perform experiments. A larger population is the most
      // useful, demote nightly to merely qualified.
      guarded = mIsNightly;
      break;
    case nsIGfxInfo::FEATURE_ALLOW_QUALIFIED:
      break;
    case nsIGfxInfo::FEATURE_DENIED:
      mFeatureWrQualified->Disable(FeatureStatus::Denied, "Not on allowlist",
                                   failureId);
      break;
    default:
      mFeatureWrQualified->Disable(FeatureStatus::Blocklisted,
                                   "No qualified hardware", failureId);
      break;
    case nsIGfxInfo::FEATURE_STATUS_OK:
      MOZ_ASSERT_UNREACHABLE("We should still be rolling out WebRender!");
      mFeatureWrQualified->Disable(FeatureStatus::Blocked,
                                   "Not controlled by rollout", failureId);
      break;
  }

  if (!mIsNightly) {
    // Disable WebRender if we don't have DirectComposition
    nsAutoString adapterVendorID;
    mGfxInfo->GetAdapterVendorID(adapterVendorID);
    if (adapterVendorID == u"0x8086") {
      bool mixed;
      int32_t maxRefreshRate = mGfxInfo->GetMaxRefreshRate(&mixed);
      if (maxRefreshRate > 60) {
        mFeatureWrQualified->Disable(FeatureStatus::Blocked,
                                     "Monitor refresh rate too high",
                                     "REFRESH_RATE_TOO_HIGH"_ns);
      }
    } else if (adapterVendorID == u"0x10de") {
      bool mixed = false;
      int32_t maxRefreshRate = mGfxInfo->GetMaxRefreshRate(&mixed);
      if (maxRefreshRate > 60 && mixed) {
        mFeatureWrQualified->Disable(FeatureStatus::Blocked,
                                     "Monitor refresh rate too high/mixed",
                                     "NVIDIA_REFRESH_RATE_MIXED"_ns);
      }
    }
  }

  return guarded;
}

void gfxConfigManager::ConfigureWebRender() {
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(mFeatureWr);
  MOZ_ASSERT(mFeatureWrQualified);
  MOZ_ASSERT(mFeatureWrCompositor);
  MOZ_ASSERT(mFeatureWrAngle);
  MOZ_ASSERT(mFeatureWrDComp);
  MOZ_ASSERT(mFeatureWrPartial);
  MOZ_ASSERT(mFeatureHwCompositing);
  MOZ_ASSERT(mFeatureGPUProcess);

  // Initialize WebRender native compositor usage
  mFeatureWrCompositor->SetDefaultFromPref("gfx.webrender.compositor", true,
                                           false, mWrCompositorEnabled);

  if (mWrCompositorForceEnabled) {
    mFeatureWrCompositor->UserForceEnable("Force enabled by pref");
  }

  ConfigureFromBlocklist(nsIGfxInfo::FEATURE_WEBRENDER_COMPOSITOR,
                         mFeatureWrCompositor);

  // Disable native compositor when hardware stretching is not supported. It is
  // for avoiding a problem like Bug 1618370.
  // XXX Is there a better check for Bug 1618370?
  if (!mHwStretchingSupport.IsFullySupported() && mScaledResolution) {
    nsPrintfCString failureId(
        "FEATURE_FAILURE_NO_HARDWARE_STRETCHING_B%uW%uF%uN%uE%u",
        mHwStretchingSupport.mBoth, mHwStretchingSupport.mWindowOnly,
        mHwStretchingSupport.mFullScreenOnly, mHwStretchingSupport.mNone,
        mHwStretchingSupport.mError);
    mFeatureWrCompositor->Disable(FeatureStatus::Unavailable,
                                  "No hardware stretching support", failureId);
  }

  bool guardedByQualifiedPref = ConfigureWebRenderQualified();

  mFeatureWr->EnableByDefault();

  // envvar works everywhere; note that we need this for testing in CI.
  // Prior to bug 1523788, the `prefEnabled` check was only done on Nightly,
  // so as to prevent random users from easily enabling WebRender on
  // unqualified hardware in beta/release.
  if (mWrEnvForceEnabled) {
    mFeatureWr->UserForceEnable("Force enabled by envvar");
  } else if (mWrForceEnabled) {
    mFeatureWr->UserForceEnable("Force enabled by pref");
  } else if (mWrForceDisabled ||
             (mWrEnvForceDisabled && mWrQualifiedOverride.isNothing())) {
    // If the user set the pref to force-disable, let's do that. This
    // will override all the other enabling prefs
    // (gfx.webrender.enabled, gfx.webrender.all, and
    // gfx.webrender.all.qualified).
    mFeatureWr->UserDisable("User force-disabled WR",
                            "FEATURE_FAILURE_USER_FORCE_DISABLED"_ns);
  }

  if (!mFeatureWrQualified->IsEnabled()) {
    mFeatureWr->Disable(FeatureStatus::Disabled, "Not qualified",
                        "FEATURE_FAILURE_NOT_QUALIFIED"_ns);
  } else if (guardedByQualifiedPref && !mWrQualified) {
    // If the HW is qualified, we enable if either the HW has been qualified
    // on the release channel (i.e. it's no longer guarded by the qualified
    // pref), or if the qualified pref is enabled.
    mFeatureWr->Disable(FeatureStatus::Disabled, "Control group for experiment",
                        "FEATURE_FAILURE_IN_EXPERIMENT"_ns);
  }

  // HW_COMPOSITING being disabled implies interfacing with the GPU might break
  if (!mFeatureHwCompositing->IsEnabled() &&
      !StaticPrefs::gfx_webrender_software_AtStartup()) {
    mFeatureWr->ForceDisable(FeatureStatus::UnavailableNoHwCompositing,
                             "Hardware compositing is disabled",
                             "FEATURE_FAILURE_WEBRENDER_NEED_HWCOMP"_ns);
  }

  if (mSafeMode) {
    mFeatureWr->ForceDisable(FeatureStatus::UnavailableInSafeMode,
                             "Safe-mode is enabled",
                             "FEATURE_FAILURE_SAFE_MODE"_ns);
  }

  if (mXRenderEnabled) {
    // XRender and WebRender don't play well together. XRender is disabled by
    // default. If the user opts into it don't enable webrender.
    mFeatureWr->ForceDisable(FeatureStatus::Blocked, "XRender is enabled",
                             "FEATURE_FAILURE_XRENDER"_ns);
  }

  mFeatureWrAngle->EnableByDefault();
  if (mFeatureD3D11HwAngle) {
    if (mWrForceAngle) {
      if (!mFeatureD3D11HwAngle->IsEnabled()) {
        mFeatureWrAngle->ForceDisable(FeatureStatus::UnavailableNoAngle,
                                      "ANGLE is disabled",
                                      mFeatureD3D11HwAngle->GetFailureId());
      } else if (!mFeatureGPUProcess->IsEnabled() &&
                 (!mIsNightly || !mWrForceAngleNoGPUProcess)) {
        // WebRender with ANGLE relies on the GPU process when on Windows
        mFeatureWrAngle->ForceDisable(
            FeatureStatus::UnavailableNoGpuProcess, "GPU Process is disabled",
            "FEATURE_FAILURE_GPU_PROCESS_DISABLED"_ns);
      } else if (!mFeatureWr->IsEnabled()) {
        mFeatureWrAngle->ForceDisable(FeatureStatus::Unavailable,
                                      "WebRender disabled",
                                      "FEATURE_FAILURE_WR_DISABLED"_ns);
      }
    } else {
      mFeatureWrAngle->Disable(FeatureStatus::Disabled, "ANGLE is not forced",
                               "FEATURE_FAILURE_ANGLE_NOT_FORCED"_ns);
    }
  } else {
    mFeatureWrAngle->Disable(FeatureStatus::Unavailable, "OS not supported",
                             "FEATURE_FAILURE_OS_NOT_SUPPORTED"_ns);
  }

  if (mWrForceAngle && mFeatureWr->IsEnabled() &&
      !mFeatureWrAngle->IsEnabled()) {
    // Ensure we disable WebRender if ANGLE is unavailable and it is required.
    mFeatureWr->ForceDisable(FeatureStatus::UnavailableNoAngle,
                             "ANGLE is disabled",
                             mFeatureWrAngle->GetFailureId());
  }

  if (!mFeatureWr->IsEnabled() && mDisableHwCompositingNoWr) {
    if (mFeatureHwCompositing->IsEnabled()) {
      // Hardware compositing should be disabled by default if we aren't using
      // WebRender. We had to check if it is enabled at all, because it may
      // already have been forced disabled (e.g. safe mode, headless). It may
      // still be forced on by the user, and if so, this should have no effect.
      mFeatureHwCompositing->Disable(FeatureStatus::Blocked,
                                     "Acceleration blocked by platform", ""_ns);
    }

    if (!mFeatureHwCompositing->IsEnabled() &&
        mFeatureGPUProcess->IsEnabled() && !mGPUProcessAllowSoftware) {
      // We have neither WebRender nor OpenGL, we don't allow the GPU process
      // for basic compositor, and it wasn't disabled already.
      mFeatureGPUProcess->Disable(FeatureStatus::Unavailable,
                                  "Hardware compositing is unavailable.",
                                  ""_ns);
    }
  }

  mFeatureWrDComp->EnableByDefault();
  if (!mWrDCompWinEnabled) {
    mFeatureWrDComp->UserDisable("User disabled via pref",
                                 "FEATURE_FAILURE_DCOMP_PREF_DISABLED"_ns);
  }

  if (!mIsWin10OrLater) {
    // XXX relax win version to windows 8.
    mFeatureWrDComp->Disable(FeatureStatus::Unavailable,
                             "Requires Windows 10 or later",
                             "FEATURE_FAILURE_DCOMP_NOT_WIN10"_ns);
  }

  mFeatureWrDComp->MaybeSetFailed(
      mFeatureWr->IsEnabled(), FeatureStatus::Unavailable, "Requires WebRender",
      "FEATURE_FAILURE_DCOMP_NOT_WR"_ns);
  mFeatureWrDComp->MaybeSetFailed(mFeatureWrAngle->IsEnabled(),
                                  FeatureStatus::Unavailable, "Requires ANGLE",
                                  "FEATURE_FAILURE_DCOMP_NOT_ANGLE"_ns);

  if (!mFeatureWrDComp->IsEnabled() && mWrCompositorDCompRequired) {
    mFeatureWrCompositor->ForceDisable(FeatureStatus::Unavailable,
                                       "No DirectComposition usage",
                                       mFeatureWrDComp->GetFailureId());
  }

  // Initialize WebRender partial present config.
  // Partial present is used only when WebRender compositor is not used.
  if (mWrPartialPresent) {
    if (mFeatureWr->IsEnabled()) {
      mFeatureWrPartial->EnableByDefault();
    }
  }
}

}  // namespace gfx
}  // namespace mozilla
