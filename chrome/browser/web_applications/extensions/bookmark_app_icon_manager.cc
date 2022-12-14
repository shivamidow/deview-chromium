// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/web_applications/extensions/bookmark_app_icon_manager.h"

#include "base/logging.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/web_applications/extensions/bookmark_app_registrar.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/browser/image_loader.h"
#include "extensions/common/extension_icon_set.h"
#include "extensions/common/manifest_handlers/icons_handler.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/image/image.h"

namespace extensions {

namespace {

void OnExtensionIconLoaded(BookmarkAppIconManager::ReadIconCallback callback,
                           const gfx::Image& image) {
  std::move(callback).Run(image.IsEmpty() ? SkBitmap() : *image.ToSkBitmap());
}

const Extension* GetBookmarkApp(Profile* profile,
                                const web_app::AppId& app_id) {
  const Extension* extension =
      ExtensionRegistry::Get(profile)->enabled_extensions().GetByID(app_id);
  return (extension && extension->from_bookmark()) ? extension : nullptr;
}

void ReadExtensionIcon(Profile* profile,
                       const web_app::AppId& app_id,
                       int icon_size_in_px,
                       ExtensionIconSet::MatchType match_type,
                       BookmarkAppIconManager::ReadIconCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const Extension* extension = GetBookmarkApp(profile, app_id);
  DCHECK(extension);

  ImageLoader* loader = ImageLoader::Get(profile);
  loader->LoadImageAsync(
      extension,
      IconsInfo::GetIconResource(extension, icon_size_in_px, match_type),
      gfx::Size(icon_size_in_px, icon_size_in_px),
      base::BindOnce(&OnExtensionIconLoaded, std::move(callback)));
}

}  // anonymous namespace

BookmarkAppIconManager::BookmarkAppIconManager(Profile* profile)
    : profile_(profile) {}

BookmarkAppIconManager::~BookmarkAppIconManager() = default;

bool BookmarkAppIconManager::HasIcon(const web_app::AppId& app_id,
                                     int icon_size_in_px) const {
  return GetBookmarkApp(profile_, app_id);
}

bool BookmarkAppIconManager::HasSmallestIcon(const web_app::AppId& app_id,
                                             int icon_size_in_px) const {
  return GetBookmarkApp(profile_, app_id);
}

void BookmarkAppIconManager::ReadIcon(const web_app::AppId& app_id,
                                      int icon_size_in_px,
                                      ReadIconCallback callback) const {
  DCHECK(HasIcon(app_id, icon_size_in_px));
  ReadExtensionIcon(profile_, app_id, icon_size_in_px,
                    ExtensionIconSet::MATCH_EXACTLY, std::move(callback));
}

void BookmarkAppIconManager::ReadSmallestIcon(const web_app::AppId& app_id,
                                              int icon_size_in_px,
                                              ReadIconCallback callback) const {
  DCHECK(HasSmallestIcon(app_id, icon_size_in_px));
  ReadExtensionIcon(profile_, app_id, icon_size_in_px,
                    ExtensionIconSet::MATCH_BIGGER, std::move(callback));
}

void BookmarkAppIconManager::ReadSmallestCompressedIcon(
    const web_app::AppId& app_id,
    int icon_size_in_px,
    ReadCompressedIconCallback callback) const {
  NOTIMPLEMENTED();
  DCHECK(HasSmallestIcon(app_id, icon_size_in_px));
  std::move(callback).Run(std::vector<uint8_t>());
}

}  // namespace extensions
