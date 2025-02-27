/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "NSSErrorsService.h"
#include "mozilla/Telemetry.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/dom/WindowGlobalParent.h"
#include "mozpkix/pkixnss.h"
#include "nsCOMPtr.h"
#include "nsHTTPSOnlyStreamListener.h"
#include "nsHTTPSOnlyUtils.h"
#include "nsIChannel.h"
#include "nsIRequest.h"
#include "nsITransportSecurityInfo.h"
#include "nsIURI.h"
#include "nsPrintfCString.h"
#include "secerr.h"
#include "sslerr.h"

NS_IMPL_ISUPPORTS(nsHTTPSOnlyStreamListener, nsIStreamListener,
                  nsIRequestObserver)

nsHTTPSOnlyStreamListener::nsHTTPSOnlyStreamListener(
    nsIStreamListener* aListener, nsILoadInfo* aLoadInfo)
    : mListener(aListener), mCreationStart(mozilla::TimeStamp::Now()) {
  RefPtr<mozilla::dom::WindowGlobalParent> wgp =
      mozilla::dom::WindowGlobalParent::GetByInnerWindowId(
          aLoadInfo->GetInnerWindowID());
  // For Top-level document loads (which don't have a requesting window-context)
  // we compute these flags once we create the Document in nsSecureBrowserUI.
  if (wgp) {
    wgp->TopWindowContext()->AddSecurityState(
        nsIWebProgressListener::STATE_HTTPS_ONLY_MODE_UPGRADED);
  }
}

NS_IMETHODIMP
nsHTTPSOnlyStreamListener::OnDataAvailable(nsIRequest* aRequest,
                                           nsIInputStream* aInputStream,
                                           uint64_t aOffset, uint32_t aCount) {
  return mListener->OnDataAvailable(aRequest, aInputStream, aOffset, aCount);
}

NS_IMETHODIMP
nsHTTPSOnlyStreamListener::OnStartRequest(nsIRequest* request) {
  return mListener->OnStartRequest(request);
}

NS_IMETHODIMP
nsHTTPSOnlyStreamListener::OnStopRequest(nsIRequest* request,
                                         nsresult aStatus) {
  nsCOMPtr<nsIChannel> channel = do_QueryInterface(request);

  // Note: CouldBeHttpsOnlyError also returns true if there was no error
  if (nsHTTPSOnlyUtils::CouldBeHttpsOnlyError(channel, aStatus)) {
    RecordUpgradeTelemetry(request, aStatus);
    LogUpgradeFailure(request, aStatus);

    // If the request failed and there is a requesting window-context, set
    // HTTPS-Only state flag to indicate a failed upgrade.
    // For Top-level document loads (which don't have a requesting
    // window-context) we simply check in the UI code whether we landed on the
    // HTTPS-Only error page.
    if (NS_FAILED(aStatus)) {
      nsCOMPtr<nsILoadInfo> loadInfo = channel->LoadInfo();
      RefPtr<mozilla::dom::WindowGlobalParent> wgp =
          mozilla::dom::WindowGlobalParent::GetByInnerWindowId(
              loadInfo->GetInnerWindowID());

      if (wgp) {
        wgp->TopWindowContext()->AddSecurityState(
            nsIWebProgressListener::STATE_HTTPS_ONLY_MODE_UPGRADE_FAILED);
      }
    }
  }

  return mListener->OnStopRequest(request, aStatus);
}

void nsHTTPSOnlyStreamListener::RecordUpgradeTelemetry(nsIRequest* request,
                                                       nsresult aStatus) {
  // 1. Get time between now and when the initial upgrade request started
  int64_t duration =
      (mozilla::TimeStamp::Now() - mCreationStart).ToMilliseconds();

  // 2. Assemble the category string
  // [!] All strings have to be present in Histograms.json
  nsresult rv;
  nsCOMPtr<nsIChannel> channel = do_QueryInterface(request, &rv);
  if (NS_FAILED(rv)) {
    return;
  }

  nsAutoCString category;
  nsCOMPtr<nsILoadInfo> loadInfo = channel->LoadInfo();

  if (loadInfo->InternalContentPolicyType() ==
      nsIContentPolicy::TYPE_DOCUMENT) {
    category.AppendLiteral("top_");
  } else {
    category.AppendLiteral("sub_");
  }

  if (NS_SUCCEEDED(aStatus)) {
    category.AppendLiteral("successful");
  } else {
    int32_t code = -1 * NS_ERROR_GET_CODE(aStatus);

    if (aStatus == NS_ERROR_REDIRECT_LOOP) {
      category.AppendLiteral("f_redirectloop");
    } else if (aStatus == NS_ERROR_NET_TIMEOUT) {
      category.AppendLiteral("f_timeout");
    } else if (aStatus == NS_BINDING_ABORTED) {
      category.AppendLiteral("f_aborted");
    } else if (aStatus == NS_ERROR_CONNECTION_REFUSED) {
      category.AppendLiteral("f_cxnrefused");
    } else if (mozilla::psm::IsNSSErrorCode(code)) {
      switch (code) {
        case mozilla::pkix::MOZILLA_PKIX_ERROR_SELF_SIGNED_CERT:
          category.AppendLiteral("f_ssl_selfsignd");
          break;
        case SSL_ERROR_BAD_CERT_DOMAIN:
          category.AppendLiteral("f_ssl_badcertdm");
          break;
        case SEC_ERROR_UNKNOWN_ISSUER:
          category.AppendLiteral("f_ssl_unkwnissr");
          break;
        default:
          category.AppendLiteral("f_ssl_other");
          break;
      }
    } else {
      category.AppendLiteral("f_other");
    }
  }

  mozilla::Telemetry::Accumulate(
      mozilla::Telemetry::HTTPS_ONLY_MODE_UPGRADE_TIME_MS, category, duration);
}

void nsHTTPSOnlyStreamListener::LogUpgradeFailure(nsIRequest* request,
                                                  nsresult aStatus) {
  // If the request failed we'll log it to the console with the error-code
  if (NS_SUCCEEDED(aStatus)) {
    return;
  }
  nsresult rv;
  // Try to query for the channel-object
  nsCOMPtr<nsIChannel> channel = do_QueryInterface(request, &rv);
  if (NS_FAILED(rv)) {
    return;
  }

  nsCOMPtr<nsIURI> uri;
  rv = channel->GetURI(getter_AddRefs(uri));
  if (NS_FAILED(rv)) {
    return;
  }
  // Logging URI as well as Module- and Error-Code
  AutoTArray<nsString, 2> params = {
      NS_ConvertUTF8toUTF16(uri->GetSpecOrDefault()),
      NS_ConvertUTF8toUTF16(nsPrintfCString("M%u-C%u",
                                            NS_ERROR_GET_MODULE(aStatus),
                                            NS_ERROR_GET_CODE(aStatus)))};

  nsCOMPtr<nsILoadInfo> loadInfo = channel->LoadInfo();
  nsHTTPSOnlyUtils::LogLocalizedString("HTTPSOnlyFailedRequest", params,
                                       nsIScriptError::errorFlag, loadInfo,
                                       uri);
}
