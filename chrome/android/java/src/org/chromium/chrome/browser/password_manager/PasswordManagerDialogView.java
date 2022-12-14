// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.password_manager;

import android.content.Context;
import android.util.AttributeSet;
import android.widget.ImageView;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.annotation.Nullable;

import org.chromium.chrome.R;
import org.chromium.ui.widget.ChromeImageButton;

/**
 * The dialog content view for illustration dialogs used by the password manager (e.g. onboarding,
 * leak warning).
 */
public class PasswordManagerDialogView extends ScrollView {
    private @Nullable ChromeImageButton mHelpButtonView;
    private @Nullable ChromeImageButton mInlineHelpButtonView;
    private ImageView mIllustrationView;
    private TextView mTitleView;
    private TextView mDetailsView;

    public PasswordManagerDialogView(Context context, @Nullable AttributeSet attrs) {
        super(context, attrs);
    }

    @Override
    protected void onFinishInflate() {
        super.onFinishInflate();
        mHelpButtonView = findViewById(R.id.password_dialog_help_button);
        mInlineHelpButtonView = findViewById(R.id.password_dialog_inline_help_button);
        mIllustrationView = findViewById(R.id.password_manager_dialog_illustration);
        mTitleView = findViewById(R.id.password_manager_dialog_title);
        mDetailsView = findViewById(R.id.password_manager_dialog_details);
    }

    void addHelpButton(Runnable callback) {
        if (mHelpButtonView == null) return;
        mHelpButtonView.setOnClickListener(view -> callback.run());
        mInlineHelpButtonView.setOnClickListener(view -> callback.run());
        mHelpButtonView.setVisibility(VISIBLE);
    }

    void setIllustration(int illustration) {
        mIllustrationView.setImageResource(illustration);
    }

    public void updateIllustrationVisibility(boolean illustrationVisible) {
        if (illustrationVisible) {
            mIllustrationView.setVisibility(VISIBLE);
        } else {
            mIllustrationView.setVisibility(GONE);
        }
    }

    public void updateHelpIcon(boolean usesInlineIcon) {
        // There is no help button to update.
        if (mHelpButtonView == null) return;

        mHelpButtonView.setVisibility(usesInlineIcon ? GONE : VISIBLE);
        mInlineHelpButtonView.setVisibility(usesInlineIcon ? VISIBLE : GONE);
    }

    void setTitle(String title) {
        mTitleView.setText(title);
    }

    void setDetails(String details) {
        mDetailsView.setText(details);
    }
}
