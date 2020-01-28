// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains forward declarations for items in later SDKs than the
// default one with which Chromium is built (currently 10.10).
// If you call any function from this header, be sure to check at runtime for
// respondsToSelector: before calling these functions (else your code will crash
// on older OS X versions that chrome still supports).

#ifndef BASE_MAC_SDK_FORWARD_DECLARATIONS_H_
#define BASE_MAC_SDK_FORWARD_DECLARATIONS_H_

#import <AppKit/AppKit.h>
#include <AvailabilityMacros.h>
#import <CoreBluetooth/CoreBluetooth.h>
#import <CoreWLAN/CoreWLAN.h>
#import <IOBluetooth/IOBluetooth.h>
#import <ImageCaptureCore/ImageCaptureCore.h>
#import <QuartzCore/QuartzCore.h>
#include <stdint.h>

#include "base/base_export.h"

// ----------------------------------------------------------------------------
// Define typedefs, enums, and protocols not available in the version of the
// OSX SDK being compiled against.
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// Define NSStrings only available in newer versions of the OSX SDK to force
// them to be statically linked.
// ----------------------------------------------------------------------------

extern "C" {
#if !defined(MAC_OS_X_VERSION_10_11) || \
    MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_11
BASE_EXPORT extern NSString* const CIDetectorTypeText;
#endif  // MAC_OS_X_VERSION_10_11
}  // extern "C"

#if !defined(MAC_OS_X_VERSION_10_15) || \
    MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_15

@interface NSScreen (ForwardDeclare)
@property(readonly)
    CGFloat maximumPotentialExtendedDynamicRangeColorComponentValue;
@end

@interface SFUniversalLink : NSObject
- (instancetype)initWithWebpageURL:(NSURL*)url;
@property(readonly) NSURL* webpageURL;
@property(readonly) NSURL* applicationURL;
@property(getter=isEnabled) BOOL enabled;
@end

#endif  // MAC_OS_X_VERSION_10_15

// ----------------------------------------------------------------------------
// The symbol for kCWSSIDDidChangeNotification is available in the
// CoreWLAN.framework for OSX versions 10.6 through 10.10. The symbol is not
// declared in the OSX 10.9+ SDK, so when compiling against an OSX 10.9+ SDK,
// declare the symbol.
// ----------------------------------------------------------------------------
BASE_EXPORT extern "C" NSString* const kCWSSIDDidChangeNotification;

#endif  // BASE_MAC_SDK_FORWARD_DECLARATIONS_H_
