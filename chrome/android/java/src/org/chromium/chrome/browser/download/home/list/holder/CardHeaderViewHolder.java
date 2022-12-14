// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.download.home.list.holder;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import org.chromium.chrome.browser.download.home.list.ListItem;
import org.chromium.chrome.download.R;
import org.chromium.ui.modelutil.PropertyModel;

/**
 * A {@link RecyclerView.ViewHolder} specifically meant to display a card header.
 */
public class CardHeaderViewHolder extends ListItemViewHolder {
    /**
     * Creates a new {@link CardHeaderViewHolder} instance.
     */
    public static CardHeaderViewHolder create(ViewGroup parent) {
        View view = LayoutInflater.from(parent.getContext())
                            .inflate(R.layout.download_manager_card_header, null);
        return new CardHeaderViewHolder(view);
    }

    public CardHeaderViewHolder(View view) {
        super(view);
    }

    @Override
    public void bind(PropertyModel properties, ListItem item) {
        ListItem.CardHeaderListItem headerListItem = (ListItem.CardHeaderListItem) item;
        TextView domainView = itemView.findViewById(R.id.domain);
        domainView.setText(headerListItem.dateAndDomain.second);
    }
}
