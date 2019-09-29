/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.provider.settings.validators;

import android.annotation.Nullable;

/**
 * Validate the elements in a list.
 *
 * @hide
 */
abstract class ListValidator implements Validator {

    private String mListSplitRegex;

    ListValidator(String listSplitRegex) {
        mListSplitRegex = listSplitRegex;
    }

    public boolean validate(@Nullable String value) {
        if (!isEntryValid(value)) {
            return false;
        }
        String[] items = value.split(",");
        for (String item : items) {
            if (!isItemValid(item)) {
                return false;
            }
        }
        return true;
    }

    protected abstract boolean isEntryValid(String entry);

    protected abstract boolean isItemValid(String item);
}

