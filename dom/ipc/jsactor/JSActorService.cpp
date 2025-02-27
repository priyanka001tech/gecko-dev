/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/JSActorService.h"
#include "mozilla/dom/ChromeUtilsBinding.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/BrowserParent.h"
#include "mozilla/dom/EventListenerBinding.h"
#include "mozilla/dom/Event.h"
#include "mozilla/dom/EventTargetBinding.h"
#include "mozilla/dom/EventTarget.h"
#include "mozilla/dom/InProcessChild.h"
#include "mozilla/dom/InProcessParent.h"
#include "mozilla/dom/JSActorManager.h"
#include "mozilla/dom/JSProcessActorBinding.h"
#include "mozilla/dom/JSProcessActorChild.h"
#include "mozilla/dom/JSProcessActorProtocol.h"
#include "mozilla/dom/JSWindowActorBinding.h"
#include "mozilla/dom/JSWindowActorChild.h"
#include "mozilla/dom/JSWindowActorProtocol.h"
#include "mozilla/dom/MessageManagerBinding.h"
#include "mozilla/dom/PContent.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/WindowGlobalChild.h"
#include "mozilla/dom/WindowGlobalParent.h"
#include "mozilla/ArrayAlgorithm.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/Logging.h"
#include "nsIObserverService.h"

namespace mozilla {
namespace dom {
namespace {
StaticRefPtr<JSActorService> gJSActorService;
}

JSActorService::JSActorService() { MOZ_ASSERT(NS_IsMainThread()); }

JSActorService::~JSActorService() { MOZ_ASSERT(NS_IsMainThread()); }

/* static */
already_AddRefed<JSActorService> JSActorService::GetSingleton() {
  MOZ_ASSERT(NS_IsMainThread());
  if (!gJSActorService) {
    gJSActorService = new JSActorService();
    ClearOnShutdown(&gJSActorService);
  }

  RefPtr<JSActorService> service = gJSActorService.get();
  return service.forget();
}

void JSActorService::RegisterWindowActor(const nsACString& aName,
                                         const WindowActorOptions& aOptions,
                                         ErrorResult& aRv) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(XRE_IsParentProcess());

  auto entry = mWindowActorDescriptors.LookupForAdd(aName);
  if (entry) {
    aRv.ThrowNotSupportedError(nsPrintfCString(
        "'%s' actor is already registered.", PromiseFlatCString(aName).get()));
    return;
  }

  // Insert a new entry for the protocol.
  RefPtr<JSWindowActorProtocol> proto =
      JSWindowActorProtocol::FromWebIDLOptions(aName, aOptions, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    entry.OrRemove();
    return;
  }

  entry.OrInsert([&] { return proto; });

  // Send information about the newly added entry to every existing content
  // process.
  AutoTArray<JSWindowActorInfo, 1> windowInfos{proto->ToIPC()};
  nsTArray<JSProcessActorInfo> contentInfos{};
  for (auto* cp : ContentParent::AllProcesses(ContentParent::eLive)) {
    Unused << cp->SendInitJSActorInfos(contentInfos, windowInfos);
  }

  // Register event listeners for any existing chrome targets.
  for (EventTarget* target : mChromeEventTargets) {
    proto->RegisterListenersFor(target);
  }

  // Add observers to the protocol.
  proto->AddObservers();
}

void JSActorService::UnregisterWindowActor(const nsACString& aName) {
  MOZ_ASSERT(nsContentUtils::IsSafeToRunScript());
  CrashReporter::AutoAnnotateCrashReport autoActorName(
      CrashReporter::Annotation::JSActorName, aName);
  CrashReporter::AutoAnnotateCrashReport autoMessageName(
      CrashReporter::Annotation::JSActorMessage, "<Unregister>"_ns);

  nsAutoCString name(aName);
  RefPtr<JSWindowActorProtocol> proto;
  if (mWindowActorDescriptors.Remove(name, getter_AddRefs(proto))) {
    // Remove listeners for this actor from each of our chrome targets.
    for (EventTarget* target : mChromeEventTargets) {
      proto->UnregisterListenersFor(target);
    }

    // Remove observers for this actor from observer serivce.
    proto->RemoveObservers();

    // Tell every content process to also unregister, and accumulate the set of
    // potential managers, to have the actor disabled.
    nsTArray<RefPtr<JSActorManager>> managers;
    if (XRE_IsParentProcess()) {
      for (auto* cp : ContentParent::AllProcesses(ContentParent::eLive)) {
        Unused << cp->SendUnregisterJSWindowActor(name);
        for (auto& bp : cp->ManagedPBrowserParent()) {
          for (auto& wgp : bp.GetKey()->ManagedPWindowGlobalParent()) {
            managers.AppendElement(
                static_cast<WindowGlobalParent*>(wgp.GetKey()));
          }
        }
      }

      for (auto& wgp :
           InProcessParent::Singleton()->ManagedPWindowGlobalParent()) {
        managers.AppendElement(static_cast<WindowGlobalParent*>(wgp.GetKey()));
      }
      for (auto& wgc :
           InProcessChild::Singleton()->ManagedPWindowGlobalChild()) {
        managers.AppendElement(static_cast<WindowGlobalChild*>(wgc.GetKey()));
      }
    } else {
      for (auto& bc : ContentChild::GetSingleton()->ManagedPBrowserChild()) {
        for (auto& wgc : bc.GetKey()->ManagedPWindowGlobalChild()) {
          managers.AppendElement(static_cast<WindowGlobalChild*>(wgc.GetKey()));
        }
      }
    }

    for (auto& mgr : managers) {
      mgr->JSActorUnregister(name);
    }
  }
}

void JSActorService::LoadJSActorInfos(nsTArray<JSProcessActorInfo>& aProcess,
                                      nsTArray<JSWindowActorInfo>& aWindow) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(XRE_IsContentProcess());

