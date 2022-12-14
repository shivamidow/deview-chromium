// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHARING_MOCK_SHARING_SERVICE_H_
#define CHROME_BROWSER_SHARING_MOCK_SHARING_SERVICE_H_

#include "chrome/browser/sharing/sharing_message_sender.h"
#include "chrome/browser/sharing/sharing_service.h"

#include "testing/gmock/include/gmock/gmock.h"

class MockSharingService : public SharingService {
 public:
  MockSharingService();
  ~MockSharingService() override;

  MOCK_CONST_METHOD1(
      GetDeviceCandidates,
      std::vector<std::unique_ptr<syncer::DeviceInfo>>(
          sync_pb::SharingSpecificFields::EnabledFeatures required_feature));

  MOCK_METHOD4(SendMessageToDevice,
               void(const std::string& device_guid,
                    base::TimeDelta response_timeout,
                    chrome_browser_sharing::SharingMessage message,
                    SharingMessageSender::ResponseCallback callback));

  MOCK_CONST_METHOD1(
      GetDeviceByGuid,
      std::unique_ptr<syncer::DeviceInfo>(const std::string& guid));

 private:
  DISALLOW_COPY_AND_ASSIGN(MockSharingService);
};

#endif  // CHROME_BROWSER_SHARING_MOCK_SHARING_SERVICE_H_