  for (auto& info : aProcess) {
    // Create our JSProcessActorProtocol, register it in
    // mProcessActorDescriptors.
    auto name = info.name();
    RefPtr<JSProcessActorProtocol> proto =
        JSProcessActorProtocol::FromIPC(std::move(info));
    mProcessActorDescriptors.Put(std::move(name), RefPtr{proto});

    // Add observers for each actor.
    proto->AddObservers();
  }

  for (auto& info : aWindow) {
    auto name = info.name();
    RefPtr<JSWindowActorProtocol> proto =
        JSWindowActorProtocol::FromIPC(std::move(info));
    mWindowActorDescriptors.Put(std::move(name), RefPtr{proto});

    // Register listeners for each chrome target.
    for (EventTarget* target : mChromeEventTargets) {
      proto->RegisterListenersFor(target);
    }

    // Add observers for each actor.
    proto->AddObservers();
  }
}

void JSActorService::GetJSWindowActorInfos(
    nsTArray<JSWindowActorInfo>& aInfos) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(XRE_IsParentProcess());

  for (auto iter = mWindowActorDescriptors.ConstIter(); !iter.Done();
       iter.Next()) {
    aInfos.AppendElement(iter.Data()->ToIPC());
  }
}

void JSActorService::RegisterChromeEventTarget(EventTarget* aTarget) {
  MOZ_ASSERT(!mChromeEventTargets.Contains(aTarget));
  mChromeEventTargets.AppendElement(aTarget);

  // Register event listeners on the newly added Window Root.
  for (auto iter = mWindowActorDescriptors.Iter(); !iter.Done(); iter.Next()) {
    iter.Data()->RegisterListenersFor(aTarget);
  }

  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  obs->NotifyObservers(aTarget, "chrome-event-target-created", nullptr);
}

/* static */
void JSActorService::UnregisterChromeEventTarget(EventTarget* aTarget) {
  if (gJSActorService) {
    // NOTE: No need to unregister listeners here, as the target is going away.
    gJSActorService->mChromeEventTargets.RemoveElement(aTarget);
  }
}

void JSActorService::RegisterProcessActor(const nsACString& aName,
                                          const ProcessActorOptions& aOptions,
                                          ErrorResult& aRv) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(XRE_IsParentProcess());

  auto entry = mProcessActorDescriptors.LookupForAdd(aName);
  if (entry) {
    aRv.ThrowNotSupportedError(nsPrintfCString(
        "'%s' actor is already registered.", PromiseFlatCString(aName).get()));
    return;
  }

  // Insert a new entry for the protocol.
  RefPtr<JSProcessActorProtocol> proto =
      JSProcessActorProtocol::FromWebIDLOptions(aName, aOptions, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    entry.OrRemove();
    return;
  }

  entry.OrInsert([&] { return proto; });

  // Send information about the newly added entry to every existing content
  // process.
  AutoTArray<JSProcessActorInfo, 1> contentInfos{proto->ToIPC()};
  nsTArray<JSWindowActorInfo> windowInfos{};
  for (auto* cp : ContentParent::AllProcesses(ContentParent::eLive)) {
    Unused << cp->SendInitJSActorInfos(contentInfos, windowInfos);
  }

  // Add observers to the protocol.
  proto->AddObservers();
}

void JSActorService::UnregisterProcessActor(const nsACString& aName) {
  MOZ_ASSERT(nsContentUtils::IsSafeToRunScript());
  CrashReporter::AutoAnnotateCrashReport autoActorName(
      CrashReporter::Annotation::JSActorName, aName);
  CrashReporter::AutoAnnotateCrashReport autoMessageName(
      CrashReporter::Annotation::JSActorMessage, "<Unregister>"_ns);

  nsAutoCString name(aName);
  RefPtr<JSProcessActorProtocol> proto;
  if (mProcessActorDescriptors.Remove(name, getter_AddRefs(proto))) {
    // Remove observers for this actor from observer serivce.
    proto->RemoveObservers();

    // Tell every content process to also unregister, and accumulate the set of
    // potential managers, to have the actor disabled.
    nsTArray<RefPtr<JSActorManager>> managers;
    if (XRE_IsParentProcess()) {
      for (auto* cp : ContentParent::AllProcesses(ContentParent::eLive)) {
        Unused << cp->SendUnregisterJSProcessActor(name);
        managers.AppendElement(cp);
      }
      managers.AppendElement(InProcessChild::Singleton());
      managers.AppendElement(InProcessParent::Singleton());
    } else {
      managers.AppendElement(ContentChild::GetSingleton());
    }

    for (auto& mgr : managers) {
      mgr->JSActorUnregister(name);
    }
  }
}

void JSActorService::GetJSProcessActorInfos(
    nsTArray<JSProcessActorInfo>& aInfos) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(XRE_IsParentProcess());

  for (auto iter = mProcessActorDescriptors.ConstIter(); !iter.Done();
       iter.Next()) {
    aInfos.AppendElement(iter.Data()->ToIPC());
  }
}

already_AddRefed<JSProcessActorProtocol>
JSActorService::GetJSProcessActorProtocol(const nsACString& aName) {
  return mProcessActorDescriptors.Get(aName);
}

already_AddRefed<JSWindowActorProtocol>
JSActorService::GetJSWindowActorProtocol(const nsACString& aName) {
  return mWindowActorDescriptors.Get(aName);
}

}  // namespace dom
}  // namespace mozilla
